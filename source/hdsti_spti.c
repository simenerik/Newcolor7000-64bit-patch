/*
 * HDSTI.dll  -  SPTI replacement for Newcolor 7000 2.0
 *
 * Talks to the scanner directly with SCSI pass-through instead of going
 * through Windows' still-image stack. This removes sti.dll, stisvc and the
 * 32-bit minidriver HDHLusd.dll from the chain, which is what blocks the
 * product on 64-bit Windows.
 *
 *   Topaz2.ext -> KSS32.dll -> HDSTI.dll -> \\.\SCSIn: -> scanner
 *
 * Build 32-bit. Newcolor is a 32-bit application and SPTI works fine from a
 * 32-bit process under WOW64 on x64 Windows.
 *
 * ---------------------------------------------------------------------------
 * WHAT IS ESTABLISHED (recovered from the binaries, not guessed):
 *
 *   All eight entry points are __stdcall, undecorated, exported by ordinal.
 *   KSS32.dll imports by ORDINAL, so the .def ordinals must not move.
 *
 *   Return convention is BOOL: nonzero success, zero failure.
 *     KSS32 does "test eax,eax / jne <continue>" after Read and Write, and
 *     "test eax,eax / mov [esi+0xAC],eax / je <fail>" after Open.
 *
 *   Open returns the device handle in EAX. KSS32 stores it and passes it back
 *   as argument 1 of every other call. It never inspects it, so the value is
 *   ours to define. Here it is a pointer to struct dev.
 *
 *   Read takes a POINTER to the length (in/out), Write takes the length BY
 *   VALUE. That asymmetry mirrors IStiDevice::RawReadData / RawWriteData,
 *   which is what the original HDSTI called.
 *
 * WHAT IS INFERRED (verify against a real scanner):
 *
 *   The device reports SCSI peripheral type 03 (processor). Processor devices
 *   use SEND (0x0A) to take a block of bytes and RECEIVE (0x08) to return
 *   one. The original minidriver did plain ReadFile/WriteFile on a handle and
 *   built no CDBs of its own, so the kernel must have been wrapping the byte
 *   stream in exactly these two commands. Both opcodes are settable in the
 *   INI so this can be corrected without a rebuild.
 *
 *   Open's five arguments are ignored. They are most likely match criteria;
 *   with a single scanner on the bus, finding it by vendor string is
 *   equivalent. If a second Heidelberg device is ever attached, revisit this.
 * ---------------------------------------------------------------------------
 */

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <stddef.h>
#include <string.h>
#include <ntddscsi.h>

/* ---- configuration (hdsti.ini next to the DLL, section [hdsti]) -------- */
static char  cfg_vendor[16]  = "";      /* optional vendor filter; "" = any */
static char  cfg_prod[16][128];         /* type -> INQUIRY product substring */
static int   cfg_send_op     = 0x0A;   /* SCSI SEND    */
static int   cfg_recv_op     = 0x08;   /* SCSI RECEIVE */
static int   cfg_timeout     = 120;    /* seconds per command */
static int   cfg_chunk_kb    = 0;      /* 0 = derive from adapter capability */
static int   cfg_port        = -1;     /* -1 = search all ports */
static int   cfg_log         = 1;
static int   cfg_openmode    = 1;      /* 0=shared 1=exclusive-fail 2=same-handle */
static int   cfg_retry_ua    = 1;      /* retry once on UNIT ATTENTION      */
static int   cfg_zero_out    = 0;      /* write 0 to the two out-pointer args */


static FILE *g_log;
static CRITICAL_SECTION g_lock;
static struct dev *g_live[16];  /* one open device per scanner type */


#define DEV_MAGIC 0x48445354ul   /* 'HDST' */

struct dev;
struct dev {
    DWORD  magic;
    int    type;
    HANDLE hPort;
    char   port[32];
    UCHAR  path, target, lun;
    DWORD  chunk;
    BYTE  *bounce;
    HANDLE mutex;
};

typedef struct {
    SCSI_PASS_THROUGH_DIRECT sptd;
    UCHAR sense[32];
} SPTD_SENSE;

/* ---- logging ---------------------------------------------------------- */
static void lg(const char *fmt, ...)
{
    va_list ap;
    if (!g_log || !cfg_log) return;
    fprintf(g_log, "[%8lu] ", GetTickCount());
    va_start(ap, fmt);
    vfprintf(g_log, fmt, ap);
    va_end(ap);
    fputc('\n', g_log);
    fflush(g_log);
}

