/*
 * temphook.c - 64-bit file position hook for Newcolor's IDHTempOutput
 *
 * Newcolor captures scans through libtiff, handing it I/O callbacks via
 * TIFFClientOpen. Those callbacks are 32-bit because libtiff 3.x defines
 * toff_t as uint32:
 *
 *     GetFileSize(h, NULL)                        low 32 bits only
 *     SetFilePointer(h, dist, NULL, method)       32-bit, signed distance
 *
 * Past 4 GB the size wraps and the seek fails, and IDHTempOutput.cpp:581
 * reports "not enough space" while the disk is nearly empty. Observed: a
 * capture stopping at 4,299,505,794 bytes with 15.3 GB free.
 *
 * The pixel stream itself is fine - WriteFile is called with
 * lpOverlapped=NULL, so the kernel appends at its own 64-bit position and
 * has no size limit. Only the bookkeeping breaks.
 *
 * This hook redirects those three imports in IDHTempOutput.idh:
 *
 *   Mode 1 (log)  forward everything unchanged, record what happens.
 *                 Answers the one question static analysis cannot: past
 *                 4 GB, does libtiff stay append-only, or does it seek
 *                 backward to a wrapped offset and overwrite pixels?
 *
 *   Mode 2 (fix)  reinterpret the offset as unsigned and seek with a real
 *                 64-bit value. Exact for everything below 4 GiB, which is
 *                 the whole addressable range of a classic TIFF.
 *
 * Verified against real libtiff 3.9.7 driven through the same
 * TIFFClientOpen callback design:
 *
 *   2.6 GB, Win32 32-bit callbacks : all pixels written, seek to
 *       2,599,941,368 fails ERROR_NEGATIVE_SEEK, no directory written.
 *       Reproduces the observed Newcolor failure exactly.
 *   2.6 GB, corrected callbacks    : complete valid TIFF, 0 failed seeks,
 *       IFD at 2,599,941,242 with all tags correct.
 *
 * Above 4 GiB nothing here helps: a classic TIFF header holds a 32-bit
 * pointer to its directory, and no value in 32 bits can address past
 * 2^32. libtiff still writes a correct IFD at the true position, and the
 * pixel data stays intact, but the pointer to it is unwritable. Recover
 * those with bigtiff_newcolor.py.
 */

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern void lg(const char *fmt, ...);      /* from hdsti_spti.c */

static int   th_mode;                      /* 0 off, 1 log, 2 fix */
/*
 * Offset at or above which the corrected 64-bit path is taken. The real
 * threshold is 2 GiB - below that the shipped 32-bit code is already
 * correct. Lowering it forces the same code path on a small scan, so the
 * plumbing can be exercised in seconds instead of an hour.
 *
 * It does NOT exercise the large-number arithmetic: on a small file the
 * offsets are small. It tests that the hook installs, that the IAT patch
 * takes, that SetFilePointerEx substitution works inside Newcolor, and
 * that libtiff accepts what we hand back. That is where nearly all the
 * failure modes live.
 */
static LONGLONG th_threshold = 0x80000000LL;

/*
 * Mode 3 adds the >4 GiB sidecar on top of mode 2.
 *
 * libtiff 3.x holds every offset in a uint32. Past 4 GiB the values it
 * records wrap - including the one TIFFWriteDirectory seeks to in order to
 * write the directory. Left alone that lands mid-file and overwrites pixel
 * data while reporting success. Verified against real libtiff 3.9.7: a
 * 4.6 GB write produced a file of exactly header+pixels, with a ten-entry
 * IFD sitting on top of the image at wrapped offset 305,015,902.
 *
 * The fix is to translate, on both write and read, from RECORDED truth:
 * every time SEEK_END yields a position at or above 4 GiB we store the
 * (wrapped, true) pair. A SEEK_SET whose value matches a recorded wrapped
 * value is redirected to the true one. Nothing is inferred - un-wrapping
 * arithmetically guesses wrong on small legitimate offsets such as the
 * 4-byte header pointer.
 *
 * The table is written beside the file as <name>.hdsti64 so the reader,
 * which is a different module and a different handle, can load it.
 */
#define SIDECAR_EXT ".hdsti64"
#define SIDECAR_MAGIC "HDSTI64"

/*
 * Offset at or above which a (wrapped, true) pair is recorded. The real
 * value is 4 GiB - below that a 32-bit offset is not ambiguous and there is
 * nothing to translate.
 *
 * Lowering it forces the sidecar machinery to run on a small scan: pairs
 * get recorded, the file is written on close, the reader loads it and looks
 * values up. Below 4 GiB the wrapped value equals the true one, so the
 * translation itself is a no-op - but everything around it is exercised,
 * which is where the untested assumptions live (CreateFileA vs CreateFileW,
 * whether the reader opens read-only, whether writer and reader agree on
 * the sidecar name).
 */
static LONGLONG th_wrapat = 0x100000000LL;
static int th_sidecar_files = 0;   /* also write .hdsti64 files (debug only) */
static int cfg_bigtiff = 0;        /* write the SAVED tiff as BigTIFF */

/*
 * Newcolor RENAMES the temp file between writing and reading it: the writer
 * produces _finescan.tmp, the reader opens TImage<id>.tmp. A sidecar named
 * after the writer's path is therefore never found.
 *
 * Writer and reader are the same process, so the table does not need to
 * touch disk at all. On close-after-write it goes into this registry keyed
 * by final size; on open-for-read we look the size up. That survives the
 * rename, leaves no files behind, and cannot go stale across runs.
 */
#define MAXREG 8
static struct {
    LONGLONG   size;
    struct wt *map;
    int        n;
} g_reg[MAXREG];

static FILE *th_log;
static CRITICAL_SECTION th_lock;

