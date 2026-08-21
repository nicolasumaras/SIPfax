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
    /* SIPFAX: the caller's INFO (INFO0c/INFO1c) is 600bps DPSK on a 1200Hz carrier, so
       its energy sits in ~900-1500Hz and the CARRIER ITSELF IS SUPPRESSED - measured
       m1200 = 3..578 against rms 1750, far under the rms*0.4 gate below. It therefore
       fell through to OTHR (or to WIDE via the mid test) and we never saw INFO1c at all:
       we talked over it, the caller re-sent it for 16s, and the handshake stalled. The
       band sum is decisive - measured 1644..2283 vs a 0.5*rms threshold of ~880 - and
       the probe comb is excluded first by hi (it omits 900/1200/1800/2400 but carries
       3000/3300/3600). */
    double inf=mag_at(x,n,900.0)+mag_at(x,n,1050.0)+mag_at(x,n,1350.0)+mag_at(x,n,1500.0);
    if(hi>300)return WIDE;
    if(inf>rms*0.5)return INFOC;
    if(mid>300&&m1200<rms*0.3)return WIDE;
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
    long tx_t;
    long l1_t;       /* SIPFAX: samples into the current probe; <160ms => L1 (+6dB) */ double tonea_extra; int tx_rev_at;
    /* observation */
    int toneb_run, wide_run, info_run, saw_wide, got_brev, rev_sent;
    /* SIPFAX reactive ranging (11.2.1.2.4-6): sub-block Tone-B reversal tracking */
    double bref_re, bref_im; int bref_n, brev_cnt, brev_base, rseq_ph;
    long brev_at, rev2_due;
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
            if(p->tx_rev_at>=0 && (p->tstate+k)>=p->tx_rev_at){ p->tonea_extra+=M_PI; p->tx_rev_at=-1; }
            /* PURE 2400 Hz, matched to slmodem (rms~3009, amp~4254). The 1800 Hz guard
               tone was spurious — real Tone A is a single tone; the modem's Tone A /
               phase-reversal detector needs it clean. */
            v=4254*cos(2*M_PI*2400.0*t/SR+p->tonea_extra);
        } else if(p->txmode==TX_SILENCE){
            v=0;   /* half-duplex turnaround — let the modem transmit */
        } else if(p->txmode==TX_L1L2){
            /* matched to slmodem L1/L2 rms~2898 (was ~2388, 2dB low).
               SIPFAX: spec 10.1.3.4 - L1 is the FIRST 160 ms of probing at 6 dB ABOVE
               nominal; the rest (L2) is nominal. This generator used one amplitude for
               everything, so our probe had no L1 marker at all: wire-measured dead flat
               rms 2989 in every call, while slmodem and the caller both transmit a
               160 ms head at +5.8-6.0 dB. A receiver that windows its per-tone L2
               measurement relative to the L1 level step gets garbage from us. Keep the
               reverse-engineered slot timing exactly as it is (the caller's ranging
               interactions are tuned to it) and add the boost to the head only. */
            double a = (p->l1_t < (long)(0.160*SR)) ? 2.0 : 1.0;
            int j;for(j=0;j<21;j++) v+=cos(2*M_PI*PF[j]*t/SR+PPh[j]*M_PI/180); v=v/21*4*4850*a;
            p->l1_t++;
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

/* SIPFAX: sub-block Tone-B reversal detector (V.34 11.2.1.2.4/.6). The caller keeps
   Tone B only ~10 ms after it reverses, so the whole-block phase latch (got_brev)
   catches the flip at ~25% of block alignments and 0% when L1 follows - measured on
   real call audio. 20-sample windows are exactly 3 cycles of 1200 Hz at 8 kHz, so each
   yields a clean phase: windows coherent with the running reference extend it, one in
   anti-phase latches a reversal (brev_cnt++, brev_at = sample time) and re-references,
   so the SECOND reversal after our rev#3 is caught the same way. Callers must gate this
   off WIDE/INFOC blocks - a 2.5 ms window cannot separate 1200 Hz from the probe comb's
   1050/1350 neighbours or from INFO0c DPSK. */
static void brev_scan(V34Phase2 *p, short *in, int n){
    long base = p->tstate - n;
    int w, i;
    for(w = 0; w + 20 <= n; w += 20){
        double re = 0, im = 0, a;
        for(i = 0; i < 20; i++){
            a = 2*M_PI*3.0*(double)((base + w + i) % 20)/20.0;
            re += in[w+i]*cos(a); im -= in[w+i]*sin(a);
        }
        {
            double mag = sqrt(re*re + im*im)/20.0;
            if(mag < 300) continue;
            if(p->bref_n < 6){ p->bref_re += re; p->bref_im += im; p->bref_n++; continue; }
            {
                double dot = re*p->bref_re + im*p->bref_im;
                double nm  = sqrt((re*re + im*im)*(p->bref_re*p->bref_re + p->bref_im*p->bref_im));
                double c   = nm > 0 ? dot/nm : 0;
                if(c > 0.6){ p->bref_re = 0.9*p->bref_re + re; p->bref_im = 0.9*p->bref_im + im; }
                else if(c < -0.6){
                    /* refractory: a real ranging reversal happens once; DPSK blocks that
                       sneak past the INFOC gate (<3 jumps) alternate every window and
                       produced a 32-latch storm live. One latch per 200 ms, and the
                       reference must be rebuilt from 6 fresh coherent windows before the
                       detector can fire again. */
                    if(base + w - p->brev_at < (long)(0.200*SR) && p->brev_cnt > 0){
                        p->bref_n = 0; p->bref_re = p->bref_im = 0;
                        continue;
                    }
                    p->brev_cnt++; p->brev_at = base + w;
                    p->bref_n = 0; p->bref_re = p->bref_im = 0;
                    fprintf(stderr,"[v34p2] caller B-REVERSAL #%d (sub-block, cos=%.2f)\n", p->brev_cnt, c);
                    fflush(stderr);
                }
            }
        }
    }
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
    if(p->state==R_SEQ && cls!=WIDE && cls!=INFOC) brev_scan(p, in, n);

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
            p->rseq_ph=0; p->rev2_due=-1; p->bref_re=p->bref_im=0; p->bref_n=0; p->brev_cnt=0; p->brev_base=0;
        } else if(p->tstate>(long)(10.0*S)){          /* never locked -> try the sequence anyway */
            fprintf(stderr,"[v34p2] no Tone B lock (%.1fs) -> ranging anyway\n",p->tstate/S);fflush(stderr);
            p->state=R_SEQ; p->tstate=0; p->rev_sent=0; set_tx(p,TX_TONEA); p->probe_len=0; p->saw_wide=0;
            p->rseq_ph=0; p->rev2_due=-1; p->bref_re=p->bref_im=0; p->bref_n=0; p->brev_cnt=0; p->brev_base=0;
        }
        if(p->tstate>(long)(15.0*S)) return -1;
        break;
    case R_SEQ: {   /* SIPFAX: REACTIVE ranging per V.34 11.2.1.2.3-7, replacing a scripted
           replay of a MISIDENTIFIED recording (its "L1 at 0.30-0.70" was actually
           INFO0a; the real slmodem probes only after the second B-reversal). The old
           scripted rev#2 fired from a wall-clock fallback 914 ms after the caller's
           B-reversal instead of the spec's 40+/-1 ms - and that interval IS the
           caller's round-trip-delay estimate (11.2.1.1.4), which places its far-echo
           canceller. Every failing call handed it RTDEc ~ 960 ms vs ~67 ms in the
           working call; its recovery timing proves it operates on the corrupted value.
           Spec sequence (verified against slmodem's working wire): ToneA + rev#1 ->
           caller B-rev -> OUR rev#2 exactly 40 ms after receiving it -> ToneA 10 ms ->
           L1(160 ms, +6 dB)+L2 contiguous -> ToneA 50 ms -> rev#3 -> 10 ms -> silence
           -> caller's 2nd B-rev -> its probe (R_PRX). Timeout fallbacks on every wait
           so a missed detection can never outlast the caller's 2000 ms 11.2.2.1.3
           restart. */
        long ms=(long)(p->tstate*1000/(long)S);
        switch(p->rseq_ph){
        case 0:     /* Tone A on; rev#1 immediately at lock (spec-legal; caller reacts) */
            if(p->txmode!=TX_TONEA) set_tx(p,TX_TONEA);
            if(p->rev_sent==0){ p->tonea_extra+=M_PI; p->rev_sent=1;
                fprintf(stderr,"[v34p2] Tone A reversal #1 @%ldms\n",ms);fflush(stderr); }
            p->brev_base=p->brev_cnt; p->rseq_ph=1;
            break;
        case 1:     /* wait for the caller's B-reversal; rev#2 sample-exact +40 ms */
            if(p->brev_cnt>p->brev_base && p->rev2_due<0){
                p->rev2_due = p->brev_at + (long)(0.040*S);
                p->tx_rev_at = p->rev2_due;
                fprintf(stderr,"[v34p2] B-rev at t=%ld -> reactive rev#2 armed for t=%ld (+40ms)\n",
                        p->brev_at, p->rev2_due);fflush(stderr);
            }
            if(p->rev2_due>=0 && p->tstate >= p->rev2_due + 160){
                p->rev_sent=2; p->seg=(int)p->tstate; p->rseq_ph=2;
            } else if(ms>=1800){    /* fallback: never hang ranging */
                p->tonea_extra+=M_PI; p->rev_sent=2; p->seg=(int)p->tstate; p->rseq_ph=2;
                fprintf(stderr,"[v34p2] rev#2 FALLBACK @%ldms (no B-reversal seen)\n",ms);fflush(stderr);
            }
            break;
        case 2:     /* Tone A 10 ms after rev#2, then L1+L2 contiguous */
            if(p->tstate >= p->seg + (long)(0.010*S)){
                set_tx(p,TX_L1L2); p->l1_t=0; p->seg=(int)p->tstate; p->rseq_ph=3;
                fprintf(stderr,"[v34p2] L1(160ms,+6dB)+L2 after reversals (11.2.1.2.5)\n");fflush(stderr);
            }
            break;
        case 3:     /* L1 160 ms + L2 600 ms, then SILENCE - the caller's probe follows */
            if(p->tstate >= p->seg + (long)(0.760*S)){
                set_tx(p,TX_SILENCE); p->seg=(int)p->tstate; p->probe_len=0; p->saw_wide=0; p->rseq_ph=4;
                fprintf(stderr,"[v34p2] L2 done -> silent, waiting for caller probe\n");fflush(stderr);
            }
            break;
        case 4:     /* The caller probes next. Tone A is its probe-TERMINATE signal, so it
                       must rise only once the probe is well underway (>=450ms, matching
                       slmodem/R_PRX) - raising it at our L2 end made the caller abort its
                       probe at 240ms and the handshake died with no white-noise phase.
                       rev#3 fires at the probe end (11.2.1.2.6) with >=50ms of Tone A
                       already up; the caller's B-rev#2 answers ~100ms later. */
            if(p->txmode==TX_SILENCE && p->wide_run>0 && p->probe_len>(int)(0.45*S)){
                set_tx(p,TX_TONEA); p->seg=(int)p->tstate;
                fprintf(stderr,"[v34p2] Tone A during caller probe (terminate-L2)\n");fflush(stderr);
            }
            if((p->txmode==TX_TONEA && p->tstate >= p->seg + (long)(0.050*S)
                && p->saw_wide && p->wide_run==0 && cls!=WIDE && p->probe_len>(int)(0.45*S))
               || p->tstate >= p->seg + (long)(4.0*S)){
                if(p->txmode!=TX_TONEA){ set_tx(p,TX_TONEA); p->seg=(int)p->tstate; break; }
                p->tonea_extra+=M_PI; p->rev_sent=3; p->seg=(int)p->tstate; p->rseq_ph=5;
                fprintf(stderr,"[v34p2] Tone A reversal #3 at caller probe end (probe %dms)\n",
                        (int)(p->probe_len*1000/(int)S));fflush(stderr);
            }
            break;
        case 5:     /* 10 ms Tone A, then silence; arm for the caller's 2nd B-reversal */
            if(p->tstate >= p->seg + (long)(0.010*S)){
                set_tx(p,TX_SILENCE); p->brev_base=p->brev_cnt;
                p->seg=(int)p->tstate; p->rseq_ph=6;
            }
            break;
        case 6:     /* Ranging is over the moment the caller's probe has ended or its
                       INFO1c starts. Waiting a fixed 1.5s for a 2nd B-reversal pushed
                       our INFO1a 1.4s past the caller's INFO1c, so it never got the
                       reply it was waiting for and re-sent INFO1c for 16s. */
            if(p->brev_cnt>p->brev_base || cls==INFOC
               || (p->saw_wide && p->wide_run==0 && p->probe_len>(int)(0.30*S))
               || p->tstate >= p->seg + (long)(0.40*S)){
                fprintf(stderr,"[v34p2] ranging complete (%s) -> caller's turn\n",
                        p->brev_cnt>p->brev_base ? "B-rev#2" : (cls==INFOC ? "INFO1c" : "probe end"));
                fflush(stderr);
                p->state=R_PRX; p->tstate=0; p->retries=0; set_tx(p,TX_SILENCE);
            }
            break;
        }
        /* capture the modem's probe whenever it appears; `in` is only the modem's signal */
        if(cls==WIDE && p->probe_len+n<24000){ memcpy(p->probe+p->probe_len,in,n*sizeof(short)); p->probe_len+=n; }
        break;
    }
    case R_PRX:     /* capture the modem's L1/L2 probe. TIMING (from the working call): the
                       caller probes UNTIL it hears our Tone A — slmodem raises Tone A while
                       the probe is still running (probe: 0.57s with slmodem, 2.0s with our
                       old silent wait), then INFO1a lands 0.15s after probe end. Raise Tone A
                       once we've captured enough probe; go to INFO1a the moment it ends. */
        if(cls==WIDE && p->probe_len+n<24000){ memcpy(p->probe+p->probe_len,in,n*sizeof(short)); p->probe_len+=n; }
        if(p->retries==0 && p->txmode==TX_SILENCE && p->wide_run>0 && p->probe_len>(int)(0.45*S)){
            set_tx(p,TX_TONEA);
            fprintf(stderr,"[v34p2] Tone A during modem probe (terminate-L2 signal)\n");fflush(stderr);
        }
        if(p->retries==0 && p->saw_wide && p->wide_run==0 && p->probe_len>(int)(0.30*S)){
            double mx=0,top=0;int j;
            for(j=0;j<21;j++){double m=mag_at(p->probe,p->probe_len,PF[j]);if(m>mx)mx=m;}
            for(j=0;j<21;j++){double m=mag_at(p->probe,p->probe_len,PF[j]);if(m>mx*0.25&&PF[j]>top)top=PF[j];}
            p->symrate=(top>=3600)?5:(top>=3300)?4:(top>=3000)?3:(top>=2850)?2:(top>=2700)?1:0;
            fprintf(stderr,"[v34p2] modem probe RECEIVED (%dms, top %.0fHz) -> symrate %d; awaiting INFO1c\n",
                    (int)(p->probe_len*1000/(int)S),top,p->symrate);fflush(stderr);
            /* SIPFAX: the caller sends INFO1c next and WAITS for our INFO1a. Go silent
               and let it speak - firing INFO1a before/over its INFO1c left it re-sending
               INFO1c indefinitely while we sat muted in Phase-3 WAIT_J. */
            p->retries=1; set_tx(p,TX_SILENCE);
        }
        if(p->retries==1 && cls==INFOC){
            p->retries=2;
            fprintf(stderr,"[v34p2] caller INFO1c arriving\n");fflush(stderr);
        }
        if(p->retries==2 && p->info_run==0){
            fprintf(stderr,"[v34p2] caller INFO1c done -> our INFO1a\n");fflush(stderr);
            p->state=R_INFO1A; p->tstate=0; p->retries=0; set_tx(p,TX_SILENCE);
        }
        if(p->tstate>(long)(2.5*S)){
            if(p->symrate<0)p->symrate=5;
            fprintf(stderr,"[v34p2] no INFO1c in 2.5s -> default symrate %d; INFO1a now\n",p->symrate);fflush(stderr);
            p->state=R_INFO1A; p->tstate=0; p->retries=0; set_tx(p,TX_SILENCE);
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
        /* SIPFAX: slmodem's measured reply to INFO1c - 0.5s silence, then Tone A
           0.3s, then ONE INFO1a, then the 70ms gap into Phase 3. */
        if(p->txmode==TX_SILENCE && p->rev_sent!=99 && p->tstate>(long)(0.50*S)){
            set_tx(p,TX_TONEA);
            fprintf(stderr,"[v34p2] Tone A 0.3s before INFO1a (slmodem shape)\n");fflush(stderr);
        }
        if(p->txmode==TX_TONEA && p->tstate>(long)(0.80*S)){
            set_tx(p,TX_INFO1A);
            fprintf(stderr,"[v34p2] INFO1a -> Phase 3\n");fflush(stderr);
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