static void hexdump(const char *tag, const BYTE *b, DWORD n)
{
    DWORD i;
    if (!g_log || !cfg_log) return;
    if (n > 32) n = 32;
    fprintf(g_log, "             %s ", tag);
    for (i = 0; i < n; i++) fprintf(g_log, "%02X ", b[i]);
    fputc('\n', g_log);
    fflush(g_log);
}

/* dump the DWORD an argument points at, if it is readable */
static void peek(const char *tag, void *p) __attribute__((unused));
static void peek(const char *tag, void *p)
{
    if (!g_log || !cfg_log) return;
    if (!p || IsBadReadPtr(p, 4)) { lg("    %s 0x%08lX -> (not readable)",
                                       tag, (DWORD)(UINT_PTR)p); return; }
    lg("    %s 0x%08lX -> [%08lX] %02X %02X %02X %02X %02X %02X %02X %02X",
       tag, (DWORD)(UINT_PTR)p, *(DWORD *)p,
       ((BYTE*)p)[0],((BYTE*)p)[1],((BYTE*)p)[2],((BYTE*)p)[3],
       IsBadReadPtr(p,8)?0:((BYTE*)p)[4], IsBadReadPtr(p,8)?0:((BYTE*)p)[5],
       IsBadReadPtr(p,8)?0:((BYTE*)p)[6], IsBadReadPtr(p,8)?0:((BYTE*)p)[7]);
}

static void here(char *out, const char *leaf)
{
    char *p;
    GetModuleFileNameA(GetModuleHandleA("HDSTI.dll"), out, MAX_PATH);
    p = strrchr(out, '\\');
    if (p) lstrcpyA(p + 1, leaf); else lstrcpyA(out, leaf);
}

static void load_cfg(void)
{
    char ini[MAX_PATH];
    here(ini, "hdsti.ini");
    if (GetFileAttributesA(ini) == INVALID_FILE_ATTRIBUTES) return;
    GetPrivateProfileStringA("hdsti", "Vendor", cfg_vendor, cfg_vendor, sizeof(cfg_vendor), ini);
    cfg_send_op  = GetPrivateProfileIntA("hdsti", "SendOpcode",    cfg_send_op,  ini);
    cfg_recv_op  = GetPrivateProfileIntA("hdsti", "ReceiveOpcode", cfg_recv_op,  ini);
    cfg_timeout  = GetPrivateProfileIntA("hdsti", "TimeoutSec",    cfg_timeout,  ini);
    cfg_chunk_kb = GetPrivateProfileIntA("hdsti", "ChunkKB",       cfg_chunk_kb, ini);
    cfg_port     = GetPrivateProfileIntA("hdsti", "Port",          cfg_port,     ini);
    cfg_log      = GetPrivateProfileIntA("hdsti", "Log",           cfg_log,      ini);
    cfg_openmode = GetPrivateProfileIntA("hdsti", "OpenMode",      cfg_openmode, ini);
    cfg_zero_out = GetPrivateProfileIntA("hdsti", "ZeroOutArgs",   cfg_zero_out, ini);
    cfg_retry_ua = GetPrivateProfileIntA("hdsti", "RetryUnitAttention", cfg_retry_ua, ini);
    {   /* [types] maps Newcolor's scanner-type number to a product substring.
           Defaults cover every scanner these modules support. */
        int t; char key[8];
        lstrcpynA(cfg_prod[3], "TOPAZ", sizeof(cfg_prod[3]));
        lstrcpynA(cfg_prod[4], "TANGO,PRIMESCAN,PRIME,NEXSCAN,NEX",
                  sizeof(cfg_prod[4]));
        for (t = 0; t < 16; t++) {
            wsprintfA(key, "%d", t);
            GetPrivateProfileStringA("types", key, cfg_prod[t],
                                     cfg_prod[t], sizeof(cfg_prod[t]), ini);
        }
    }
}

/* ---- device discovery ------------------------------------------------- */
/* patterns is a comma-separated list; product matches if ANY entry is a
   substring of it. Never use an empty list as a wildcard: a type that
   matches any scanner will claim one belonging to another family. */