/* ---- per-handle true position ---------------------------------------- */
#define MAXH 32
static struct {
    HANDLE    h;
    LONGLONG  pos;        /* our idea of the real file position */
    LONGLONG  high_water;  /* largest position reached */
    long      writes, seeks, sizes, backseeks;
    int       warned4g;
    long      fixed_seeks;
    long      reads;
    char      name[MAX_PATH];      /* from CreateFileA, to name the sidecar */
    struct wt { DWORD wrapped; LONGLONG truth; } *map;
    int       nmap, capmap;
    int       is_reader;
    int       owner;           /* MOD_* that opened it, -1 unknown */
    int       padded;          /* header write already padded to 16 */
    long      translated;
} g_h[MAXH];

/* lookup that does NOT allocate - CloseHandle sees every handle in the
   process, not just our files, and must not consume table entries. */
static int find_slot(HANDLE h)
{
    int i;
    for (i = 0; i < MAXH; i++) if (g_h[i].h == h) return i;
    return -1;
}

static int slot(HANDLE h)
{
    int i, free_i = -1;
    for (i = 0; i < MAXH; i++) {
        if (g_h[i].h == h) return i;
        if (!g_h[i].h && free_i < 0) free_i = i;
    }
    if (free_i < 0) return -1;
    memset(&g_h[free_i], 0, sizeof(g_h[0]));
    g_h[free_i].h = h;
    return free_i;
}

static void tlog(const char *fmt, ...);

/*
 * Which module a call came from decides what we may do to the file.
 * IDHTiffOutput writes the SAVED tiff, which nothing inside Newcolor reads
 * back, so it can be converted to BigTIFF. The two temp modules must be left
 * alone: padding a temp header by 8 bytes shifts every strip, and
 * IDHTempInput would read garbage.
 *
 * The caller is identified by return address, which lands in the calling
 * module's .text. Our functions are reached through a patched IAT slot, so
 * they cannot be inlined away.
 */
#define MOD_TEMPOUT 0
#define MOD_TEMPIN  1
#define MOD_TIFFOUT 2
static struct { HMODULE base; SIZE_T size; } g_modrange[3];

static int which_module(void *ra)
{
    int k;
    for (k = 0; k < 3; k++) {
        if (!g_modrange[k].base) continue;
        if ((BYTE *)ra >= (BYTE *)g_modrange[k].base &&
            (BYTE *)ra <  (BYTE *)g_modrange[k].base + g_modrange[k].size)
            return k;
    }
    return -1;
}

/*
 * Size from the module's own PE header. An HMODULE is its base address, so
 * SizeOfImage is a few dereferences away and needs no psapi.
 *
 * A default would be actively dangerous here: these modules load close
 * together - IDHTempInput at 0x06980000 and IDHTempOutput at 0x069C0000 in
 * one observed run, 256 KB apart - so an assumed size of any size would
 * overlap them and attribute calls to the wrong module. If the header cannot
 * be read we record no range at all and which_module returns -1, which
 * disables the padding rather than applying it to the wrong file.
 */
static void note_module(int k, HMODULE m)
{
    IMAGE_DOS_HEADER *dos = (IMAGE_DOS_HEADER *)m;
    IMAGE_NT_HEADERS *nt;
    if (k < 0 || k > 2 || !m) return;
    g_modrange[k].base = NULL;
    g_modrange[k].size = 0;
    if (IsBadReadPtr(dos, sizeof(*dos)) || dos->e_magic != IMAGE_DOS_SIGNATURE) {
        tlog("note_module: %p has no DOS header - range not recorded", m);
        return;
    }
    nt = (IMAGE_NT_HEADERS *)((BYTE *)m + dos->e_lfanew);
    if (IsBadReadPtr(nt, sizeof(*nt)) || nt->Signature != IMAGE_NT_SIGNATURE) {
        tlog("note_module: %p has no NT header - range not recorded", m);
        return;
    }
    g_modrange[k].base = m;
    g_modrange[k].size = nt->OptionalHeader.SizeOfImage;
}

static void map_record(int i, LONGLONG truth)
{
    struct wt *m;
    if (i < 0 || truth < th_wrapat) return;
    if (g_h[i].nmap && g_h[i].map[g_h[i].nmap-1].truth == truth) return;
    if (g_h[i].nmap >= g_h[i].capmap) {
        int cap = g_h[i].capmap ? g_h[i].capmap * 2 : 256;
        m = (struct wt *)realloc(g_h[i].map, cap * sizeof(*m));
        if (!m) return;
        g_h[i].map = m; g_h[i].capmap = cap;
    }
    g_h[i].map[g_h[i].nmap].wrapped = (DWORD)(truth & 0xFFFFFFFF);
    g_h[i].map[g_h[i].nmap].truth   = truth;
    g_h[i].nmap++;
}

/* exact match only; where several true offsets share a wrapped value take
   the one nearest the current position. No match -> return unchanged. */
static LONGLONG map_translate(int i, DWORD want, LONGLONG now, int *hit)
{
    int k, best = -1;
    LONGLONG d, bd = 0x7FFFFFFFFFFFFFFFLL;
    *hit = 0;
    if (i < 0) return (LONGLONG)(ULONGLONG)want;
    for (k = 0; k < g_h[i].nmap; k++) {
        if (g_h[i].map[k].wrapped != want) continue;
        d = g_h[i].map[k].truth - now;
        if (d < 0) d = -d;
        if (d < bd) { bd = d; best = k; }
    }
    if (best < 0) return (LONGLONG)(ULONGLONG)want;
    *hit = 1;
    return g_h[i].map[best].truth;
}

static void reg_put(LONGLONG size, struct wt *map, int n)
{
    int i, victim = 0;
    if (!map || n <= 0 || size <= 0) return;
    for (i = 0; i < MAXREG; i++) {
        if (g_reg[i].size == size) { victim = i; goto fill; }   /* replace */
        if (!g_reg[i].map)         { victim = i; goto fill; }
    }
    victim = 0;                                                  /* evict oldest */
fill:
    if (g_reg[victim].map) free(g_reg[victim].map);
    g_reg[victim].map = (struct wt *)malloc(n * sizeof(struct wt));
    if (!g_reg[victim].map) { g_reg[victim].size = 0; g_reg[victim].n = 0; return; }
    memcpy(g_reg[victim].map, map, n * sizeof(struct wt));
    g_reg[victim].size = size;
    g_reg[victim].n = n;
}

