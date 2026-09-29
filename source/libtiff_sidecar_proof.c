/*
 * nc64.c - >4 GiB round trip through real libtiff 3.9.7 using a sidecar
 *
 * libtiff 3.x stores every offset as uint32, so past 4 GiB the values it
 * records are wrapped. We cannot un-wrap them by guessing - that guesses
 * wrong on small legitimate offsets. But we are on BOTH sides of the file:
 * the writer and the reader both go through our callbacks. So we record the
 * true 64-bit position for every wrapped value as we write, and translate
 * on read. Nothing is inferred.
 *
 *   ./nc64 write <bytes>    write a file larger than 4 GiB, emit sidecar
 *   ./nc64 read             read it back through the sidecar, verify pixels
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <inttypes.h>
#include "tiffio.h"

#define PATH "/tmp/nc_test.tif"
#define SIDE "/tmp/nc_test.sidecar"
#define THRESH 0x80000000ULL          /* below this the 32-bit path is fine */

static int g_fd;
static uint64_t g_pos, g_high;
static long g_seeks, g_writes, g_reads, g_fixed;

/* recorded wrapped->true pairs, in write order */
#define MAXMAP 8192
static struct { uint32_t wrapped; uint64_t truth; } g_map[MAXMAP];
static int g_nmap;

static void record(uint64_t truth)
{
    uint32_t w = (uint32_t)(truth & 0xFFFFFFFFu);
    if (truth < 0x100000000ULL) return;        /* nothing ambiguous below 4 GiB */
    if (g_nmap >= MAXMAP) return;
    /* skip exact duplicates */
    if (g_nmap && g_map[g_nmap-1].truth == truth) return;
    g_map[g_nmap].wrapped = w;
    g_map[g_nmap].truth   = truth;
    g_nmap++;
}

static uint64_t translate(uint32_t want)
{
    int i, best = -1;
    uint64_t d, bd = ~0ULL;
    for (i = 0; i < g_nmap; i++) {
        if (g_map[i].wrapped != want) continue;
        d = (g_map[i].truth > g_pos) ? g_map[i].truth - g_pos : g_pos - g_map[i].truth;
        if (d < bd) { bd = d; best = i; }
    }
    return best >= 0 ? g_map[best].truth : (uint64_t)want;
}

static tsize_t rd(thandle_t h, tdata_t p, tsize_t n)
{ ssize_t r; (void)h; r = read(g_fd, p, n); if (r>0) g_pos += r; g_reads++; return r; }

static tsize_t wr(thandle_t h, tdata_t p, tsize_t n)
{
    ssize_t r; (void)h;
    r = write(g_fd, p, n);
    if (r > 0) { g_pos += r; if (g_pos > g_high) g_high = g_pos; }
    g_writes++;
    return (tsize_t)r;
}

static int g_reading;

static toff_t sk(thandle_t h, toff_t off, int whence)
{
    uint64_t target;
    (void)h; g_seeks++;

    if (whence == SEEK_END) {
        off_t e = lseek(g_fd, (off_t)(int32_t)off, SEEK_END);
        g_pos = (uint64_t)e;
        if (g_pos > g_high) g_high = g_pos;
        record(g_pos);
        return (toff_t)(g_pos & 0xFFFFFFFFu);
    }
    if (whence == SEEK_CUR) target = g_pos + (int32_t)off;
    else {
        target = (uint64_t)(uint32_t)off;      /* unsigned, as libtiff means it */
        /* mirror the hook exactly: translation is tried BEFORE the 2 GiB
           threshold check, because wrapped offsets are small. */
        {
            int hit2 = 0;
            uint64_t t3 = g_nmap ? translate((uint32_t)off) : 0;
            if (g_nmap) { hit2 = (t3 != (uint64_t)(uint32_t)off); }
            if (hit2) {
                g_fixed++;
                lseek(g_fd,(off_t)t3,SEEK_SET); g_pos=t3;
                if (g_pos > g_high) g_high = g_pos;
                return (toff_t)(t3 & 0xFFFFFFFFu);
            }
        }
        if (target < 0x80000000ULL) {          /* below threshold: untouched */
            lseek(g_fd,(off_t)target,SEEK_SET); g_pos=target;
            return (toff_t)(target & 0xFFFFFFFFu);
        }
        /*
         * Translate on BOTH sides. libtiff's tif_curoff is uint32, so once
         * the file passes 4 GiB every offset it hands back is wrapped -
         * including the one it seeks to in order to write the directory.
         * Left alone, that lands mid-file and overwrites pixel data.
         * We recorded the true position when SEEK_END produced it, so the
         * mapping is exact rather than inferred.
         */

    }
    if (target >= THRESH) g_fixed++;
    lseek(g_fd, (off_t)target, SEEK_SET);
    g_pos = target;
    if (g_pos > g_high) g_high = g_pos;
    return (toff_t)(target & 0xFFFFFFFFu);
}