static int prod_matches(const char *product, const char *patterns)
{
    char buf[128], *p, *q;
    if (!patterns || !patterns[0]) return 0;
    lstrcpynA(buf, patterns, sizeof(buf));
    p = buf;
    while (p && *p) {
        q = strchr(p, ',');
        if (q) *q = 0;
        while (*p == ' ') p++;
        if (*p && strstr(product, p)) return 1;
        p = q ? q + 1 : NULL;
    }
    return 0;
}

static int probe(struct dev *d, int n, const char *prod_match)
{
    char path[32], buf[8192];
    HANDLE h;
    DWORD ret;
    SCSI_ADAPTER_BUS_INFO *info;
    IO_SCSI_CAPABILITIES caps;
    int bus, hit = 0;

    sprintf(path, "\\\\.\\SCSI%d:", n);
    h = CreateFileA(path, GENERIC_READ | GENERIC_WRITE,
                    FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                    OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) return 0;

    if (!DeviceIoControl(h, IOCTL_SCSI_GET_INQUIRY_DATA, NULL, 0,
                         buf, sizeof(buf), &ret, NULL)) {
        CloseHandle(h);
        return 0;
    }

    info = (SCSI_ADAPTER_BUS_INFO *)buf;
    for (bus = 0; bus < info->NumberOfBuses && !hit; bus++) {
        ULONG off = info->BusData[bus].InquiryDataOffset;
        while (off && off < sizeof(buf)) {
            SCSI_INQUIRY_DATA *q = (SCSI_INQUIRY_DATA *)(buf + off);
            if (q->InquiryDataLength >= 36 &&
                (q->InquiryData[0] & 0x1f) == 0x03 &&
                (!cfg_vendor[0] ||
                 memcmp(q->InquiryData + 8, cfg_vendor, strlen(cfg_vendor)) == 0)) {
                char prod[17] = {0}, ven[9] = {0};
                memcpy(prod, q->InquiryData + 16, 16);
                memcpy(ven,  q->InquiryData + 8,  8);
                if (!prod_matches(prod, prod_match)) {
                    lg("  skipping '%s %s' (does not match '%s')",
                       ven, prod, prod_match);
                    if (q->NextInquiryDataOffset == 0 ||
                        q->NextInquiryDataOffset == off) break;
                    off = q->NextInquiryDataOffset;
                    continue;
                }
                d->path = q->PathId; d->target = q->TargetId; d->lun = q->Lun;
                lstrcpyA(d->port, path);
                lg("found '%s %s' at %s bus %u id %u lun %u  claimed=%d",
                   ven, prod, path,
                   q->PathId, q->TargetId, q->Lun, q->DeviceClaimed);
                if (q->DeviceClaimed)
                    lg("  WARNING: device is claimed by a driver; pass-through may fail");
                hit = 1;
                break;
            }
            if (q->NextInquiryDataOffset == 0 || q->NextInquiryDataOffset == off) break;
            off = q->NextInquiryDataOffset;
        }
    }

    if (!hit) { CloseHandle(h); return 0; }

    d->chunk = 64 * 1024;
    if (DeviceIoControl(h, IOCTL_SCSI_GET_CAPABILITIES, NULL, 0,
                        &caps, sizeof(caps), &ret, NULL)) {
        DWORD by_len = caps.MaximumTransferLength;
        DWORD by_pg  = (caps.MaximumPhysicalPages > 1)
                     ? (caps.MaximumPhysicalPages - 1) * 4096u : 64u * 1024u;
        /* 0xFFFFFFFF means "no stated limit"; the page count is the real cap */
        if (by_len == 0xFFFFFFFFu || by_len == 0) by_len = by_pg;
        d->chunk = by_len < by_pg ? by_len : by_pg;
        lg("adapter: maxlen=%lu maxpages=%lu -> chunk %lu bytes",
           caps.MaximumTransferLength, caps.MaximumPhysicalPages, d->chunk);
    }
    if (cfg_chunk_kb > 0) {
        d->chunk = (DWORD)cfg_chunk_kb * 1024u;
        lg("chunk overridden by INI: %lu bytes", d->chunk);
    }

    d->hPort = h;
    return 1;
}

static int find_device(struct dev *d, const char *prod_match)
{
    int n;
    if (cfg_port >= 0) return probe(d, cfg_port, prod_match);
    for (n = 0; n < 32; n++) if (probe(d, n, prod_match)) return 1;
    return 0;
}