static int reg_get(LONGLONG size, struct wt **out)
{
    int i, found = -1, dupes = 0;
    for (i = 0; i < MAXREG; i++)
        if (g_reg[i].map && g_reg[i].size == size) { dupes++; found = i; }
    if (found < 0) return 0;
    if (dupes > 1) tlog("registry: %d tables share size %lld - using the last",
                        dupes, size);
    *out = g_reg[found].map;
    return g_reg[found].n;
}

static void sidecar_path(const char *file, char *out)
{
    lstrcpynA(out, file, MAX_PATH - (int)sizeof(SIDECAR_EXT) - 1);
    lstrcatA(out, SIDECAR_EXT);
}

static void sidecar_save(int i)
{
    char p[MAX_PATH];
    FILE *f;
    if (i < 0 || !g_h[i].nmap || !g_h[i].name[0]) return;
    sidecar_path(g_h[i].name, p);
    f = fopen(p, "wb");
    if (!f) { tlog("sidecar: cannot write %s", p); return; }
    fwrite(SIDECAR_MAGIC, 8, 1, f);
    fwrite(&g_h[i].nmap, sizeof(int), 1, f);
    fwrite(g_h[i].map, sizeof(struct wt), g_h[i].nmap, f);
    fclose(f);
    tlog("sidecar: wrote %d pairs to %s", g_h[i].nmap, p);
}

static void sidecar_load(int i)
{
    char p[MAX_PATH], m[8];
    FILE *f;
    int n = 0;
    if (i < 0 || !g_h[i].name[0]) return;
    sidecar_path(g_h[i].name, p);
    f = fopen(p, "rb");
    if (!f) return;
    if (fread(m, 8, 1, f) != 1 || memcmp(m, SIDECAR_MAGIC, 7) != 0) { fclose(f); return; }
    if (fread(&n, sizeof(int), 1, f) != 1 || n <= 0 || n > 1000000) { fclose(f); return; }
    g_h[i].map = (struct wt *)malloc(n * sizeof(struct wt));
    if (!g_h[i].map) { fclose(f); return; }
    if ((int)fread(g_h[i].map, sizeof(struct wt), n, f) != n) {
        free(g_h[i].map); g_h[i].map = NULL; fclose(f); return;
    }
    g_h[i].nmap = g_h[i].capmap = n;
    g_h[i].is_reader = 1;
    fclose(f);
    tlog("sidecar: loaded %d pairs for %s", n, g_h[i].name);
}

static void tlog(const char *fmt, ...)
{
    va_list ap;
    if (!th_log) return;
    fprintf(th_log, "[%8lu] ", GetTickCount());
    va_start(ap, fmt);
    vfprintf(th_log, fmt, ap);
    va_end(ap);
    fputc('\n', th_log);
    fflush(th_log);
}

/* ---- originals -------------------------------------------------------- */
typedef BOOL  (WINAPI *pWriteFile)(HANDLE, LPCVOID, DWORD, LPDWORD, LPOVERLAPPED);
typedef DWORD (WINAPI *pSetFilePointer)(HANDLE, LONG, PLONG, DWORD);
typedef DWORD (WINAPI *pGetFileSize)(HANDLE, LPDWORD);

static pWriteFile      o_WriteFile;
static pSetFilePointer o_SetFilePointer;
static pGetFileSize    o_GetFileSize;
typedef BOOL (WINAPI *pReadFile)(HANDLE, LPVOID, DWORD, LPDWORD, LPOVERLAPPED);
static pReadFile       o_ReadFile;
typedef HANDLE (WINAPI *pCreateFileA)(LPCSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES,
                                      DWORD, DWORD, HANDLE);
static pCreateFileA    o_CreateFileA;
typedef BOOL (WINAPI *pCloseHandle)(HANDLE);
static pCloseHandle    o_CloseHandle;

static LONGLONG true_pos(HANDLE h)
{
    LARGE_INTEGER z = {{0}}, out;
    if (SetFilePointerEx(h, z, &out, FILE_CURRENT)) return out.QuadPart;
    return -1;
}

static LONGLONG true_size(HANDLE h)
{
    LARGE_INTEGER s;
    if (GetFileSizeEx(h, &s)) return s.QuadPart;
    return -1;
}

/* ---- replacements ----------------------------------------------------- */
/*
 * Position and length are not enough to identify a TIFF header. Checking
 * only "8 bytes at offset 0" pads whatever the module happens to write
 * first, and on a build where that is not the header, everything after it
 * is 8 bytes out of place. Verify the magic.
 */
static int is_tiff_header(LPCVOID p)
{
    const BYTE *b = (const BYTE *)p;
    if (b[0] == 'I' && b[1] == 'I' && b[2] == 42 && b[3] == 0) return 1;
    if (b[0] == 'M' && b[1] == 'M' && b[2] == 0  && b[3] == 42) return 1;
    return 0;
}

