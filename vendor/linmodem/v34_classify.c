/*
 * V.34 Phase-2 modem-signal CLASSIFIER — the observation layer of the reactive
 * sequencer. Classifies the modem's upstream each ~30 ms window into:
 *   SILENCE, TONE_B (steady 1200), INFO (1200 DPSK, modulated),
 *   WIDEBAND (L1/L2 probing or training), OTHER.
 * A reversal of Tone B is reported separately (180-degree flip in the 1200 carrier).
 *
 * Validate: gcc -O2 -lm -o v34_classify v34_classify.c && ./v34_classify cap.s16
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#define SR 8000.0

enum { SIL, TONEB, INFO, WIDE, OTHER };
static const char *CN[] = {"SILENCE","TONE_B","INFO","WIDEBAND","OTHER"};

static double goer(const short *x, int n, double f) {
    double w=2*M_PI*f/SR, c=2*cos(w), s1=0,s2=0,s0; int k;
    for(k=0;k<n;k++){s0=x[k]+c*s1-s2;s2=s1;s1=s0;}
    return sqrt(s1*s1+s2*s2-c*s1*s2)/n;
}
/* phase of the 1200 Hz component over [a,a+n) */
static double ph1200(const short *x, int a, int n) {
    double w=2*M_PI*1200.0/SR, I=0,Q=0; int k;
    for(k=0;k<n;k++){ double t=w*(a+k); I+=x[a+k]*cos(t); Q-=x[a+k]*sin(t); }
    return atan2(Q,I);
}

/* classify one window; sets *activity (phase transitions/window) for INFO vs TONE_B */
static int classify(const short *x, int n, int *act) {
    double rms=0; int k;
    for(k=0;k<n;k++) rms+=(double)x[k]*x[k]; rms=sqrt(rms/n);
    *act=0;
    if (rms < 80) return SIL;
    double m1200 = goer(x,n,1200.0);
    /* high-band + spectral spread => wideband (probe/training) */
    double hi = goer(x,n,3000.0)+goer(x,n,3300.0)+goer(x,n,3600.0)+goer(x,n,2700.0);
    double mid = goer(x,n,1950.0)+goer(x,n,2250.0);
    if (hi > 300 || (mid > 300 && m1200 < rms*0.3)) return WIDE;
    if (m1200 > rms*0.4) {
        /* phase activity: count big phase jumps across 6 ms sub-windows */
        int sub=48, j, jumps=0; double prev=0; int have=0;
        for (j=0;j+sub<=n;j+=sub) {
            double p=ph1200(x,j,sub);
            if (have) { double d=p-prev; while(d>M_PI)d-=2*M_PI; while(d<-M_PI)d+=2*M_PI; if(fabs(d)>1.5) jumps++; }
            prev=p; have=1;
        }
        *act=jumps;
        return (jumps >= 3) ? INFO : TONEB;   /* DPSK modulates; Tone B is steady */
    }
    return OTHER;
}

int main(int argc, char **argv) {
    if (argc<2){fprintf(stderr,"usage: %s cap.s16\n",argv[0]);return 1;}
    FILE *f=fopen(argv[1],"rb");
    static short s[2000000]; int n=fread(s,2,2000000,f); fclose(f);
    int W=240;                 /* 30 ms windows */
    int prev=-1; double seg_start=0; int act=0, segact=0;
    /* Tone B reversal tracking */
    double tbprev=0; int tbhave=0;
    printf("classifier timeline of %s (%.1fs):\n", argv[1], n/SR);
    for (int k=0; k+W<=n; k+=W) {
        int c=classify(s+k, W, &act);
        /* report Tone B reversals */
        if (c==TONEB) {
            double p=ph1200(s+k,0,W);
            if (tbhave){ double d=p-tbprev; while(d>M_PI)d-=2*M_PI; while(d<-M_PI)d+=2*M_PI;
                if (fabs(fabs(d)-M_PI)<0.6) printf("   [%.2fs] Tone B 180-deg REVERSAL\n", k/SR); }
            tbprev=p; tbhave=1;
        } else tbhave=0;
        if (c!=prev) {
            if (prev>=0) printf("  %5.2f - %5.2fs  %-9s (act=%d)\n", seg_start, k/SR, CN[prev], segact);
            prev=c; seg_start=k/SR; segact=act;
        } else if (act>segact) segact=act;
    }
    printf("  %5.2f - %5.2fs  %-9s\n", seg_start, n/SR, CN[prev]);
    return 0;
}