/* ---- one SCSI command ------------------------------------------------- */
static int scsi_io(struct dev *d, int to_device, BYTE *data, DWORD len, DWORD *done)
{
    SPTD_SENSE s;
    DWORD ret = 0;
    BOOL ok;
    int retrying = 0;

again:
    memset(&s, 0, sizeof(s));
    s.sptd.Length             = sizeof(SCSI_PASS_THROUGH_DIRECT);
    s.sptd.PathId             = d->path;
    s.sptd.TargetId           = d->target;
    s.sptd.Lun                = d->lun;
    s.sptd.CdbLength          = 6;
    s.sptd.SenseInfoLength    = sizeof(s.sense);
    s.sptd.DataIn             = to_device ? SCSI_IOCTL_DATA_OUT : SCSI_IOCTL_DATA_IN;
    s.sptd.DataTransferLength = len;
    s.sptd.TimeOutValue       = cfg_timeout;
    s.sptd.DataBuffer         = data;
    s.sptd.SenseInfoOffset    = offsetof(SPTD_SENSE, sense);

    /* 6-byte CDB, 24-bit big-endian length in bytes 2..4 */
    s.sptd.Cdb[0] = (UCHAR)(to_device ? cfg_send_op : cfg_recv_op);
    s.sptd.Cdb[1] = 0;
    s.sptd.Cdb[2] = (UCHAR)((len >> 16) & 0xff);
    s.sptd.Cdb[3] = (UCHAR)((len >>  8) & 0xff);
    s.sptd.Cdb[4] = (UCHAR)( len        & 0xff);
    s.sptd.Cdb[5] = 0;

    ok = DeviceIoControl(d->hPort, IOCTL_SCSI_PASS_THROUGH_DIRECT,
                         &s, sizeof(s), &s, sizeof(s), &ret, NULL);

    if (!ok) {
        lg("  ioctl failed, win32 error %lu", GetLastError());
        return 0;
    }
    if (s.sptd.ScsiStatus != 0) {
        UCHAR key = s.sense[2] & 0x0f;
        lg("  scsi status %02X  sense %02X/%02X/%02X (key/asc/ascq)",
           s.sptd.ScsiStatus, key, s.sense[12], s.sense[13]);
        hexdump("sense:", s.sense, sizeof(s.sense));
        /* Key 06 = UNIT ATTENTION: the device announcing it just reset,
           which happens right after a firmware upload. Standard practice
           is to re-issue the command once. */
        if (key == 0x06 && cfg_retry_ua && !retrying) {
            lg("  unit attention -> retrying once");
            retrying = 1;
            goto again;
        }
        return 0;
    }
    *done = s.sptd.DataTransferLength;
    return 1;
}

/* transfer len bytes, splitting into adapter-sized commands */
static int transfer(struct dev *d, int to_device, BYTE *buf, DWORD len, DWORD *moved)
{
    DWORD off = 0;
    *moved = 0;
    if (len == 0) return 1;

    while (off < len) {
        DWORD want = len - off;
        DWORD got  = 0;
        if (want > d->chunk) want = d->chunk;

        if (to_device) memcpy(d->bounce, buf + off, want);

        if (!scsi_io(d, to_device, d->bounce, want, &got)) return 0;

        if (!to_device) memcpy(buf + off, d->bounce, got ? got : want);

        off    += got ? got : want;
        *moved += got ? got : want;

        /* short read means the device has no more to give */
        if (!to_device && got && got < want) break;
    }
    return 1;
}

/* Newcolor has been seen calling Close with a bogus value (1). Never
   dereference a handle we did not issue. */
static struct dev *valid(void *h, const char *who)
{
    struct dev *d = (struct dev *)h;
    if (!d || IsBadReadPtr(d, sizeof(*d)) || d->magic != DEV_MAGIC) {
        lg("%s: ignoring invalid handle 0x%08lX", who, (DWORD)(UINT_PTR)h);
        return NULL;
    }
    return d;
}

/* ---- exported entry points ------------------------------------------- */