static BOOL WINAPI my_WriteFile(HANDLE h, LPCVOID buf, DWORD n,
                                LPDWORD done, LPOVERLAPPED ov)
{
    BOOL r;
    int i;
    LONGLONG before = -1;

    EnterCriticalSection(&th_lock);
    i = slot(h);
    /*
     * libtiff's first write is the 8-byte classic header at offset 0. For the
     * SAVED tiff only, pad it to 16 so pixel data starts clear of the space a
     * BigTIFF header needs. libtiff then asks SEEK_END where strip 0 goes and
     * records 16 itself.
     *
     * Never for the temp modules: padding there shifts every strip by 8 bytes
     * and IDHTempInput would read garbage.
     */
    if (cfg_bigtiff && i >= 0 && !g_h[i].padded &&
        g_h[i].owner == MOD_TIFFOUT && g_h[i].pos == 0 && n == 8 &&
        is_tiff_header(buf)) {
        BYTE pad[16];
        DWORD put = 0;
        memset(pad, 0, sizeof(pad));
        memcpy(pad, buf, 8);
        g_h[i].padded = 1;
        LeaveCriticalSection(&th_lock);
        r = o_WriteFile(h, pad, 16, &put, ov);
        EnterCriticalSection(&th_lock);
        if (r && put == 16) {
            g_h[i].pos = 16;
            if (g_h[i].pos > g_h[i].high_water) g_h[i].high_water = g_h[i].pos;
            g_h[i].writes++;
            tlog("bigtiff: padded header to 16 bytes for %s", g_h[i].name);
            if (done) *done = 8;          /* libtiff asked to write 8 */
            LeaveCriticalSection(&th_lock);
            return TRUE;
        }
        tlog("bigtiff: header padding FAILED, falling back");
        g_h[i].padded = 0;
        LeaveCriticalSection(&th_lock);
        return o_WriteFile(h, buf, n, done, ov);
    }
    if (i >= 0) { g_h[i].writes++; before = g_h[i].pos; }
    LeaveCriticalSection(&th_lock);

    r = o_WriteFile(h, buf, n, done, ov);

    EnterCriticalSection(&th_lock);
    if (i >= 0 && r && done) {
        g_h[i].pos += *done;
        if (g_h[i].pos > g_h[i].high_water) g_h[i].high_water = g_h[i].pos;
    }
    /* log the first few, then only around the 4 GB boundary */
    if (i >= 0 && (g_h[i].writes <= 5 ||
                   (before < 0x100000000LL && g_h[i].pos >= 0x100000000LL) ||
                   (g_h[i].writes % 20000) == 0 || !r))
        tlog("write  #%ld  %lu bytes at %lld -> %lld  %s",
             g_h[i].writes, n, before, i >= 0 ? g_h[i].pos : -1,
             r ? "ok" : "FAILED");
    LeaveCriticalSection(&th_lock);
    return r;
}


/* ------------------------------------------------------------------ */
/* classic TIFF -> BigTIFF, in place, at close                        */
/*                                                                    */
/* Verified against libtiff 3.9.7 and tifffile (warnings as errors):  */
/* a 4.6 GB file, 508 strips, 34 of them past 4 GiB, rows correct at  */
/* 2.1/4.2/4.6 GB. See source/libtiff_bigtiff_proof.c                 */
/*                                                                    */
/* No seek above 2 GiB is needed: the new IFD is appended where we    */
/* already are, and the only backward seek is to offset 0.            */
/* ------------------------------------------------------------------ */

static const int BT_TSZ[19] = {0,1,1,2,4,8,1,1,2,4,8,4,8,0,0,0,8,8,8};

static BOOL bt_read(HANDLE h, void *buf, DWORD n, LONGLONG at)
{
    LARGE_INTEGER li; DWORD got = 0;
    li.QuadPart = at;
    if (!SetFilePointerEx(h, li, NULL, FILE_BEGIN)) return FALSE;
    if (!o_ReadFile(h, buf, n, &got, NULL)) return FALSE;
    return got == n;
}
static BOOL bt_write(HANDLE h, const void *buf, DWORD n, LONGLONG at)
{
    LARGE_INTEGER li; DWORD put = 0;
    li.QuadPart = at;
    if (!SetFilePointerEx(h, li, NULL, FILE_BEGIN)) return FALSE;
    if (!o_WriteFile(h, buf, n, &put, NULL)) return FALSE;
    return put == n;
}