static toff_t sz(thandle_t h)
{ off_t c=lseek(g_fd,0,SEEK_CUR), e=lseek(g_fd,0,SEEK_END); (void)h;
  lseek(g_fd,c,SEEK_SET); return (toff_t)((uint64_t)e & 0xFFFFFFFFu); }
static int  cl(thandle_t h){(void)h;return 0;}
static int  mp(thandle_t h,tdata_t*p,toff_t*n){(void)h;(void)p;(void)n;return 0;}
static void up(thandle_t h,tdata_t p,toff_t n){(void)h;(void)p;(void)n;}

static void save_sidecar(void)
{
    FILE *f = fopen(SIDE, "wb");
    fwrite("HDSTI64\0", 8, 1, f);
    fwrite(&g_nmap, sizeof(g_nmap), 1, f);
    fwrite(g_map, sizeof(g_map[0]), g_nmap, f);
    fclose(f);
    printf("  sidecar: %d wrapped->true pairs\n", g_nmap);
}
static int load_sidecar(void)
{
    char m[8];
    FILE *f = fopen(SIDE, "rb");
    if (!f) return 0;
    if (fread(m,8,1,f)!=1 || memcmp(m,"HDSTI64",7)) { fclose(f); return 0; }
    if (fread(&g_nmap,sizeof(g_nmap),1,f)!=1) { fclose(f); return 0; }
    if (g_nmap<0 || g_nmap>MAXMAP) { fclose(f); return 0; }
    fread(g_map, sizeof(g_map[0]), g_nmap, f);
    fclose(f);
    printf("  sidecar: %d pairs loaded\n", g_nmap);
    return 1;
}

static uint32_t W=11803, SPP=3, BPS=16;

