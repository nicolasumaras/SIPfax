/*
 * V.34 Phase-2 ANSWER-modem module for linmodem/SIPfax — REACTIVE.
 *
 * Transitions are driven by a CLASSIFIER of the modem's actual signal (SILENCE /
 * TONE_B / INFO / WIDEBAND) so we follow the modem's ~10-12 s Phase-2 schedule
 * instead of racing it on internal timers.  TX is a continuous-phase generator
 * (DPSK INFO0a/INFO1a on 2400 + guard, Tone A w/ reversals, L1/L2 probe).
 *
 * Validated offline by replaying the real modem upstream (v34_phase2r.c): it syncs
 * to the modem (RANGE when Tone B starts, PROBE_RX when the modem probes at ~7.8 s).
 *
 * Standalone: gcc -O2 -DP2_TEST -lm -o v34_phase2 v34_phase2.c
 * In linmodem: v34_phase2_new()/v34_phase2_run()/v34_phase2_symrate()/v34_phase2_free().
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#ifndef SR
#define SR 8000.0
#endif

/* ---------- INFO codec (validated) ---------- */
static int info_crc(const unsigned char *b,int n){int c=0xffff,i,x;for(i=0;i<n;i++){x=(c&1)^b[i];c>>=1;if(x)c^=(1<<15)|(1<<10)|(1<<3);}return c&0xffff;}
static const int INFO_SYNC[8]={0,1,1,1,0,0,1,0};
static void info0a_build(unsigned char*b){int i,c;memset(b,0,49);b[0]=b[1]=b[2]=b[3]=1;for(i=0;i<8;i++)b[4+i]=INFO_SYNC[i];b[12]=b[13]=b[14]=1;b[15]=b[16]=b[17]=b[18]=1;b[19]=1;b[20]=1;b[25]=1;c=info_crc(b+12,17);for(i=0;i<16;i++)b[29+i]=(c>>i)&1;b[45]=b[46]=b[47]=b[48]=1;}
static void info1a_build(unsigned char*b,int sr_a2c,int sr_c2a,int pre,int rate){int i,c;memset(b,0,70);b[0]=b[1]=b[2]=b[3]=1;for(i=0;i<8;i++)b[4+i]=INFO_SYNC[i];for(i=0;i<4;i++)b[26+i]=(pre>>i)&1;for(i=0;i<4;i++)b[30+i]=(rate>>i)&1;for(i=0;i<3;i++)b[34+i]=(sr_a2c>>i)&1;for(i=0;i<3;i++)b[37+i]=(sr_c2a>>i)&1;c=info_crc(b+12,38);for(i=0;i<16;i++)b[50+i]=(c>>i)&1;b[66]=b[67]=b[68]=b[69]=1;}

/* ---------- DSP: tone magnitude, 1200 phase, probe tones, classifier ---------- */
static const double PF[21]={150,300,450,600,750,1050,1350,1500,1650,1950,2100,2250,2550,2700,2850,3000,3150,3300,3450,3600,3750};
static const double PPh[21]={0,180,0,0,0,0,0,0,180,0,0,180,0,180,0,180,180,180,180,0,0};
static double mag_at(const short*x,int n,double f){double w=2*M_PI*f/SR,c=2*cos(w),s1=0,s2=0,s0;int k;for(k=0;k<n;k++){s0=x[k]+c*s1-s2;s2=s1;s1=s0;}return sqrt(s1*s1+s2*s2-c*s1*s2)/n;}
static double ph1200(const short*x,int a,int n){double w=2*M_PI*1200.0/SR,I=0,Q=0;int k;for(k=0;k<n;k++){double t=w*(a+k);I+=x[a+k]*cos(t);Q-=x[a+k]*sin(t);}return atan2(Q,I);}
enum { SIL, TONEB, INFOC, WIDE, OTHR };
static int classify(const short*x,int n){
    double rms=0;int k;for(k=0;k<n;k++)rms+=(double)x[k]*x[k];rms=sqrt(rms/n);
    if(rms<80)return SIL;
    double m1200=mag_at(x,n,1200.0);
    double hi=mag_at(x,n,3000.0)+mag_at(x,n,3300.0)+mag_at(x,n,3600.0)+mag_at(x,n,2700.0);
    double mid=mag_at(x,n,1950.0)+mag_at(x,n,2250.0);
    if(hi>300||(mid>300&&m1200<rms*0.3))return WIDE;
    if(m1200>rms*0.4){int sub=48,j,jumps=0;double prev=0;int have=0;
        for(j=0;j+sub<=n;j+=sub){double p=ph1200(x,j,sub);if(have){double d=p-prev;while(d>M_PI)d-=2*M_PI;while(d<-M_PI)d+=2*M_PI;if(fabs(d)>1.5)jumps++;}prev=p;have=1;}
        return(jumps>=3)?INFOC:TONEB;}
    return OTHR;
}