static int bigtiff_convert(int i, HANDLE h)
{
    BYTE hdr[8], big[16];
    DWORD ifd32;
    LONGLONG ifdoff, filesize, newifd, datacur;
    WORD n;
    BYTE *ent = NULL, *bifd = NULL;
    LONGLONG *extra = NULL;
    LARGE_INTEGER fsz;
    int k, hit, rc = -1;

    if (!GetFileSizeEx(h, &fsz)) return -1;
    filesize = fsz.QuadPart;
    if (!bt_read(h, hdr, 8, 0)) return -1;
    if (hdr[0] != 'I' || hdr[1] != 'I' || *(WORD *)(hdr + 2) != 42) {
        tlog("bigtiff: not a little-endian classic TIFF, leaving alone");
        return 1;                       /* not an error - just not ours */
    }
    memcpy(&ifd32, hdr + 4, 4);
    ifdoff = map_translate(i, ifd32, g_h[i].high_water, &hit);
    if (ifdoff + 2 > filesize) { tlog("bigtiff: IFD %lld beyond EOF %lld",
                                      ifdoff, filesize); return -1; }
    if (!bt_read(h, &n, 2, ifdoff) || n == 0 || n > 512) {
        tlog("bigtiff: entry count %u at offset %lld is not plausible -"
             " leaving the file as classic TIFF", n, ifdoff);
        return -1;
    }
    if (ifdoff + 2 + (LONGLONG)n * 12 > filesize) {
        tlog("bigtiff: a %u-entry directory does not fit at %lld in a %lld"
             " byte file - leaving it alone", n, ifdoff, filesize);
        return -1;
    }

    ent = (BYTE *)malloc((size_t)n * 12);
    if (!ent || !bt_read(h, ent, (DWORD)n * 12, ifdoff + 2)) goto done;

    extra = (LONGLONG *)calloc(n, sizeof(LONGLONG));
    bifd  = (BYTE *)calloc(1, 8 + (size_t)n * 20 + 8);
    if (!extra || !bifd) goto done;

    newifd  = (filesize + 7) & ~7LL;
    datacur = newifd + 8 + (LONGLONG)n * 20 + 8;

    for (k = 0; k < n; k++) {
        WORD tag, typ; DWORD cnt; LONGLONG total;
        memcpy(&tag, ent + k*12,     2);
        memcpy(&typ, ent + k*12 + 2, 2);
        memcpy(&cnt, ent + k*12 + 4, 4);
        /*
         * A tag count is only trustworthy if the data it describes could fit
         * in the file. A directory read from the wrong offset yields garbage
         * counts, and multiplying those out asks for allocations of gigabytes
         * - which surfaces as "not enough virtual memory" rather than as an
         * obviously bad read.
         */
        total = (LONGLONG)cnt * (typ < 19 ? BT_TSZ[typ] : 1);
        if (tag == 273 || tag == 279) total = (LONGLONG)cnt * 8;
        if (cnt > 0x400000 || total < 0 || total > filesize) {
            tlog("bigtiff: tag %u has an impossible count (%lu, %lld bytes"
                 " against a %lld byte file) - not converting",
                 tag, (unsigned long)cnt, total, filesize);
            goto done;
        }
        if (total > 8) { extra[k] = datacur; datacur += (total + 1) & ~1LL; }
    }

    *(ULONGLONG *)bifd = (ULONGLONG)n;

    for (k = 0; k < n; k++) {
        WORD tag, typ, otyp; DWORD cnt, p32; BYTE val[4];
        ULONGLONG cnt64; LONGLONG total;
        BYTE *e = bifd + 8 + (size_t)k * 20;

        memcpy(&tag, ent + k*12,     2);
        memcpy(&typ, ent + k*12 + 2, 2);
        memcpy(&cnt, ent + k*12 + 4, 4);
        memcpy(val,  ent + k*12 + 8, 4);
        otyp = typ; cnt64 = cnt;

        if (tag == 273 || tag == 279) {          /* strip offsets / counts */
            DWORD *src; ULONGLONG *dst; DWORD q;
            otyp = 16;                            /* LONG8 */
            total = (LONGLONG)cnt * 8;
            src = (DWORD *)malloc((size_t)cnt * 4);
            dst = (ULONGLONG *)malloc((size_t)cnt * 8);
            if (!src || !dst) { free(src); free(dst); goto done; }
            if (cnt * 4 <= 4) memcpy(src, val, (size_t)cnt * 4);
            else { memcpy(&p32, val, 4);
                   if (!bt_read(h, src, cnt * 4, p32)) { free(src); free(dst); goto done; } }
            for (q = 0; q < cnt; q++) {
                if (tag == 273) {                 /* offsets: un-wrap */
                    int h2;
                    dst[q] = (ULONGLONG)map_translate(i, src[q],
                                                      g_h[i].high_water, &h2);
                } else dst[q] = src[q];
            }
            if (total <= 8) memcpy(e + 12, dst, (size_t)total);
            else { if (!bt_write(h, dst, (DWORD)total, extra[k])) { free(src); free(dst); goto done; }
                   memcpy(e + 12, &extra[k], 8); }
            free(src); free(dst);
        } else {
            total = (LONGLONG)cnt * (typ < 19 ? BT_TSZ[typ] : 1);
            if (total <= 4) {
                memcpy(e + 12, val, (size_t)total);
            } else if (total <= 8) {
                BYTE tmp[8];
                memcpy(&p32, val, 4);
                if (!bt_read(h, tmp, (DWORD)total, p32)) goto done;
                memcpy(e + 12, tmp, (size_t)total);
            } else {
                BYTE *tmp = (BYTE *)malloc((size_t)total);
                memcpy(&p32, val, 4);
                if (!tmp || !bt_read(h, tmp, (DWORD)total, p32) ||
                    !bt_write(h, tmp, (DWORD)total, extra[k])) { free(tmp); goto done; }
                memcpy(e + 12, &extra[k], 8);
                free(tmp);
            }
        }
        memcpy(e,     &tag,   2);
        memcpy(e + 2, &otyp,  2);
        memcpy(e + 4, &cnt64, 8);
    }

    if (!bt_write(h, bifd, 8 + (DWORD)n * 20 + 8, newifd)) goto done;

    memset(big, 0, sizeof(big));
    big[0] = 'I'; big[1] = 'I';
    *(WORD *)(big + 2)      = 43;
    *(WORD *)(big + 4)      = 8;
    *(WORD *)(big + 6)      = 0;
    *(ULONGLONG *)(big + 8) = (ULONGLONG)newifd;
    if (!bt_write(h, big, 16, 0)) goto done;

    tlog("bigtiff: converted %s - %u tags, IFD at %lld", g_h[i].name, n, newifd);
    rc = 0;
done:
    if (rc < 0) tlog("bigtiff: CONVERSION FAILED for %s (err %lu)",
                     g_h[i].name, GetLastError());
    free(ent); free(extra); free(bifd);
    return rc;
}

/* CreateFileA gives us the name, which is what lets the reader find the
   sidecar the writer left behind. */
static HANDLE WINAPI my_CreateFileA(LPCSTR name, DWORD acc, DWORD share,
                                    LPSECURITY_ATTRIBUTES sa, DWORD disp,
                                    DWORD flags, HANDLE tmpl)
{
    void *ra = __builtin_return_address(0);
    HANDLE h = o_CreateFileA(name, acc, share, sa, disp, flags, tmpl);
    int i;
    if (h == INVALID_HANDLE_VALUE) return h;
    EnterCriticalSection(&th_lock);
    i = slot(h);
    if (i >= 0) {
        /* Windows reuses handle values. Never let a new file inherit the
           translation table of a previous one. */
        if (g_h[i].map) { free(g_h[i].map); g_h[i].map = NULL; }
        memset(&g_h[i], 0, sizeof(g_h[i]));
        g_h[i].h = h;
        g_h[i].owner = which_module(ra);
        lstrcpynA(g_h[i].name, name ? name : "", MAX_PATH);
        g_h[i].pos = 0;
        if (th_mode >= 3) {
            /*
             * Match on "the file already has content" rather than on the
             * access flags. A writer creating the temp file opens it empty,
             * so size 0 means nothing to match; a reader - whether it asked
             * for read-only or read-write - sees the bytes already there.
             * That avoids assuming which access mode IDHTempInput uses.
             */
            LONGLONG sz64 = true_size(h);
            struct wt *src = NULL;
            int n = (sz64 > 0) ? reg_get(sz64, &src) : 0;
            if (n > 0) {
                g_h[i].map = (struct wt *)malloc(n * sizeof(struct wt));
                if (g_h[i].map) {
                    memcpy(g_h[i].map, src, n * sizeof(struct wt));
                    g_h[i].nmap = g_h[i].capmap = n;
                    g_h[i].is_reader = 1;
                    tlog("registry: MATCHED %d pairs by size %lld for %s",
                         n, sz64, name ? name : "(null)");
                } else {
                    tlog("registry: out of memory copying %d pairs", n);
                }
            } else if (sz64 >= th_wrapat) {
                /* A file big enough to contain wrapped offsets, opened with
                   no table available. Say so rather than fail silently. */
                tlog("registry: NO TABLE for size %lld (%s) - this handle will"
                     " not be translated", sz64, name ? name : "(null)");
                if (th_sidecar_files) sidecar_load(i);
            }
        }
    }
    LeaveCriticalSection(&th_lock);
    return h;
}