DWORD WINAPI HDSTIIsStiPresent(void *a0)
{
    struct dev p;
    int t, ok = 0;
    (void)a0;
    EnterCriticalSection(&g_lock);
    for (t = 0; t < 16 && !ok; t++) {
        if (!cfg_prod[t][0]) continue;
        memset(&p, 0, sizeof(p));
        if (find_device(&p, cfg_prod[t])) {
            ok = 1;
            if (p.hPort) CloseHandle(p.hPort);
        }
    }
    LeaveCriticalSection(&g_lock);
    lg("IsStiPresent -> %d", ok);
    return ok ? 1 : 0;
}

DWORD WINAPI HDSTIKSSOpenFirstScannerProcessor(void *a0, void *a1, void *a2,
                                               void *a3, void *a4)
{
    struct dev *d;
    int type = (int)(UINT_PTR)a1;

    lg("OpenFirstScannerProcessor type=%d (0x%08lX, 0x%08lX, 0x%08lX, 0x%08lX)",
       type, (DWORD)(UINT_PTR)a0, (DWORD)(UINT_PTR)a2,
       (DWORD)(UINT_PTR)a3, (DWORD)(UINT_PTR)a4);

    if (type < 0 || type > 15 || !cfg_prod[type][0]) {
        lg("  no product pattern configured for type %d -> FAILURE", type);
        return 0;
    }

    /* Newcolor probes every scanner family it has a module for. Answering
       for one we cannot actually see is what invented a phantom TANGO. */
    if (g_live[type]) {
        if (cfg_openmode == 1) { lg("  type %d already open -> FAILURE", type); return 0; }
        if (cfg_openmode == 2) {
            lg("  type %d already open -> same handle 0x%08lX",
               type, (DWORD)(UINT_PTR)g_live[type]);
            return (DWORD)(UINT_PTR)g_live[type];
        }
    }

    d = (struct dev *)calloc(1, sizeof(*d));
    if (!d) return 0;
    d->type = type;

    EnterCriticalSection(&g_lock);
    if (!find_device(d, cfg_prod[type])) {
        LeaveCriticalSection(&g_lock);
        lg("  no scanner matching '%s' -> FAILURE (correct if not attached)",
           cfg_prod[type]);
        free(d);
        return 0;
    }
    LeaveCriticalSection(&g_lock);

    d->bounce = (BYTE *)VirtualAlloc(NULL, d->chunk, MEM_COMMIT, PAGE_READWRITE);
    if (!d->bounce) { CloseHandle(d->hPort); free(d); return 0; }
    {
        char mtx[64];
        wsprintfA(mtx, "Global\\NewcolorHDSTI_%d", type);
        d->mutex = CreateMutexA(NULL, FALSE, mtx);
    }
    d->magic = DEV_MAGIC;

    if (cfg_zero_out) {
        if (a0 && !IsBadWritePtr(a0, 4)) *(DWORD *)a0 = 0;
        if (a3 && !IsBadWritePtr(a3, 4)) *(DWORD *)a3 = 0;
    }

    g_live[type] = d;
    lg("  opened type %d, handle 0x%08lX", type, (DWORD)(UINT_PTR)d);
    return (DWORD)(UINT_PTR)d;
}

DWORD WINAPI HDSTIKSSCloseScannerProcessor(void *a0)
{
    struct dev *d = valid(a0, "HDSTIKSSCloseScannerProcessor");
    lg("CloseScannerProcessor(0x%08lX)", (DWORD)(UINT_PTR)a0);
    if (!d) return 0;
    if (d->bounce) VirtualFree(d->bounce, 0, MEM_RELEASE);
    if (d->hPort)  CloseHandle(d->hPort);
    if (d->mutex)  CloseHandle(d->mutex);
    d->magic = 0;
    if (d->type >= 0 && d->type < 16 && g_live[d->type] == d)
        g_live[d->type] = NULL;
    free(d);
    return 1;
}

DWORD WINAPI HDSTIKSSLockScannerProcessor(void *a0, void *a1)
{
    struct dev *d = valid(a0, "HDSTIKSSLockScannerProcessor");
    DWORD ms = (DWORD)(UINT_PTR)a1;
    DWORD r;
    if (!d) return 0;
    if (ms == 0 || ms > 600000) ms = 30000;
    r = WaitForSingleObject(d->mutex, ms);
    lg("LockScannerProcessor(timeout=%lu) -> %lu", (DWORD)(UINT_PTR)a1, r);
    return (r == WAIT_OBJECT_0 || r == WAIT_ABANDONED) ? 1 : 0;
}

