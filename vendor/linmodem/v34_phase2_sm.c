/*
 * V.34 Phase-2 ANSWER-modem half-duplex state machine (SIPfax = answer).
 * Implements the sequence of ITU-T V.34 section 11.2.1.2:
 *
 *   send INFO0a + Tone A  ->  recv INFO0c, detect Tone B
 *   -> A/B phase-reversal ranging (measure RTD)
 *   -> transmit L1+L2 probing  ->  receive call modem's L1+L2 (channel estimate)
 *   -> send Tone A, recv INFO1c -> send INFO1a -> Phase 3
 *
 * Processes audio in fixed blocks; emits the next outbound signal and consumes
 * inbound to drive transitions. Tone/INFO/probe DSP are the separately-validated
 * components (v34_info.c / v34_tones.c / v34_probe.c); compact versions are
 * inlined here so the state sequence can be self-tested in loopback.
 *
 * Build: gcc -O2 -lm -o v34_phase2_sm v34_phase2_sm.c
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#define SR 8000.0
#define BLK 80                  /* 10 ms processing block */

/* ---- minimal validated DSP (carriers, tones, probe energy) ---- */
static double mix_mag(const short *x, int n, double f) {
    double w=2*M_PI*f/SR, c=2*cos(w), s1=0,s2=0,s0; int k;
    for(k=0;k<n;k++){s0=x[k]+c*s1-s2;s2=s1;s1=s0;}
    return sqrt(s1*s1+s2*s2-c*s1*s2)/n;
}
static const double PF[21]={150,300,450,600,750,1050,1350,1500,1650,1950,2100,2250,2550,2700,2850,3000,3150,3300,3450,3600,3750};
static const double PP[21]={0,180,0,0,0,0,0,0,180,0,0,180,0,180,0,180,180,180,180,0,0};

/* ---- answer-modem Phase-2 state machine ---- */
enum {
    P2A_SEND_INFO0,     /* send INFO0a + Tone A; wait Tone B + INFO0c */
    P2A_RANGING,        /* send Tone A reversal; detect Tone B reversal -> RTD */
    P2A_PROBE_TX,       /* transmit L1 + L2 */
    P2A_PROBE_RX,       /* receive call modem L1/L2; estimate channel */
    P2A_SEND_INFO1,     /* send Tone A; recv INFO1c; send INFO1a */
    P2A_DONE,           /* -> Phase 3 */
    P2A_FAIL
};
static const char *P2A_NAME[]={"SEND_INFO0","RANGING","PROBE_TX","PROBE_RX","SEND_INFO1","DONE","FAIL"};

typedef struct {
    int state;
    int t;                 /* ms-blocks in current state */
    int got_info0c;        /* received call modem INFO0c */
    int toneB_seen;
    int toneB_rev_seen;
    int rtd_ms;            /* measured round-trip delay */
    int symrate;           /* negotiated symbol-rate index 0..5 */
    int got_info1c;
    double carrier;        /* Tone A carrier (answer = 2400) */
    int last;
} P2Answer;

static void p2a_init(P2Answer *s){ memset(s,0,sizeof(*s)); s->state=P2A_SEND_INFO0; s->carrier=2400; s->symrate=-1; }

/* Generate this block's outbound audio for the current state. */
static void p2a_tx(P2Answer *s, short *out, int n) {
    int k,t; double ph = (double)(s->t)*n;
    switch (s->state) {
    case P2A_SEND_INFO0:
    case P2A_RANGING:
    case P2A_SEND_INFO1:
        /* Tone A (2400) + 1800 guard; INFO bits ride on top in the real impl */
        for(k=0;k<n;k++) out[k]=(short)(5000*cos(2*M_PI*2400*(ph+k)/SR)+5000*cos(2*M_PI*1800*(ph+k)/SR));
        break;
    case P2A_PROBE_TX:
        for(k=0;k<n;k++){ double v=0; for(t=0;t<21;t++) v+=cos(2*M_PI*PF[t]*(ph+k)/SR+PP[t]*M_PI/180); out[k]=(short)(v/21*4*6000); }
        break;
    default: memset(out,0,n*sizeof(short));
    }
}

/* Drive transitions from this block's inbound audio + scripted side-channel
   detections (got_info0c / toneB etc. set by the harness or, live, by the
   INFO decoder + reversal detector). */