/* ---------- reactive answer-modem state machine ---------- */
/* §11.2.1.2 answer-modem: sync START to the modem's Tone B, then run the working
   slmodem's MEASURED ~2.9s choreography (don't over-wait; the modem follows in lockstep). */
enum { R_OPEN, R_SEQ, R_PRX, R_RANGE2, R_INFO1A, R_DONE, R_FAIL };
static const char *RN[]={"OPEN","SEQ","PRX","RANGE2","INFO1A","DONE","FAIL"};
enum { TX_INFO0A, TX_TONEA, TX_L1L2, TX_INFO1A, TX_SILENCE };

typedef struct {
    int state; long tstate;
    int txmode;
    unsigned char info0a[49], info1a[70];
    int info_bit, info_n, info_lastsym; long info_t; double info_symphase;
    long tx_t; double tonea_extra; int tx_rev_at;
    /* observation */
    int toneb_run, wide_run, info_run, saw_wide, got_brev, rev_sent;
    double tb_ph; int tb_have;
    short probe[24000]; int probe_len;
    int symrate;
    int retries;
    int seg;
    int last;
} V34Phase2;

void v34_phase2_init(V34Phase2 *p){
    memset(p,0,sizeof(*p));
    p->state=R_OPEN; p->txmode=TX_INFO0A; p->symrate=-1; p->last=-1; p->rev_sent=0;
    /* INFO1a field values COPIED from slmodem's decoded working frame on this line
       (infodec.py on sl-down.s16 @7.41s: pre=6 rate=9 sr=5/5, CRC OK — identical
       layout to ours, only these two values differed; the modem rejected pre=0/rate=14
       by silently never training). */
    info0a_build(p->info0a); info1a_build(p->info1a,5,5,6,9);
    p->info_n=49; p->info_bit=-1; p->info_lastsym=-1; p->tx_rev_at=-1; p->seg=-1;
}

/* continuous-phase TX for the current txmode */
static void p2_emit(V34Phase2 *p, short *out, int n){
    int k;
    for(k=0;k<n;k++){
        long t=p->tx_t+k; double v=0;
        if(p->txmode==TX_INFO0A||p->txmode==TX_INFO1A){
            const unsigned char*bits=(p->txmode==TX_INFO0A)?p->info0a:p->info1a;
            int nb=(p->txmode==TX_INFO0A)?49:70; double sp=SR/600.0;
            int sym=(int)(p->info_t/sp);
            if(sym!=p->info_lastsym){p->info_lastsym=sym;int bi=sym-1;p->info_bit=bi;if(bi>=0&&bi<nb&&bits[bi])p->info_symphase+=M_PI;}
            /* Measured from the working slmodem TX: INFO0a = DPSK@2400 + 1800 guard at
               rms~2300 (matches); INFO1a = PURE DPSK@2400, NO guard, rms~2890 (+2dB).
               Our guard tone during INFO1a sat in the caller's demod band — the likely
               reason it never accepted INFO1a. */
            if(p->txmode==TX_INFO1A)
                v=4085*cos(2*M_PI*2400.0*t/SR+p->info_symphase);
            else
                v=3000*cos(2*M_PI*2400.0*t/SR+p->info_symphase)+1300*cos(2*M_PI*1800.0*t/SR);
            p->info_t++;
        } else if(p->txmode==TX_TONEA){
            if(p->tx_rev_at>=0 && (p->tstate+k)==p->tx_rev_at) p->tonea_extra+=M_PI;
            /* PURE 2400 Hz, matched to slmodem (rms~3009, amp~4254). The 1800 Hz guard
               tone was spurious — real Tone A is a single tone; the modem's Tone A /
               phase-reversal detector needs it clean. */
            v=4254*cos(2*M_PI*2400.0*t/SR+p->tonea_extra);
        } else if(p->txmode==TX_SILENCE){
            v=0;   /* half-duplex turnaround — let the modem transmit */
        } else if(p->txmode==TX_L1L2){
            /* matched to slmodem L1/L2 rms~2898 (was ~2388, 2dB low) */
            int j;for(j=0;j<21;j++) v+=cos(2*M_PI*PF[j]*t/SR+PPh[j]*M_PI/180); v=v/21*4*4850;
        }
        out[k]=(short)v;
    }
    p->tx_t+=n;
}