DWORD WINAPI HDSTIKSSUnLockScannerProcessor(void *a0)
{
    struct dev *d = valid(a0, "HDSTIKSSUnLockScannerProcessor");
    if (!d) return 0;
    ReleaseMutex(d->mutex);
    lg("UnLockScannerProcessor");
    return 1;
}

DWORD WINAPI HDSTIKSSReadData(void *a0, void *a1, void *a2, void *a3)
{
    struct dev *d   = valid(a0, "HDSTIKSSReadData");
    BYTE *buf       = (BYTE *)a1;
    DWORD *plen     = (DWORD *)a2;
    DWORD want, moved = 0;
    int ok;
    (void)a3;

    if (!d || !buf || !plen) return 0;
    want = *plen;
    lg("ReadData  want=%lu", want);

    ok = transfer(d, 0, buf, want, &moved);
    if (ok) {
        *plen = moved;
        hexdump("in: ", buf, moved);
        lg("  read %lu bytes", moved);
    } else {
        *plen = 0;
    }
    return ok ? 1 : 0;
}

DWORD WINAPI HDSTIKSSWriteData(void *a0, void *a1, void *a2, void *a3)
{
    struct dev *d = valid(a0, "HDSTIKSSWriteData");
    BYTE *buf     = (BYTE *)a1;
    DWORD len     = (DWORD)(UINT_PTR)a2;
    DWORD moved = 0;
    int ok;
    (void)a3;

    if (!d || !buf) return 0;
    lg("WriteData len=%lu", len);
    hexdump("out:", buf, len);

    ok = transfer(d, 1, buf, len, &moved);
    lg("  wrote %lu bytes, ok=%d", moved, ok);
    return ok ? 1 : 0;
}

DWORD WINAPI HDSTIKSSReOpenScannerProcessor(void *a0, void *a1, void *a2)
{
    struct dev *d = valid(a0, "HDSTIKSSReOpenScannerProcessor");
    lg("ReOpenScannerProcessor(0x%08lX, %lu, %lu)",
       (DWORD)(UINT_PTR)a0, (DWORD)(UINT_PTR)a1, (DWORD)(UINT_PTR)a2);
    if (!d) return 0;

    if (d->hPort) CloseHandle(d->hPort);
    d->hPort = CreateFileA(d->port, GENERIC_READ | GENERIC_WRITE,
                           FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                           OPEN_EXISTING, 0, NULL);
    if (d->hPort == INVALID_HANDLE_VALUE) {
        d->hPort = NULL;
        lg("  reopen failed, error %lu", GetLastError());
        return 0;
    }

    /* IMPORTANT: ReOpen returns a HANDLE, not a boolean. KSS32 does
         call ReOpen / test eax,eax / je fail / mov [edi+0xAC],eax
       and then Locks with that value. Returning 1 here made the stored
       handle become 1, which every later call then rejected.
       KSS32 also treats 0 and -1 as invalid, so never return those. */
    lg("  reopened %s, returning handle 0x%08lX", d->port, (DWORD)(UINT_PTR)d);
    return (DWORD)(UINT_PTR)d;
}

/* unused by KSS32, exported to keep the table identical */
DWORD WINAPI HDSTIGetNTVersion(void)     { return 0x0501; }
DWORD WINAPI HDSTIOpenFirstScanner(void) { return 0; }
DWORD WINAPI HDSTICloseScanner(void)     { return 1; }

BOOL WINAPI DllMain(HINSTANCE h, DWORD reason, LPVOID reserved)
{
    char p[MAX_PATH];
    (void)h; (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) {
        InitializeCriticalSection(&g_lock);
        load_cfg();
        here(p, "hdsti.log");
        g_log = fopen(p, "a");
        lg("=== HDSTI SPTI replacement loaded, pid %lu ===", GetCurrentProcessId());
        lg("vendor='%s' send=0x%02X recv=0x%02X timeout=%ds",
           cfg_vendor, cfg_send_op, cfg_recv_op, cfg_timeout);
    } else if (reason == DLL_PROCESS_DETACH) {
        lg("=== unloaded ===");
        if (g_log) fclose(g_log);
        DeleteCriticalSection(&g_lock);
    }
    return TRUE;
}