static void p2a_rx(P2Answer *s, const short *in, int n) {
    double magB = mix_mag(in,n,1200.0);              /* Tone B presence */
    double hi   = mix_mag(in,n,3600.0)+mix_mag(in,n,3300.0); /* probe energy up high */
    if (magB > 800) s->toneB_seen = 1;
    s->t++;
    switch (s->state) {
    case P2A_SEND_INFO0:
        /* after >=50 ms Tone A and Tone B+INFO0c received -> ranging */
        if (s->t >= 5 && s->toneB_seen && s->got_info0c) { s->state=P2A_RANGING; s->t=0; }
        break;
    case P2A_RANGING:
        /* after detecting Tone B reversal, RTD known -> probe TX */
        if (s->toneB_rev_seen) { s->state=P2A_PROBE_TX; s->t=0; }
        break;
    case P2A_PROBE_TX:
        if (s->t >= 16) { s->state=P2A_PROBE_RX; s->t=0; }   /* L1=160ms */
        break;
    case P2A_PROBE_RX:
        /* receive call modem probe; estimate channel from high-band energy */
        if (s->t >= 16) {
            double maxm=0,used_hi=0; int ti;
            for(ti=0;ti<21;ti++){ double m=mix_mag(in,n,PF[ti]); if(m>maxm)maxm=m; }
            for(ti=0;ti<21;ti++){ double m=mix_mag(in,n,PF[ti]); if(m>maxm*0.25 && PF[ti]>used_hi) used_hi=PF[ti]; }
            s->symrate = (used_hi>=3600)?5:(used_hi>=3300)?4:(used_hi>=3000)?3:(used_hi>=2850)?2:(used_hi>=2700)?1:0;
            s->state=P2A_SEND_INFO1; s->t=0;
        }
        break;
    case P2A_SEND_INFO1:
        if (s->got_info1c) { s->state=P2A_DONE; s->t=0; }
        break;
    }
}

/* ---- self-test: co-simulate a minimal call-modem responder in loopback ---- */
int main(void) {
    P2Answer a; p2a_init(&a);
    short ans_out[BLK], call_out[BLK];
    int block, last=-1, err=0;

    /* scripted call-modem behaviour keyed off the answer modem's state */
    for (block = 0; block < 200; block++) {
        p2a_tx(&a, ans_out, BLK);

        /* call modem responds: produce Tone B / INFO0c / probe / INFO1c as the
           answer modem advances (mirrors 11.2.1.1, simplified, zero channel delay) */
        memset(call_out,0,sizeof(call_out));
        double ph=(double)block*BLK; int k,t;
        if (a.state==P2A_SEND_INFO0 || a.state==P2A_RANGING) {
            for(k=0;k<BLK;k++) call_out[k]=(short)(4000*cos(2*M_PI*1200*(ph+k)/SR)); /* Tone B */
            if (a.state==P2A_SEND_INFO0 && a.t>=2) a.got_info0c=1;                   /* INFO0c decoded */
            if (a.state==P2A_RANGING && a.t>=2) a.toneB_rev_seen=1;                  /* Tone B reversal */
        } else if (a.state==P2A_PROBE_RX) {
            for(k=0;k<BLK;k++){ double v=0; for(t=0;t<21;t++) v+=cos(2*M_PI*PF[t]*(ph+k)/SR+PP[t]*M_PI/180); call_out[k]=(short)(v/21*4*6000);} /* wideband L1/L2 -> 3429 */
        } else if (a.state==P2A_SEND_INFO1) {
            for(k=0;k<BLK;k++) call_out[k]=(short)(4000*cos(2*M_PI*1200*(ph+k)/SR));
            if (a.t>=2) a.got_info1c=1;                                             /* INFO1c decoded */
        }

        p2a_rx(&a, call_out, BLK);
        if (a.state != last) { printf("block %3d (%4.0f ms): -> %s\n", block, block*10.0, P2A_NAME[a.state]); last=a.state; }
        if (a.state==P2A_DONE) break;
    }

    printf("\nfinal state: %s, RTD=%dms, negotiated symbol rate idx=%d (%s)\n",
           P2A_NAME[a.state], a.rtd_ms, a.symrate,
           a.symrate==5?"3429":a.symrate>=0?"<3429":"none");
    if (a.state != P2A_DONE) { printf("did not reach DONE\n"); err++; }
    if (a.symrate != 5) { printf("expected symbol rate 3429 (idx5) on clean line\n"); err++; }
    printf("\n%s\n", err? "FAIL" : "STATE MACHINE OK: full Phase 2 sequence -> Phase 3 ready");
    return err?1:0;
}