/* set txmode + reset INFO streaming when entering a state */
static void set_tx(V34Phase2 *p, int mode){
    p->txmode=mode;
    if(mode==TX_INFO1A){ p->info_n=70; p->info_bit=-1; p->info_lastsym=-1; p->info_t=0; p->info_symphase=0; }
}

/* main process: returns 0 running, 1 done, -1 fail */
int v34_phase2_process(V34Phase2 *p, short *out, short *in, int n){
    if(p->state!=p->last){
        fprintf(stderr,"[v34p2] -> %s (toneb_run=%d saw_wide=%d brev=%d symrate=%d t=%ldms)\n",
                RN[p->state],p->toneb_run,p->saw_wide,p->got_brev,p->symrate,(long)(p->tstate/8));
        fflush(stderr); p->last=p->state;
    }
    p2_emit(p,out,n);
    p->tstate+=n;

    /* classify the modem's signal this block */
    int cls=classify(in,n);
    if(cls==TONEB) p->toneb_run++; else p->toneb_run=0;
    if(cls==WIDE){ p->wide_run++; p->saw_wide=1; } else p->wide_run=0;
    if(cls==INFOC) p->info_run++; else p->info_run=0;   /* modem DPSK = its INFO1c */
    if(cls==TONEB){
        double ph=ph1200(in,0,n);
        if(p->tb_have){double d=ph-p->tb_ph;while(d>M_PI)d-=2*M_PI;while(d<-M_PI)d+=2*M_PI;if(fabs(fabs(d)-M_PI)<0.6)p->got_brev=1;}
        p->tb_ph=ph; p->tb_have=1;
    } else p->tb_have=0;

    /* ================= REACTIVE Phase-2 DIALOGUE =================
       Each step WAITS for the modem's actual signal (via the classifier) before
       advancing — NOT a timed playlist. Our own transmissions are timed (we control
       them); ADVANCEMENT gates on the modem, exactly like V.8 waits for CJ.
       Generous timeouts are fallbacks so we never hang, but the primary path is to
       wait for the real reply. */
    double S=SR;
    switch(p->state){
    case R_OPEN:    /* §11.2.1.2: brief INFO0a, then HOLD steady Tone A and wait for the modem
                       to LOCK (sustained Tone B). The modem cycles INFO0c -> brief Tone B
                       attempts; it only commits to ranging when it finds our Tone A already
                       present (it answered our Tone A with Tone B in the captures). Holding
                       Tone A is robust to our V.8 entering Phase 2 ~1.5s late (variable lag).
                       NOTE: continuous INFO0a DEADLOCKED — it kept the modem in INFO0c. */
        if(p->txmode==TX_INFO0A){
            if(p->info_bit>=p->info_n){ if(p->tstate<(long)(0.3*S)){p->info_t=0;p->info_lastsym=-1;p->info_bit=-1;} else set_tx(p,TX_TONEA); }
        }
        if(p->txmode==TX_TONEA && p->toneb_run>=16){  /* modem's SUSTAINED ranging Tone B (>~0.35s),
                       NOT the brief early Tone B blips (run~12) it makes while finishing INFO0c.
                       Locking on a blip ran our sequence ~1.7s early and missed the modem's probe. */
            fprintf(stderr,"[v34p2] modem locked Tone B (run=%d) -> ranging sequence\n",p->toneb_run);fflush(stderr);
            p->state=R_SEQ; p->tstate=0; p->rev_sent=0; set_tx(p,TX_TONEA); p->probe_len=0; p->saw_wide=0;
        } else if(p->tstate>(long)(10.0*S)){          /* never locked -> try the sequence anyway */
            fprintf(stderr,"[v34p2] no Tone B lock (%.1fs) -> ranging anyway\n",p->tstate/S);fflush(stderr);
            p->state=R_SEQ; p->tstate=0; p->rev_sent=0; set_tx(p,TX_TONEA); p->probe_len=0; p->saw_wide=0;
        }
        if(p->tstate>(long)(15.0*S)) return -1;
        break;
    case R_SEQ: {   /* slmodem's MEASURED choreography, replayed at its real tempo.
                       Timings taken from the working sl-down.s16 (relative to Tone B):
                         0.00-0.30 ToneA + reversal #1   (slmodem rev @ +0.30)
                         0.30-0.70 L1 probe
                         0.70-1.05 ToneA + reversal #2   (slmodem rev @ +1.05)
                         1.05-1.65 L2 probe (hot)
                         1.65-2.25 silence turnaround    (hear the modem's probe -> symrate)
                         2.25-2.55 ToneA
                         2.55+     hand to Phase 3 training */
        long ms=(long)(p->tstate*1000/(long)S);
        /* Two Tone A phase reversals (like slmodem). Flip tonea_extra DIRECTLY on the
           threshold-crossing block — robust to block size; the old sample-exact arm
           window was skipped by block aliasing. Reversals sit mid-Tone-A so the modem
           has clean carrier before & after to detect the flip. tonea_extra persists
           across the L1 gap (continuous phase), so the carrier stays reversed. */
        /* REVERSAL TIMING (from golden-call diff): the caller must see our Tone A reversal
           WHILE it is transmitting Tone B (that's the RTD measurement). Its Tone B bursts
           are short — fire rev #1 IMMEDIATELY at lock (we locked BECAUSE it's on Tone B
           right now; +180ms was 60ms too late), and gate rev #2 on Tone B being present. */
        if(ms<350){            if(p->txmode!=TX_TONEA){set_tx(p,TX_TONEA);}
                               if(p->rev_sent==0){p->tonea_extra+=M_PI;p->rev_sent=1;fprintf(stderr,"[v34p2] Tone A reversal #1 @%ldms (toneb_run=%d)\n",ms,p->toneb_run);fflush(stderr);} }
        else if(ms<750){       if(p->txmode!=TX_L1L2){set_tx(p,TX_L1L2);} }
        else if(ms<1100){      if(p->txmode!=TX_TONEA){set_tx(p,TX_TONEA);}
                               if(p->rev_sent==1 && (p->toneb_run>=1 || ms>=1060)){p->tonea_extra+=M_PI;p->rev_sent=2;fprintf(stderr,"[v34p2] Tone A reversal #2 @%ldms (toneb_run=%d)\n",ms,p->toneb_run);fflush(stderr);} }
        else if(ms<1700){      if(p->txmode!=TX_L1L2){set_tx(p,TX_L1L2);} }
        else if(ms<2300){      if(p->txmode!=TX_SILENCE){set_tx(p,TX_SILENCE);} }
        else if(ms<2600){      if(p->txmode!=TX_TONEA){set_tx(p,TX_TONEA);} }
        /* Capture the modem's L1/L2 probe whenever it appears (its probe timing is its own;
           it landed AFTER our old narrow turnaround window). `in` is only the modem's signal,
           so accumulating WIDE here can't pick up our own TX. */
        if(ms>=1100 && cls==WIDE && p->probe_len+n<24000){ memcpy(p->probe+p->probe_len,in,n*sizeof(short)); p->probe_len+=n; }
        if(ms>=2600){
            /* Our probes are out. Captured call (lm-*.118089) shows the modem then takes ITS
               turn: silent while we probed, its ranging Tone B, its 1.7s L1/L2 probe, then it
               HOLDS Tone B (up to 18s!) waiting for our INFO1a. So: go SILENT and WAIT for its
               probe — do not barrel into Phase 3. */
            fprintf(stderr,"[v34p2] our probes sent -> waiting for modem's probe\n");fflush(stderr);
            p->state=R_PRX; p->tstate=0; set_tx(p,TX_SILENCE);
        }
        break;
    }
    case R_PRX:     /* capture the modem's L1/L2 probe. TIMING (from the working call): the
                       caller probes UNTIL it hears our Tone A — slmodem raises Tone A while
                       the probe is still running (probe: 0.57s with slmodem, 2.0s with our
                       old silent wait), then INFO1a lands 0.15s after probe end. Raise Tone A
                       once we've captured enough probe; go to INFO1a the moment it ends. */
        if(cls==WIDE && p->probe_len+n<24000){ memcpy(p->probe+p->probe_len,in,n*sizeof(short)); p->probe_len+=n; }
        if(p->txmode==TX_SILENCE && p->probe_len>(int)(0.45*S)){
            set_tx(p,TX_TONEA);
            fprintf(stderr,"[v34p2] Tone A during modem probe (terminate-L2 signal)\n");fflush(stderr);
        }
        if(p->saw_wide && p->wide_run==0 && p->probe_len>(int)(0.30*S)){
            double mx=0,top=0;int j;
            for(j=0;j<21;j++){double m=mag_at(p->probe,p->probe_len,PF[j]);if(m>mx)mx=m;}
            for(j=0;j<21;j++){double m=mag_at(p->probe,p->probe_len,PF[j]);if(m>mx*0.25&&PF[j]>top)top=PF[j];}
            p->symrate=(top>=3600)?5:(top>=3300)?4:(top>=3000)?3:(top>=2850)?2:(top>=2700)?1:0;
            fprintf(stderr,"[v34p2] modem probe RECEIVED (%dms, top %.0fHz) -> symrate %d; INFO1a now\n",
                    (int)(p->probe_len*1000/(int)S),top,p->symrate);fflush(stderr);
            p->state=R_INFO1A; p->tstate=0; p->retries=0; if(p->txmode!=TX_TONEA)set_tx(p,TX_TONEA);
        } else if(p->tstate>(long)(8.0*S)){
            if(p->symrate<0)p->symrate=5;
            fprintf(stderr,"[v34p2] no modem probe in 8s -> default symrate %d; INFO1a now\n",p->symrate);fflush(stderr);
            p->state=R_INFO1A; p->tstate=0; p->retries=0; if(p->txmode!=TX_TONEA)set_tx(p,TX_TONEA);
        }
        break;
    case R_RANGE2:  /* the modem holds Tone B after its probe when ranging is incomplete
                       (golden call: 22s hold!). Reverse Tone A WHILE it's on Tone B; it
                       answers with a Tone B reversal (RTD done) or leaves Tone B — then
                       and only then does INFO1a mean anything to it. Re-fire every 1s. */
        {long ms2=(long)(p->tstate*1000/(long)S);
        if(p->toneb_run>=2 && (p->seg<0 || ms2-p->seg>1000)){
            p->tonea_extra+=M_PI; p->seg=(int)ms2; p->got_brev=0;
            fprintf(stderr,"[v34p2] final Tone A reversal @%ldms (modem on Tone B)\n",ms2);fflush(stderr);
        }
        if(p->seg>=0 && p->got_brev){
            fprintf(stderr,"[v34p2] modem Tone B reversal -> ranging COMPLETE; sending INFO1a\n");fflush(stderr);
            p->state=R_INFO1A; p->tstate=0; p->retries=0; set_tx(p,TX_TONEA);
        } else if(p->seg>=0 && ms2-p->seg>400 && p->toneb_run==0 && cls!=TONEB){
            fprintf(stderr,"[v34p2] modem left Tone B after reversal -> sending INFO1a\n");fflush(stderr);
            p->state=R_INFO1A; p->tstate=0; p->retries=0; set_tx(p,TX_TONEA);
        } else if(p->tstate>(long)(8.0*S)){
            fprintf(stderr,"[v34p2] final ranging timeout -> sending INFO1a anyway\n");fflush(stderr);
            p->state=R_INFO1A; p->tstate=0; p->retries=0; set_tx(p,TX_TONEA);
        }
        break;}
    case R_INFO1A:  /* Tone A ~0.3s, INFO1a TWICE back-to-back, then IMMEDIATE Phase-3
                       handoff. Decoded from the working recording: slmodem sends ONE
                       INFO1a frame and then floods Phase-3 TRN; the modem's ~2.4s
                       post-reversal silence is its EQUALIZER TRAINING window, not an
                       INFO1a listen loop. Every earlier attempt filled that window with
                       INFO1a repeats and delivered training signal only after the modem
                       had given up. Get TRN on the wire inside the window. */
        if(p->txmode==TX_TONEA && p->tstate>(long)(0.20*S)){   /* slmodem: INFO1a 0.15s after probe end */
            set_tx(p,TX_INFO1A);
            fprintf(stderr,"[v34p2] INFO1a x2 then Phase 3\n");fflush(stderr);
        }
        if(p->txmode==TX_INFO1A && p->info_bit>=p->info_n){
            /* INFO1a is sent ONCE (spec; slmodem's burst = exactly 1 frame). Our old 2nd
               frame sat exactly where the caller — synced to frame 1 — expects the 70ms
               silence + S, breaking its S detector. */
            set_tx(p,TX_SILENCE); p->rev_sent=99; p->tstate=0;   /* spec 11.3.1.2.2: 70±5ms silence */
            fprintf(stderr,"[v34p2] INFO1a sent (x1) -> 70ms silence -> Phase 3\n");fflush(stderr);
        }
        if(p->rev_sent==99 && p->tstate>(long)(0.070*S)){ p->state=R_DONE; return 1; }
        break;
    }
    return 0;
}

