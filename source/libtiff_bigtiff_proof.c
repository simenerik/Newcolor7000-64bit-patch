/*
 * ncbig.c - write a TIFF through libtiff 3.9.7 and convert it to BigTIFF
 *           in place at close, without moving the pixel data
 *
 * Why this shape:
 *
 *   A classic TIFF header is 8 bytes; a BigTIFF header is 16. libtiff writes
 *   the 8-byte header at open and then puts the first strip at offset 8, so
 *   a BigTIFF header would overwrite the first 8 bytes of image data. The fix
 *   is to pad that first write to 16 bytes, so pixels start at 16 and the
 *   header has room. Everything after is untouched.
 *
 *   At close the classic IFD libtiff wrote is parsed, every entry widened to
 *   the 20-byte BigTIFF form, and the result appended at the end of the file.
 *   Strip offsets are replaced with TRUE 64-bit values from the translation
 *   table - the ones libtiff recorded are wrapped past 4 GiB.
 *
 *   No seek above 2 GiB is needed: the IFD is appended where we already are,
 *   and the only backward seek is to offset 0 for the 16-byte header.
 *
 *   ./ncbig write <bytes>     write and convert
 *   ./ncbig check             report what a reader sees
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <inttypes.h>
#include "tiffio.h"

#define PATH "/tmp/ncbig.tif"
#define BIGHDR 16                     /* reserve this much for the header */

static int g_fd;
static uint64_t g_pos, g_high;
static long g_writes, g_seeks, g_translated;
static int g_padded;                  /* header write already padded? */

/* wrapped -> true, recorded from SEEK_END results at or above 4 GiB */
#define MAXMAP 8192
static struct { uint32_t w; uint64_t t; } g_map[MAXMAP];
static int g_nmap;

static void rec(uint64_t truth)
{
    if (truth < 0x100000000ULL) return;
    if (g_nmap && g_map[g_nmap-1].t == truth) return;
    if (g_nmap >= MAXMAP) return;
    g_map[g_nmap].w = (uint32_t)(truth & 0xFFFFFFFFu);
    g_map[g_nmap].t = truth;
    g_nmap++;
}
static uint64_t xlat(uint32_t want, uint64_t now, int *hit)
{
    int i, best = -1; uint64_t d, bd = ~0ULL;
    *hit = 0;
    for (i = 0; i < g_nmap; i++) {
        if (g_map[i].w != want) continue;
        d = g_map[i].t > now ? g_map[i].t - now : now - g_map[i].t;
        if (d < bd) { bd = d; best = i; }
    }
    if (best < 0) return want;
    *hit = 1;
    return g_map[best].t;
}

static tsize_t rd(thandle_t h, tdata_t p, tsize_t n)
{ ssize_t r; (void)h; r = read(g_fd, p, n); if (r > 0) g_pos += r; return r; }

static tsize_t wr(thandle_t h, tdata_t p, tsize_t n)
{
    ssize_t r;
    (void)h;
    g_writes++;
    /*
     * The very first write is libtiff's 8-byte classic header at offset 0.
     * Pad it to BIGHDR so the pixel data starts clear of the space a BigTIFF
     * header will need. libtiff then asks SEEK_END where to put strip 0 and
     * gets BIGHDR, so it records the right offset itself.
     */
    if (!g_padded && g_pos == 0 && n == 8) {
        uint8_t pad[BIGHDR];
        memset(pad, 0, sizeof(pad));
        memcpy(pad, p, 8);
        r = write(g_fd, pad, BIGHDR);
        g_padded = 1;
        if (r > 0) { g_pos += r; if (g_pos > g_high) g_high = g_pos; }
        return (tsize_t)(r == BIGHDR ? 8 : -1);      /* tell libtiff 8 */
    }
    r = write(g_fd, p, n);
    if (r > 0) { g_pos += r; if (g_pos > g_high) g_high = g_pos; }
    return (tsize_t)r;
}

static toff_t sk(thandle_t h, toff_t off, int whence)
{
    uint64_t target; int hit = 0;
    (void)h; g_seeks++;
    if (whence == SEEK_END) {
        off_t e = lseek(g_fd, (off_t)(int32_t)off, SEEK_END);
        g_pos = (uint64_t)e; if (g_pos > g_high) g_high = g_pos;
        rec(g_pos);
        return (toff_t)(g_pos & 0xFFFFFFFFu);
    }
    if (whence == SEEK_CUR) target = g_pos + (int32_t)off;
    else {
        target = xlat((uint32_t)off, g_pos, &hit);
        if (hit) g_translated++;
    }
    lseek(g_fd, (off_t)target, SEEK_SET);
    g_pos = target; if (g_pos > g_high) g_high = g_pos;
    return (toff_t)(target & 0xFFFFFFFFu);
}
static toff_t sz(thandle_t h)
{ off_t c=lseek(g_fd,0,SEEK_CUR), e=lseek(g_fd,0,SEEK_END); (void)h;
  lseek(g_fd,c,SEEK_SET); return (toff_t)((uint64_t)e & 0xFFFFFFFFu); }