static BOOL WINAPI my_CloseHandle(HANDLE h)
{
    int i;
    EnterCriticalSection(&th_lock);
    i = find_slot(h);          /* never allocate here */
    if (i >= 0) {
        if (cfg_bigtiff && g_h[i].owner == MOD_TIFFOUT && g_h[i].padded &&
            g_h[i].writes > 1) {
            bigtiff_convert(i, h);
        }
        if (th_mode >= 3 && !g_h[i].is_reader && g_h[i].nmap) {
            LONGLONG sz64 = true_size(h);
            if (sz64 <= 0) sz64 = g_h[i].high_water;
            reg_put(sz64, g_h[i].map, g_h[i].nmap);
            tlog("registry: stored %d pairs for size %lld (%s)",
                 g_h[i].nmap, sz64, g_h[i].name);
            if (th_sidecar_files) sidecar_save(i);
        }
        if (g_h[i].nmap || g_h[i].writes || g_h[i].reads)
            tlog("close  %s  writes %ld reads %ld seeks %ld translated %ld high water %lld",
                 g_h[i].name[0] ? g_h[i].name : "(unnamed)",
                 g_h[i].writes, g_h[i].reads, g_h[i].seeks,
                 g_h[i].translated, g_h[i].high_water);
        if (g_h[i].map) { free(g_h[i].map); g_h[i].map = NULL; }
        memset(&g_h[i], 0, sizeof(g_h[i]));
    }
    LeaveCriticalSection(&th_lock);
    return o_CloseHandle(h);
}

static BOOL WINAPI my_ReadFile(HANDLE h, LPVOID buf, DWORD n,
                              LPDWORD done, LPOVERLAPPED ov)
{
    BOOL r = o_ReadFile(h, buf, n, done, ov);
    int i;
    EnterCriticalSection(&th_lock);
    i = slot(h);
    if (i >= 0) {
        g_h[i].reads++;
        if (r && done) g_h[i].pos += *done;
        if (!r) tlog("read   FAILED at %lld, %lu bytes, err %lu",
                     g_h[i].pos, n, GetLastError());
    }
    LeaveCriticalSection(&th_lock);
    return r;
}

