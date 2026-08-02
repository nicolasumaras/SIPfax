/*
 * V.34 Phase-2 ANSWER sequencer, REACTIVE rewrite.  Transitions are driven by the
 * modem-signal CLASSIFIER (what the modem is actually sending) instead of internal
 * timers, so we follow the modem's ~12 s schedule instead of racing it.
 *
 * Observation classes (per 30 ms): SILENCE / TONE_B / INFO / WIDEBAND / OTHER,
 * plus Tone-B 180-degree reversal events.
 *
 * Offline replay test: gcc -O2 -DR_TEST -lm -o v34_phase2r v34_phase2r.c
 *                      ./v34_phase2r cap.s16     (feeds modem upstream, logs sync)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#define SR 8000.0

/* ---- classifier (validated in v34_classify.c) ---- */
enum { SIL, TONEB, INFO, WIDE, OTHER };
static double goer(const short *x,int n,double f){double w=2*M_PI*f/SR,c=2*cos(w),s1=0,s2=0,s0;int k;for(k=0;k<n;k++){s0=x[k]+c*s1-s2;s2=s1;s1=s0;}return sqrt(s1*s1+s2*s2-c*s1*s2)/n;}
static double ph1200(const short *x,int a,int n){double w=2*M_PI*1200.0/SR,I=0,Q=0;int k;for(k=0;k<n;k++){double t=w*(a+k);I+=x[a+k]*cos(t);Q-=x[a+k]*sin(t);}return atan2(Q,I);}
static int classify(const short *x,int n){
    double rms=0;int k;for(k=0;k<n;k++)rms+=(double)x[k]*x[k];rms=sqrt(rms/n);
    if(rms<80)return SIL;
    double m1200=goer(x,n,1200.0);
    double hi=goer(x,n,3000.0)+goer(x,n,3300.0)+goer(x,n,3600.0)+goer(x,n,2700.0);
    double mid=goer(x,n,1950.0)+goer(x,n,2250.0);
    if(hi>300||(mid>300&&m1200<rms*0.3))return WIDE;
    if(m1200>rms*0.4){int sub=48,j,jumps=0;double prev=0;int have=0;
        for(j=0;j+sub<=n;j+=sub){double p=ph1200(x,j,sub);if(have){double d=p-prev;while(d>M_PI)d-=2*M_PI;while(d<-M_PI)d+=2*M_PI;if(fabs(d)>1.5)jumps++;}prev=p;have=1;}
        return (jumps>=3)?INFO:TONEB;}
    return OTHER;
}

/* ---- reactive answer-modem sequencer ---- */
enum { R_JOIN, R_RANGE, R_PROBE_RX, R_PROBE_TX, R_INFO1, R_DONE, R_FAIL };
static const char *RN[]={"JOIN","RANGE","PROBE_RX","PROBE_TX","INFO1","DONE","FAIL"};

typedef struct {
    int state;
    long tstate;           /* samples in state */
    int toneb_run;         /* consecutive TONE_B windows */
    int saw_wide;          /* observed the modem's L1/L2 */
    int wide_run;          /* consecutive WIDEBAND windows */
    int probe_samps; short probe[24000]; int probe_len;  /* captured modem L1/L2 */
    int symrate;
    int got_brev;
    double tb_ph; int tb_have;
    int last;
} V34P2R;

void v34p2r_init(V34P2R *p){ memset(p,0,sizeof(*p)); p->state=R_JOIN; p->symrate=-1; p->last=-1; }

/* feed one classified window; advance the reactive SM. Returns 0/1(done)/-1(fail).
   `cls` is the classifier output for this window, `x`/`n` the raw samples. */