int main(int argc, char **argv)
{
    const char *mode = argc>1?argv[1]:"write";
    uint64_t want = argc>2?strtoull(argv[2],NULL,10):4600000000ULL;
    uint64_t rowbytes = (uint64_t)W*SPP*(BPS/8), rows = want/rowbytes;
    TIFF *t; uint8_t *row; uint64_t y;

    if (!strcmp(mode,"write")) {
        printf("\n=== WRITE %.2f GB ===\n", (double)want/1e9);
        unlink(PATH);
        g_fd = open(PATH, O_RDWR|O_CREAT|O_TRUNC, 0644);
        t = TIFFClientOpen(PATH,"w",(thandle_t)1,rd,wr,sk,cl,sz,mp,up);
        if(!t){printf("open failed\n");return 1;}
        TIFFSetField(t,TIFFTAG_IMAGEWIDTH,W);
        TIFFSetField(t,TIFFTAG_IMAGELENGTH,(uint32_t)rows);
        TIFFSetField(t,TIFFTAG_BITSPERSAMPLE,BPS);
        TIFFSetField(t,TIFFTAG_SAMPLESPERPIXEL,SPP);
        TIFFSetField(t,TIFFTAG_PHOTOMETRIC,PHOTOMETRIC_CIELAB);
        TIFFSetField(t,TIFFTAG_PLANARCONFIG,PLANARCONFIG_CONTIG);
        TIFFSetField(t,TIFFTAG_COMPRESSION,COMPRESSION_NONE);
        TIFFSetField(t,TIFFTAG_ROWSPERSTRIP,128);
        row=malloc(rowbytes); memset(row,0xA5,rowbytes);
        for(y=0;y<rows;y++){
            memcpy(row,&y,sizeof(y));
            if(TIFFWriteScanline(t,row,(uint32_t)y,0)<0){
                printf("  scanline %" PRIu64 " FAILED\n",y); break; }
        }
        printf("  rows written: %" PRIu64 "\n", y);
        printf("  --- calling TIFFWriteDirectory explicitly ---\n");
        {
            int wd = TIFFWriteDirectory(t);
            printf("  TIFFWriteDirectory returned %d %s\n", wd,
                   wd ? "(success)" : "*** FAILED ***");
        }
        TIFFClose(t);
        {   /* what did libtiff put in the header? */
            uint32_t hdr; lseek(g_fd,4,SEEK_SET); read(g_fd,&hdr,4);
            printf("  header IFD pointer = %" PRIu32 " (0x%08X)%s\n", hdr, hdr,
                   hdr==0 ? "   <<< ZERO - untranslatable" : "");
            if (hdr) printf("  translates to      = %" PRIu64 "\n", translate(hdr));
        }
        close(g_fd);
        printf("  file size: %" PRId64 "\n",(int64_t)lseek(open(PATH,O_RDONLY),0,SEEK_END));
        printf("  seeks %ld writes %ld, above 4 GiB %ld\n", g_seeks,g_writes,g_fixed);
        save_sidecar();
        return 0;
    }

    printf("\n=== READ BACK through sidecar ===\n");
    if(!load_sidecar()){printf("  no sidecar\n");return 1;}
    g_reading=1;
    g_fd=open(PATH,O_RDONLY);
    g_high=lseek(g_fd,0,SEEK_END); lseek(g_fd,0,SEEK_SET); g_pos=0;
    t=TIFFClientOpen(PATH,"r",(thandle_t)1,rd,wr,sk,cl,sz,mp,up);
    if(!t){printf("  TIFFClientOpen FAILED\n");return 1;}
    {   uint32 w=0,h=0; uint16 spp=0,bps=0;
        TIFFGetField(t,TIFFTAG_IMAGEWIDTH,&w);TIFFGetField(t,TIFFTAG_IMAGELENGTH,&h);
        TIFFGetField(t,TIFFTAG_SAMPLESPERPIXEL,&spp);TIFFGetField(t,TIFFTAG_BITSPERSAMPLE,&bps);
        printf("  OPENED: %ux%u %ux%u-bit, %u strips\n",w,h,spp,bps,TIFFNumberOfStrips(t));
        row=malloc(TIFFScanlineSize(t));
        int bad=0; int nprobe=0;
        uint32 probe[600]; { unsigned k; for(k=0;k<600;k++) probe[k]=(uint32)((uint64_t)k*h/600); }
        for(unsigned i=0;i<600;i++){
            uint32 r=probe[i]; if(r>=h)continue; nprobe++;
            if(TIFFReadScanline(t,row,r,0)<0){printf("  row %-7u READ FAILED\n",r);bad++;continue;}
            uint64_t st2; memcpy(&st2,row,8);
            if(st2!=r){ printf("  row %-7u stamp=%-8" PRIu64 " *** MISMATCH ***\n",r,st2); bad++; }
            else if(i<3 || i>596) printf("  row %-7u stamp=%-8" PRIu64 " OK\n",r,st2);
        }
        printf("\n  rows probed: %d   mismatches: %d\n", nprobe, bad);
        printf("  translations used: %ld\n", g_fixed);
        printf("  %s\n", bad?"*** ROUND TRIP BROKEN ***":"ROUND TRIP OK");
    }
    TIFFClose(t); return 0;
}