static int  cl(thandle_t h){(void)h;return 0;}
static int  mp(thandle_t h,tdata_t*p,toff_t*n){(void)h;(void)p;(void)n;return 0;}
static void up(thandle_t h,tdata_t p,toff_t n){(void)h;(void)p;(void)n;}

/* ---- classic -> BigTIFF -------------------------------------------------- */

static const int TSZ[19] = {0,1,1,2,4,8,1,1,2,4,8,4,8,0,0,0,8,8,8};

static int to_bigtiff(int fd)
{
    uint8_t hdr[8];
    uint32_t ifdoff32;
    uint64_t ifdoff, filesize, newifd;
    uint16_t n;
    int i, hit;
    uint8_t *ent;
    uint64_t *extra_off;
    uint8_t big[16];

    if (pread(fd, hdr, 8, 0) != 8) return -1;
    if (hdr[0] != 'I' || hdr[1] != 'I') { fprintf(stderr,"only little-endian handled\n"); return -1; }
    memcpy(&ifdoff32, hdr + 4, 4);
    ifdoff = xlat(ifdoff32, g_high, &hit);
    filesize = (uint64_t)lseek(fd, 0, SEEK_END);
    printf("  classic IFD at %" PRIu32 " -> %" PRIu64 "%s\n",
           ifdoff32, ifdoff, hit ? " (translated)" : "");
    if (ifdoff + 2 > filesize) { fprintf(stderr,"IFD beyond EOF\n"); return -1; }

    if (pread(fd, &n, 2, ifdoff) != 2) return -1;
    printf("  %u entries\n", n);
    ent = malloc((size_t)n * 12);
    if (pread(fd, ent, (size_t)n * 12, ifdoff + 2) != (ssize_t)n * 12) return -1;

    /* pass 1: work out how much out-of-line data we must copy */
    extra_off = calloc(n, sizeof(uint64_t));
    newifd = (filesize + 7) & ~7ULL;            /* 8-align the BigTIFF IFD */
    uint64_t datacur = newifd + 8 + (uint64_t)n * 20 + 8;

    for (i = 0; i < n; i++) {
        uint16_t tag, typ; uint32_t cnt;
        memcpy(&tag, ent+i*12,   2);
        memcpy(&typ, ent+i*12+2, 2);
        memcpy(&cnt, ent+i*12+4, 4);
        uint64_t total = (uint64_t)cnt * (typ < 19 ? TSZ[typ] : 1);
        if (tag == 273 || tag == 279) total = (uint64_t)cnt * 8;   /* widen to LONG8 */
        if (total > 8) { extra_off[i] = datacur; datacur += (total + 1) & ~1ULL; }
    }

    /* pass 2: emit entries, copying or widening values as needed */
    uint8_t *bifd = calloc(1, 8 + (size_t)n * 20 + 8);
    uint64_t cnt64;
    memcpy(bifd, &(uint64_t){n}, 8);

    for (i = 0; i < n; i++) {
        uint16_t tag, typ; uint32_t cnt; uint8_t val[4];
        memcpy(&tag, ent+i*12,   2);
        memcpy(&typ, ent+i*12+2, 2);
        memcpy(&cnt, ent+i*12+4, 4);
        memcpy(val,  ent+i*12+8, 4);
        uint16_t otyp = typ;
        uint64_t total;

        uint8_t *e = bifd + 8 + (size_t)i * 20;
        cnt64 = cnt;

        if (tag == 273 || tag == 279) {         /* strip offsets / byte counts */
            otyp = 16;                          /* LONG8 */
            total = cnt64 * 8;
            uint32_t *src = malloc((size_t)cnt * 4);
            if (cnt * 4 <= 4) memcpy(src, val, (size_t)cnt * 4);
            else { uint32_t p32; memcpy(&p32, val, 4);
                   if (pread(fd, src, (size_t)cnt*4, p32) != (ssize_t)cnt*4) return -1; }
            uint64_t *dst = malloc((size_t)cnt * 8);
            for (uint32_t k = 0; k < cnt; k++) {
                if (tag == 273) {               /* offsets: un-wrap */
                    int h2; dst[k] = xlat(src[k], g_high, &h2);
                } else dst[k] = src[k];
            }
            if (total <= 8) memcpy(e+12, dst, (size_t)total);
            else { if (pwrite(fd, dst, (size_t)total, extra_off[i]) != (ssize_t)total) return -1;
                   memcpy(e+12, &extra_off[i], 8); }
            free(src); free(dst);
        } else {
            total = cnt64 * (typ < 19 ? TSZ[typ] : 1);
            if (total <= 4) {                   /* was inline, still inline */
                memcpy(e+12, val, (size_t)total);
            } else if (total <= 8) {            /* was out of line, now fits */
                uint32_t p32; memcpy(&p32, val, 4);
                uint8_t tmp[8];
                if (pread(fd, tmp, (size_t)total, p32) != (ssize_t)total) return -1;
                memcpy(e+12, tmp, (size_t)total);
            } else {                            /* copy the payload verbatim */
                uint32_t p32; memcpy(&p32, val, 4);
                uint8_t *tmp = malloc((size_t)total);
                if (pread(fd, tmp, (size_t)total, p32) != (ssize_t)total) return -1;
                if (pwrite(fd, tmp, (size_t)total, extra_off[i]) != (ssize_t)total) return -1;
                memcpy(e+12, &extra_off[i], 8);
                free(tmp);
            }
        }
        memcpy(e,   &tag,   2);
        memcpy(e+2, &otyp,  2);
        memcpy(e+4, &cnt64, 8);
    }

    if (pwrite(fd, bifd, 8 + (size_t)n*20 + 8, newifd) < 0) return -1;

    /* 16-byte BigTIFF header */
    memset(big, 0, sizeof(big));
    big[0]='I'; big[1]='I';
    *(uint16_t*)(big+2) = 43;
    *(uint16_t*)(big+4) = 8;        /* offset size */
    *(uint16_t*)(big+6) = 0;
    *(uint64_t*)(big+8) = newifd;
    if (pwrite(fd, big, 16, 0) != 16) return -1;

    printf("  BigTIFF IFD at %" PRIu64 ", file now %" PRIu64 "\n",
           newifd, (uint64_t)lseek(fd, 0, SEEK_END));
    free(ent); free(extra_off); free(bifd);
    return 0;
}