/* ---------- opaque API ---------- */
void *v34_phase2_new(void){V34Phase2*p=malloc(sizeof(V34Phase2));if(p)v34_phase2_init(p);return p;}
int v34_phase2_run(void*p,short*out,short*in,int n){return v34_phase2_process((V34Phase2*)p,out,in,n);}
int v34_phase2_symrate(void*p){return ((V34Phase2*)p)->symrate;}
void v34_phase2_free(void*p){free(p);}

#ifdef P2_TEST
/* offline replay against a captured modem upstream */
int main(int argc,char**argv){
    if(argc<2){fprintf(stderr,"usage: %s cap.s16\n",argv[0]);return 1;}
    FILE*f=fopen(argv[1],"rb");static short s[2000000];int n=fread(s,2,2000000,f);fclose(f);
    V34Phase2 p; v34_phase2_init(&p);
    short out[160];
    FILE *tx=fopen("/tmp/p2tx.s16","wb");   /* dump OUR generated TX for offline verify */
    for(int k=0;k+160<=n;k+=160){
        int r=v34_phase2_process(&p,out,s+k,160);
        if(tx)fwrite(out,2,160,tx);
        if(r==1){if(tx)fclose(tx);printf("DONE: symrate idx=%d\n",p.symrate);return 0;}
        if(r==-1){if(tx)fclose(tx);printf("FAIL state %s\n",RN[p.state]);return 1;}
    }
    if(tx)fclose(tx);
    printf("end of capture, state %s symrate=%d\n",RN[p.state],p.symrate);
    return 0;
}
#endif