static DWORD WINAPI my_SetFilePointer(HANDLE h, LONG dist, PLONG high, DWORD method)
{
    int i;
    DWORD r;
    LONGLONG want, old;

    EnterCriticalSection(&th_lock);
    i = slot(h);
    if (i >= 0) g_h[i].seeks++;
    old = (i >= 0) ? g_h[i].pos : -1;
    LeaveCriticalSection(&th_lock);

    if (high) {                       /* caller is already 64-bit aware */
        r = o_SetFilePointer(h, dist, high, method);
        EnterCriticalSection(&th_lock);
        if (i >= 0) g_h[i].pos = true_pos(h);
        LeaveCriticalSection(&th_lock);
        return r;
    }

    /* libtiff means these as UNSIGNED offsets; Win32 reads them as signed */
    if (method == FILE_BEGIN) want = (LONGLONG)(ULONGLONG)(DWORD)dist;
    else if (method == FILE_CURRENT) want = old + dist;
    else want = -1;                   /* FILE_END: resolved below */

    if (th_mode >= 2) {
        LARGE_INTEGER li, out;
        BOOL okx;

        /*
         * Only intervene where the original would actually fail.
         *
         * Below 2 GiB the shipped 32-bit path is correct: the distance is
         * positive as a signed value and SetFilePointer works. Forwarding
         * unchanged there means every ordinary scan behaves bit-identically
         * to stock Newcolor, and the hook can only affect the cases that
         * were already broken. (CQscan method note, section 4: threshold at
         * 2 GiB, not 4 - below it, let the original code do its job.)
         */
        /*
         * Mode 3 translation must come FIRST. Wrapped offsets are SMALL -
         * the one measured on a 4.6 GB file was 305,015,902, far below the
         * 2 GiB threshold - so checking the threshold first would send every
         * wrapped seek down the untouched path and translate nothing.
         */
        if (th_mode >= 3 && method == FILE_BEGIN && i >= 0) {
            int hit = 0;
            LONGLONG t;
            EnterCriticalSection(&th_lock);
            t = g_h[i].nmap ? map_translate(i, (DWORD)dist, g_h[i].pos, &hit)
                            : 0;
            LeaveCriticalSection(&th_lock);
            if (hit) {
                LARGE_INTEGER li2, out2;
                li2.QuadPart = t;
                if (SetFilePointerEx(h, li2, &out2, FILE_BEGIN)) {
                    EnterCriticalSection(&th_lock);
                    g_h[i].pos = out2.QuadPart;
                    if (out2.QuadPart > g_h[i].high_water)
                        g_h[i].high_water = out2.QuadPart;
                    if (g_h[i].translated < 8)
                        tlog("seek   TRANSLATED %lu -> %lld",
                             (DWORD)dist, (LONGLONG)out2.QuadPart);
                    g_h[i].translated++;
                    LeaveCriticalSection(&th_lock);
                    return (DWORD)(out2.QuadPart & 0xFFFFFFFF);
                }
                tlog("seek   TRANSLATE FAILED %lu -> %lld err %lu",
                     (DWORD)dist, t, GetLastError());
                return INVALID_SET_FILE_POINTER;
            }
        }

        if (want < th_threshold && method != FILE_END) {
            r = o_SetFilePointer(h, dist, NULL, method);
            EnterCriticalSection(&th_lock);
            if (i >= 0 && r != INVALID_SET_FILE_POINTER) g_h[i].pos = r;
            LeaveCriticalSection(&th_lock);
            return r;
        }

        /*
         * At or above 2 GiB. libtiff means the offset as UNSIGNED (toff_t is
         * uint32); Win32 reads it as SIGNED when lpDistanceToMoveHigh is
         * NULL, so it comes out negative and fails ERROR_NEGATIVE_SEEK.
         * Reinterpreting as unsigned and seeking with a real 64-bit value is
         * exact for everything below 4 GiB - one 32-bit value, one real
         * offset, nothing to guess.
         *
         * We deliberately do NOT try to un-wrap offsets past 4 GiB. That
         * means guessing which multiple of 2^32 was intended, which guesses
         * wrong on small legitimate offsets such as the 4-byte header
         * pointer - and getting it wrong overwrites pixel data.
         */
        li.QuadPart = (method == FILE_END) ? dist : want;

        if (i >= 0 && g_h[i].high_water >= 0x100000000LL && !g_h[i].warned4g) {
            g_h[i].warned4g = 1;
            tlog("*** past 4 GiB - a classic TIFF header holds a 32-bit pointer");
            tlog("    to its directory and cannot address beyond this. Pixel");
            tlog("    data stays intact and libtiff still writes a correct IFD");
            tlog("    at the true position; only the pointer to it is lost.");
            if (th_mode >= 3)
                tlog("    Mode 3 active: offsets are being translated from the"
                     " recorded table.");
            else
                tlog("    Recover with bigtiff_newcolor.py --width <W>.");
        }

        okx = SetFilePointerEx(h, li, &out,
                               method == FILE_END ? FILE_END : FILE_BEGIN);
        if (!okx) {
            /* never return a plausible-looking value on failure */
            tlog("seek   FAILED  SetFilePointerEx(%lld, %s) err %lu",
                 (LONGLONG)li.QuadPart,
                 method == FILE_END ? "FILE_END" : "FILE_BEGIN",
                 GetLastError());
            return INVALID_SET_FILE_POINTER;
        }

        EnterCriticalSection(&th_lock);
        if (i >= 0) {
            g_h[i].fixed_seeks++;
            if (out.QuadPart > g_h[i].high_water) g_h[i].high_water = out.QuadPart;
            if (out.QuadPart < g_h[i].high_water - 0x1000LL) g_h[i].backseeks++;
            g_h[i].pos = out.QuadPart;
            if (th_mode >= 3 && method == FILE_END) map_record(i, out.QuadPart);
            if (g_h[i].fixed_seeks <= 8)
                tlog("seek   FIXED  %lu (as signed %ld) -> %lld  ok",
                     (DWORD)dist, dist, (LONGLONG)out.QuadPart);
        }
        LeaveCriticalSection(&th_lock);
        return (DWORD)(out.QuadPart & 0xFFFFFFFF);
    }

    /* mode 1: forward unchanged, just observe */
    r = o_SetFilePointer(h, dist, NULL, method);
    EnterCriticalSection(&th_lock);
    if (i >= 0) {
        LONGLONG now = true_pos(h);
        if (now >= 0) {
            if (now < g_h[i].high_water - 0x1000LL) g_h[i].backseeks++;
            g_h[i].pos = now;
        }
        if (r == INVALID_SET_FILE_POINTER || g_h[i].seeks <= 10 ||
            old >= 0x100000000LL || now >= 0x100000000LL)
            tlog("seek   dist=%ld (u:%lu) method=%lu  %lld -> %lld  ret=0x%08lX%s",
                 dist, (DWORD)dist, method, old, now, r,
                 r == INVALID_SET_FILE_POINTER ? "  <<< FAILED" : "");
    }
    LeaveCriticalSection(&th_lock);
    return r;
}

static DWORD WINAPI my_GetFileSize(HANDLE h, LPDWORD high)
{
    LONGLONG s = true_size(h);
    DWORD r;
    int i;

    EnterCriticalSection(&th_lock);
    i = slot(h);
    if (i >= 0) g_h[i].sizes++;
    if (i >= 0 && (g_h[i].sizes <= 5 || s >= 0x100000000LL))
        tlog("size   true=%lld  returning low32=0x%08lX%s", s,
             (DWORD)(s & 0xFFFFFFFF), s >= 0x100000000LL ? "  <<< WRAPPED" : "");
    LeaveCriticalSection(&th_lock);

    if (high) { *high = (DWORD)(s >> 32); return (DWORD)(s & 0xFFFFFFFF); }
    /* below 4 GiB GetFileSize(h,NULL) is already correct - do not touch it */
    if (th_mode >= 2 && s >= 0x100000000LL) return (DWORD)(s & 0xFFFFFFFF);
    r = o_GetFileSize(h, NULL);
    if (r == INVALID_FILE_SIZE && GetLastError() != NO_ERROR)
        tlog("size   GetFileSize FAILED err %lu", GetLastError());
    return r;
}