static int step(V34P2R *p, int cls, const short *x, int n) {
    p->tstate += n;
    if (cls==TONEB) p->toneb_run++; else p->toneb_run=0;
    if (cls==WIDE) { p->wide_run++; p->saw_wide=1; } else p->wide_run=0;
    /* Tone B reversal detection during TONE_B */
    if (cls==TONEB) {
        double ph=ph1200(x,0,n);
        if (p->tb_have){double d=ph-p->tb_ph;while(d>M_PI)d-=2*M_PI;while(d<-M_PI)d+=2*M_PI;
            if(fabs(fabs(d)-M_PI)<0.6) p->got_brev=1;}
        p->tb_ph=ph; p->tb_have=1;
    } else p->tb_have=0;

    switch (p->state) {
    case R_JOIN:
        /* we sent INFO0a + Tone A; wait until the modem is clearly ranging (sustained
           Tone B) — handles late entry (modem already past INFO0c). */
        if (p->toneb_run >= 4) { p->state=R_RANGE; p->tstate=0; p->got_brev=0; }
        break;
    case R_RANGE:
        /* we emit a Tone A reversal; advance when we see the modem's Tone B reversal,
           OR when the modem moves on to probing (WIDEBAND) — never on a blind timer. */
        if (p->got_brev || p->saw_wide) { p->state=R_PROBE_RX; p->tstate=0; }
        if (p->tstate > (long)(20.0*SR)) return -1;   /* generous safety net */
        break;
    case R_PROBE_RX:
        /* receive the modem's L1/L2: accumulate WIDEBAND; when it ends, estimate. */
        if (cls==WIDE && p->probe_len+n < 24000) { memcpy(p->probe+p->probe_len,x,n*sizeof(short)); p->probe_len+=n; }
        if (p->saw_wide && p->wide_run==0 && p->probe_len > (int)(0.1*SR)) {
            /* probing ended -> channel estimate from captured L1/L2 */
            static const double PF[21]={150,300,450,600,750,1050,1350,1500,1650,1950,2100,2250,2550,2700,2850,3000,3150,3300,3450,3600,3750};
            double mx=0,top=0;int j;
            for(j=0;j<21;j++){double m=goer(p->probe,p->probe_len,PF[j]);if(m>mx)mx=m;}
            for(j=0;j<21;j++){double m=goer(p->probe,p->probe_len,PF[j]);if(m>mx*0.25&&PF[j]>top)top=PF[j];}
            p->symrate=(top>=3600)?5:(top>=3300)?4:(top>=3000)?3:(top>=2850)?2:(top>=2700)?1:0;
            p->state=R_PROBE_TX; p->tstate=0;
        }
        if (p->tstate > (long)(25.0*SR)) return -1;
        break;
    case R_PROBE_TX:
        /* transmit our L1+L2 (160ms + ~500ms), then INFO1 */
        if (p->tstate > (long)(0.7*SR)) { p->state=R_INFO1; p->tstate=0; }
        break;
    case R_INFO1:
        /* send INFO1a, receive modem INFO1c (its DPSK after probing); done */
        if (p->tstate > (long)(0.4*SR)) { p->state=R_DONE; return 1; }
        break;
    }
    return 0;
}

#ifdef R_TEST
int main(int argc, char **argv) {
    if(argc<2){fprintf(stderr,"usage: %s cap.s16\n",argv[0]);return 1;}
    FILE *f=fopen(argv[1],"rb"); static short s[2000000]; int n=fread(s,2,2000000,f); fclose(f);
    V34P2R p; v34p2r_init(&p);
    int W=240;
    printf("REACTIVE sequencer replayed against modem upstream %s:\n", argv[1]);
    for (int k=0; k+W<=n; k+=W) {
        int cls=classify(s+k,W);
        int r=step(&p,cls,s+k,W);
        if (p.state!=p.last){ printf("  [%.2fs] -> %-9s (toneb_run=%d saw_wide=%d brev=%d symrate=%d)\n",
                                     k/SR, RN[p.state], p.toneb_run, p.saw_wide, p.got_brev, p.symrate); p.last=p.state; }
        if (r==1){ printf("  [%.2fs] DONE: negotiated symbol rate idx=%d (%s)\n", k/SR, p.symrate,
                          p.symrate==5?"3429":"<3429"); return 0; }
        if (r==-1){ printf("  [%.2fs] FAIL in %s\n", k/SR, RN[p.state]); return 1; }
    }
    printf("  end of capture, state=%s\n", RN[p.state]);
    return 0;
}
#endif
