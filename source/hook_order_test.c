/* Replay the real 4.6 GB seek sequence through the SHAPE of the ported
   hook logic, to check the decision order actually translates. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#define THRESH 0x80000000LL
static struct { uint32_t wrapped; long long truth; } map[4096];
static int nmap;
static long long pos, high;
static int mode = 3;
static long translated, forwarded_raw, corrected;

static void map_record(long long truth){
    if (truth < 0x100000000LL) return;
    if (nmap && map[nmap-1].truth==truth) return;
    map[nmap].wrapped=(uint32_t)(truth & 0xFFFFFFFFu); map[nmap].truth=truth; nmap++;
}
static long long map_translate(uint32_t want, long long now, int *hit){
    int k,best=-1; long long d,bd=0x7FFFFFFFFFFFFFFFLL; *hit=0;
    for(k=0;k<nmap;k++){ if(map[k].wrapped!=want) continue;
        d=map[k].truth-now; if(d<0)d=-d; if(d<bd){bd=d;best=k;} }
    if(best<0) return (long long)(unsigned long long)want;
    *hit=1; return map[best].truth;
}
/* mirrors my_SetFilePointer's decision order */
static long long seek(int32_t dist, int method /*0=BEGIN 2=END*/, long long true_end){
    long long want = (method==0) ? (long long)(uint32_t)dist : -1;
    if (mode>=3 && method==0) {
        int hit=0; long long t = nmap ? map_translate((uint32_t)dist,pos,&hit) : 0;
        if (hit) { pos=t; if(pos>high)high=pos; translated++; return pos; }
    }
    if (want < THRESH && method != 0+2) {          /* method!=FILE_END */
        if (method==0) { pos = want; forwarded_raw++; return pos; }
    }
    if (method==2) { pos = true_end + dist; if(pos>high)high=pos;
                     if(mode>=3) map_record(pos); return pos; }
    pos = want; corrected++; return pos;
}
int main(void){
    /* 508 strips of 9,064,704 bytes, header 8 - the measured 4.6 GB layout */
    long long rowbytes=70818, rps=128, strip=rowbytes*rps;
    long long end=8; int s;
    printf("phase 1: writing 508 strips (SEEK_END,0 before each)\n");
    for(s=0;s<508;s++){ seek(0,2,end); end += strip; }
    printf("  recorded pairs: %d   high water %lld\n", nmap, high);
    /* directory: SEEK_END then the SEEK_SET libtiff performs to its diroff */
    long long diroff = seek(0,2,end);
    uint32_t wrapped = (uint32_t)(diroff & 0xFFFFFFFFu);
    printf("\nphase 2: directory\n");
    printf("  true diroff   %lld\n", diroff);
    printf("  wrapped value %u  (below 2 GiB threshold: %s)\n",
           wrapped, ((long long)wrapped < THRESH) ? "YES" : "no");
    long long got = seek((int32_t)wrapped, 0, end);
    printf("  seek to wrapped -> landed at %lld  %s\n", got,
           got==diroff ? "CORRECT" : "*** WRONG - would overwrite pixels ***");
    printf("\n  translated %ld, forwarded untouched %ld, corrected %ld\n",
           translated, forwarded_raw, corrected);
    /* now the reader: same table, seek to each recorded wrapped value */
    printf("\nphase 3: reader replays every recorded wrapped offset\n");
    int bad=0; pos=0;
    for(s=0;s<nmap;s++){
        long long g = seek((int32_t)map[s].wrapped,0,end);
        if (g != map[s].truth) { bad++;
            printf("  MISMATCH wrapped %u -> %lld, expected %lld\n",
                   map[s].wrapped,g,map[s].truth); }
    }
    printf("  %d of %d resolved correctly\n", nmap-bad, nmap);
    printf("\n%s\n", (bad==0 && got==diroff) ? "LOGIC OK" : "*** LOGIC BROKEN ***");
    return 0;
}