/* ---- IAT patching ----------------------------------------------------- */
static int patch_iat(HMODULE mod, const char *fn, void *repl, void **orig)
{
    IMAGE_DOS_HEADER *dos = (IMAGE_DOS_HEADER *)mod;
    IMAGE_NT_HEADERS *nt;
    IMAGE_IMPORT_DESCRIPTOR *imp;
    DWORD rva;
    int patched = 0;

    if (!mod || dos->e_magic != IMAGE_DOS_SIGNATURE) return 0;
    nt = (IMAGE_NT_HEADERS *)((BYTE *)mod + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return 0;
    rva = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress;
    if (!rva) return 0;
    imp = (IMAGE_IMPORT_DESCRIPTOR *)((BYTE *)mod + rva);

    for (; imp->Name; imp++) {
        IMAGE_THUNK_DATA *oft, *ft;
        if (!imp->OriginalFirstThunk) continue;
        oft = (IMAGE_THUNK_DATA *)((BYTE *)mod + imp->OriginalFirstThunk);
        ft  = (IMAGE_THUNK_DATA *)((BYTE *)mod + imp->FirstThunk);
        for (; oft->u1.AddressOfData; oft++, ft++) {
            IMAGE_IMPORT_BY_NAME *nm;
            DWORD old;
            if (oft->u1.Ordinal & IMAGE_ORDINAL_FLAG) continue;
            nm = (IMAGE_IMPORT_BY_NAME *)((BYTE *)mod + oft->u1.AddressOfData);
            if (lstrcmpiA((char *)nm->Name, fn) != 0) continue;
            if (!VirtualProtect(&ft->u1.Function, sizeof(void *),
                                PAGE_READWRITE, &old)) continue;
            if (orig && !*orig) *orig = (void *)ft->u1.Function;
            ft->u1.Function = (ULONG_PTR)repl;
            VirtualProtect(&ft->u1.Function, sizeof(void *), old, &old);
            patched++;
        }
    }
    return patched;
}

/*
 * Newcolor has TWO libtiff-based temp modules, each with its own copy of
 * the 32-bit I/O callbacks:
 *
 *   IDHTempOutput.idh   writes the temp file during the scan
 *   IDHTempInput.idh    reads it back to produce the final image
 *
 * Fixing only the writer moves the failure rather than removing it: a
 * >2 GiB temp file then writes correctly and fails to open, with
 * "IDHTempInput: cannot open file". Both must be patched.
 *
 * IDHTiffOutput.idh is included too - it writes the saved TIFF and has
 * the same callbacks.
 */
static const char *g_modules[] = {
    "IDHTempOutput.idh",
    "IDHTempInput.idh",
    "IDHTiffOutput.idh",
    NULL
};
static HMODULE g_done[8];



static int already_done(HMODULE m)
{
    int i;
    for (i = 0; i < 8; i++) if (g_done[i] == m) return 1;
    for (i = 0; i < 8; i++) if (!g_done[i]) { g_done[i] = m; return 0; }
    return 1;
}

static DWORD WINAPI watcher(LPVOID unused)
{
    int tries = 0, hooked = 0;
    (void)unused;
    /*
     * Modules load at different times - the reader only appears when the
     * scan finishes - so keep looking rather than patching once and
     * stopping.
     */
    for (;;) {
        int k;
        for (k = 0; g_modules[k]; k++) {
            HMODULE m = GetModuleHandleA(g_modules[k]);
            if (!m || already_done(m)) continue;
            {
                int a = patch_iat(m, "WriteFile",      (void *)my_WriteFile,      (void **)&o_WriteFile);
                int b = patch_iat(m, "SetFilePointer", (void *)my_SetFilePointer, (void **)&o_SetFilePointer);
                int c = patch_iat(m, "GetFileSize",    (void *)my_GetFileSize,    (void **)&o_GetFileSize);
                int d = patch_iat(m, "ReadFile",       (void *)my_ReadFile,       (void **)&o_ReadFile);
                int e = patch_iat(m, "CreateFileA",    (void *)my_CreateFileA,    (void **)&o_CreateFileA);
                int f2= patch_iat(m, "CloseHandle",    (void *)my_CloseHandle,    (void **)&o_CloseHandle);
                note_module(k, m);
                tlog("hooked %s at %p (%lu KB): Write=%d Seek=%d Size=%d Read=%d Create=%d Close=%d",
                     g_modules[k], m, (unsigned long)(g_modrange[k].size/1024),
                     a, b, c, d, e, f2);
                lg("temp hook attached to %s", g_modules[k]);
                hooked++;
            }
        }
        if (++tries > 14400) {              /* ~60 minutes */
            tlog("watcher stopping after %d modules hooked", hooked);
            return 0;
        }
        Sleep(250);
    }
}

void temphook_start(int mode, const char *logpath, int threshold_mb, int wrapat_mb, int sidecar_files, int bigtiff)
{
    if (mode <= 0) return;
    th_mode = mode;
    if (threshold_mb > 0)
        th_threshold = (LONGLONG)threshold_mb * 1024 * 1024;
    if (wrapat_mb > 0)
        th_wrapat = (LONGLONG)wrapat_mb * 1024 * 1024;
    th_sidecar_files = sidecar_files;
    cfg_bigtiff = bigtiff;
    InitializeCriticalSection(&th_lock);
    th_log = fopen(logpath, "a");
    tlog("=== temp hook starting, pid %lu, mode %d ===", GetCurrentProcessId(), mode);
    tlog("    BigTIFF output:       %s", cfg_bigtiff ? "ON" : "off");
    tlog("    sidecar records at:   %lld bytes (%lld MB)%s",
         th_wrapat, th_wrapat/(1024*1024),
         th_wrapat == 0x100000000LL ? "  [normal - 4 GiB]"
                                    : "  *** LOWERED FOR TESTING ***");
    tlog("    correction threshold: %lld bytes (%lld MB)%s",
         th_threshold, th_threshold / (1024*1024),
         th_threshold == 0x80000000LL ? "  [normal - 2 GiB]"
                                      : "  *** LOWERED FOR TESTING ***");
    CreateThread(NULL, 0, watcher, NULL, 0, NULL);
}

void temphook_report(void)
{
    int i;
    if (!th_log) return;
    tlog("----- per-handle summary -----");
    for (i = 0; i < MAXH; i++) {
        if (!g_h[i].h && !g_h[i].writes && !g_h[i].reads) continue;
        tlog("  handle %p  writes %ld  reads %ld  seeks %ld  sizes %ld  backseeks %ld  high water %lld",
             g_h[i].h, g_h[i].writes, g_h[i].reads, g_h[i].seeks, g_h[i].sizes,
             g_h[i].backseeks, g_h[i].high_water);
        tlog("    seeks corrected above threshold: %ld%s", g_h[i].fixed_seeks,
             g_h[i].fixed_seeks ? "" : "  (hook never needed to intervene)");
        if (g_h[i].high_water >= 0x100000000LL)
            tlog("    -> passed 4 GB%s", g_h[i].backseeks ?
                 " WITH backward seeks (mode 2 would be UNSAFE)" :
                 " append-only (mode 2 should be safe)");
    }
    fflush(th_log);
}