int main(int argc, char **argv)
{
    const char *mode = argc > 1 ? argv[1] : "write";
    uint64_t want = argc > 2 ? strtoull(argv[2], NULL, 10) : 4600000000ULL;
    uint32_t W = 11803, SPP = 3, BPS = 16;
    uint64_t rowbytes = (uint64_t)W*SPP*(BPS/8), rows = want / rowbytes, y;
    TIFF *t; uint8_t *row;

    if (strcmp(mode, "write")) { printf("usage: ncbig write <bytes>\n"); return 1; }

    printf("\n=== write %.2f GB, header padded to %d, then convert ===\n",
           (double)want/1e9, BIGHDR);
    unlink(PATH);
    g_fd = open(PATH, O_RDWR|O_CREAT|O_TRUNC, 0644);
    t = TIFFClientOpen(PATH, "w", (thandle_t)1, rd, wr, sk, cl, sz, mp, up);
    if (!t) { printf("open failed\n"); return 1; }
    TIFFSetField(t, TIFFTAG_IMAGEWIDTH, W);
    TIFFSetField(t, TIFFTAG_IMAGELENGTH, (uint32_t)rows);
    TIFFSetField(t, TIFFTAG_BITSPERSAMPLE, BPS);
    TIFFSetField(t, TIFFTAG_SAMPLESPERPIXEL, SPP);
    TIFFSetField(t, TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_CIELAB);
    TIFFSetField(t, TIFFTAG_PLANARCONFIG, PLANARCONFIG_CONTIG);
    TIFFSetField(t, TIFFTAG_COMPRESSION, COMPRESSION_NONE);
    TIFFSetField(t, TIFFTAG_ROWSPERSTRIP, 128);
    TIFFSetField(t, TIFFTAG_XRESOLUTION, 11000.0f);
    TIFFSetField(t, TIFFTAG_YRESOLUTION, 11000.0f);

    row = malloc(rowbytes); memset(row, 0xA5, rowbytes);
    for (y = 0; y < rows; y++) {
        memcpy(row, &y, sizeof(y));
        if (TIFFWriteScanline(t, row, (uint32_t)y, 0) < 0) {
            printf("  scanline %" PRIu64 " failed\n", y); break;
        }
    }
    printf("  rows %" PRIu64 ", writes %ld, seeks %ld, translated %ld\n",
           y, g_writes, g_seeks, g_translated);
    TIFFClose(t);

    printf("  converting to BigTIFF...\n");
    if (to_bigtiff(g_fd) != 0) { printf("  CONVERSION FAILED\n"); return 1; }
    close(g_fd);
    printf("  done: %s\n", PATH);
    return 0;
}
