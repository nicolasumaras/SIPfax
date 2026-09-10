/* Digital V.90 Phase4: live CPt/CP, Ri/Ri-bar, TRN2d and MP. GPL-2.0. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "v90phase4.h"
static void mp_build(V90Phase4 *s)
{
    memset(s->mp,0,sizeof(s->mp));memset(s->mp,1,17);
    s->mp[25]=1; /* Development upstream receiver: 4800 bit/s, 16-state trellis. */
    s->mp[36]=1;s->mp[33]=s->mp_ack;
    unsigned crc=0xffff;
    for(unsigned j=18;j<69;++j) {
        if(j%17==0)continue;
        unsigned feedback=(crc^s->mp[j])&1;crc>>=1;
        if(feedback)crc^=0x8408;
    }
    for(unsigned j=0;j<16;++j)s->mp[69+j]=(crc>>j)&1;
}
static int training_bit(void *opaque)
{
    V90Phase4 *s=opaque;
    unsigned n=s->generated++,d=s->encoder.k+s->encoder.s;
    if(n<s->trn_frames*d)return 1;
    unsigned index=(n-s->trn_frames*d)%s->mp_length;
    if(!s->ed_frame && !index && s->have_cp &&
       (s->have_ack || (!s->cp.silence && s->rx.e_seen)) && s->mp_ack)s->ed_frame=n/d;
    if(s->ed_frame && n>=s->ed_frame*d)return 0;
    if(!index && s->have_cp && !s->mp_ack){s->mp_ack=1;mp_build(s);}
    return s->mp[index];
}
static int data_bit(void *opaque)
{
    V90Phase4 *s=opaque;
    if(s->data_bits++<48*(s->encoder.k+s->encoder.s))return 1;
    if(s->stage!=4)return 1; /* DTE clamped as soon as S is recognized. */
    return s->get_data_bit?s->get_data_bit(s->data_opaque):1;
}
static unsigned training_frames(const char *name,long minimum_ms)
{
    const char *setting=getenv(name);
    if(setting && *setting) {
        char *end;long ms=strtol(setting,&end,10);
        if(!*end && ms>=minimum_ms && ms<=2000)return (unsigned)(ms*8/6);
    }
    return 340;
}
void v90_phase4_init(V90Phase4 *s,int alaw,int uinfo)
{
    memset(s,0,sizeof(*s));s->alaw=alaw;s->uinfo=uinfo;
    /* 9.4.1.2/3: at least 2040 samples, MP begins within 2000ms.
       Round down to complete six-sample frames. */
    s->trn_frames=training_frames("SIPFAX_V90_INITIAL_TRN2D_MS",255);
    v90_training_init(&s->rx);s->rx.cp_mode=1;v90_upstream_init(&s->upstream);
    fprintf(stderr,"[v90p4] transmit Ri; receive CPt\n");
}
static void receive_cp(V90Phase4 *s)
{
    if(!s->rx.cp.type && !s->have_cpt) {
        s->cpt=s->rx.cp;s->have_cpt=1;
        fprintf(stderr,"[v90p4] live CRC-valid CPt: drn=%u Sr=%u ld=%u gain=%u masks=%u at %.6fs\n",s->cpt.drn,s->cpt.sr,s->cpt.lookahead,s->cpt.gain,s->cpt.count,s->samples/8000.0);
    } else if(s->rx.cp.type) {
        if(s->stage==4)return; /* Do not silently change active CP. */
        s->cp=s->rx.cp;
        if(!s->have_cp || (!s->have_ack && s->cp.ack))
            fprintf(stderr,"[v90p4] live CRC-valid CP%s: downstream=%u/3 bit/s Sr=%u ack=%u at %.6fs\n",s->cp.ack?"-prime":"",(s->cp.drn+20)*4000,s->cp.sr,s->cp.ack,s->samples/8000.0);
        s->have_cp=1;if(s->cp.ack)s->have_ack=1;
        if(s->stage==7 && !s->cp.silence) {
            /* 9.6.1.2.6: leave silence only on a fresh non-silence CP.
               Retain data-frame alignment for the following Rt. */
            s->stage=8;s->rt_start=0;
            s->have_ack=s->mp_ack=0;
            s->generated=s->ed_frame=s->mp_announced=0;
            s->trn_frames=0;s->rx.e_seen=s->rx_e_logged=0;
            mp_build(s);
            fprintf(stderr,"[v90p4] CP clears silence; transmit Rt at next frame boundary\n");
        } else if(s->cp.silence && !s->renegotiations) {
            s->stage=3;
            fprintf(stderr,"[v90p4] CPs outside rate renegotiation; await retrain\n");
        }
    }
}
int16_t v90_phase4_next(V90Phase4 *s,int16_t input)
{
    if(s->stage==4 || s->stage==5) {
        int event=v90_s_detect(&s->rate_detector,input);
        if(event==1 && s->stage==4) {
            s->preceding_cp=s->cp;s->stage=5;
            v90_training_init(&s->rx);s->rx.cp_mode=1;
            s->have_cp=s->have_ack=s->mp_ack=s->rx_e_logged=0;
            s->generated=s->ed_frame=s->mp_announced=s->reneg_start=0;
            /* 9.6.1.2.2 permits optional TRN2d up to 2000ms. */
            s->trn_frames=training_frames("SIPFAX_V90_RENEG_TRN2D_MS",0);
            ++s->renegotiations;
            fprintf(stderr,"[v90p4] rate renegotiation S; clamp DTE at %.6fs\n",s->samples/8000.0);
        }
        if(event==2 && s->stage==5) {
            s->stage=6;
            fprintf(stderr,"[v90p4] rate renegotiation Sbar at %.6fs\n",s->samples/8000.0);
        }
    }
    int count=s->rx.found;v90_training_receive(&s->rx,&input,1);
    if(count!=s->rx.found)receive_cp(s);
    if(s->rx.e_seen && !s->rx_e_logged) {
        /* Upstream B1 follows E independently of our downstream Ed.
           Reset here so an early E cannot lose B1 at Ed completion. */
        void (*receive_frame)(void *,const uint8_t *,unsigned)=s->upstream.receive_frame;
        void *opaque=s->upstream.opaque;
        v90_upstream_init(&s->upstream);s->upstream.require_b1=1;
        s->upstream.receive_frame=receive_frame;s->upstream.opaque=opaque;
        s->rx_e_logged=1;fprintf(stderr,"[v90p4] upstream E detected at %.6fs; starting upstream B1/data receiver\n",s->samples/8000.0);
    }
    if(s->stage==2 && s->ed_frame && s->samples-s->trn_start==(s->ed_frame+2)*6) {
        if(s->cp.silence) {
            /* 9.6.1.2.5: Ed is followed by Ucode-zero silence, preserving
               alignment and keeping the DTE clamped during echo training. */
            s->generated-=v90_pcm_discard_lookahead(&s->encoder);
            s->stage=7;s->have_cp=s->have_ack=0;
            s->rx.e_seen=s->rx_e_logged=0;
            fprintf(stderr,"[v90p4] Ed complete; CPs echo-training silence\n");
        } else if(v90_pcm_init(&s->encoder,&s->cp,data_bit,s)==0) {
            s->stage=4;s->data_start=s->samples;
            s->data_bits=0;memset(&s->rate_detector,0,sizeof(s->rate_detector));
            fprintf(stderr,"[v90p4] Ed complete; transmit B1d K=%u S=%u at %.6fs\n",s->encoder.k,s->encoder.s,s->samples/8000.0);
        } else {s->stage=3;fprintf(stderr,"[v90p4] rejected unusable data constellation\n");}
    }
    if(!s->stage && s->have_cpt && s->samples>=192 && s->samples%6==0) {
        if(v90_pcm_init(&s->encoder,&s->cpt,training_bit,s)==0) {
            s->mp_length=((86+s->encoder.k+s->encoder.s-1)/(s->encoder.k+s->encoder.s))*(s->encoder.k+s->encoder.s);
            mp_build(s);s->stage=1;s->rbar_end=s->samples+24;
            fprintf(stderr,"[v90p4] Ri-bar then TRN2d K=%u S=%u at %.6fs\n",s->encoder.k,s->encoder.s,s->samples/8000.0);
        } else {s->stage=3;fprintf(stderr,"[v90p4] rejected unusable training constellation\n");}
    }
    if(s->rx_e_logged && (s->stage==2 || s->stage==4) && !s->cp.silence) {
        unsigned had_b1=s->upstream.b1_seen;
        v90_upstream_receive(&s->upstream,input);
        if(!had_b1 && s->upstream.b1_seen)
            fprintf(stderr,"[v90p4] upstream B1 correlation %.4f at %.6fs\n",s->upstream.b1_score,s->samples/8000.0);
    }
    int out=0;
    if(s->stage<=1) {
        int sign=s->samples%6<3?1:-1;if(s->stage==1)sign=-sign;
        out=sign*v90_pcm_level(s->alaw,s->uinfo);
        if(s->stage==1 && s->samples+1==s->rbar_end){s->stage=2;s->trn_start=s->samples+1;}
    } else if(s->stage==2) {
        unsigned n=s->samples-s->trn_start;
        if(n%6==0)v90_pcm_frame(&s->encoder,s->frame);
        out=s->frame[n%6];
        if(n>=s->trn_frames*6 && !s->mp_announced){s->mp_announced=1;fprintf(stderr,"[v90p4] transmit MP at %.6fs\n",s->samples/8000.0);}
    }
    if(s->stage==6 && !s->reneg_start && (s->samples-s->data_start)%6==0)
        s->reneg_start=s->samples;
    if(s->stage==6 && s->reneg_start) {
        unsigned n=s->samples-s->reneg_start;
        int sign=n%6<3?1:-1;if(n>=384)sign=-sign;
        out=sign*v90_pcm_level(s->alaw,s->encoder.map[n%6][0]);
        if(n==407) {
            if(v90_pcm_renegotiate(&s->encoder,&s->preceding_cp,&s->cpt,training_bit,s)) {
                s->stage=3;fprintf(stderr,"[v90p4] unusable renegotiation constellation\n");
            } else {
                s->mp_length=((86+s->encoder.k+s->encoder.s-1)/(s->encoder.k+s->encoder.s))*(s->encoder.k+s->encoder.s);
                mp_build(s);s->stage=2;s->trn_start=s->samples+1;
                fprintf(stderr,"[v90p4] Rd/Rd-bar complete; renegotiation TRN2d K=%u S=%u at %.6fs\n",s->encoder.k,s->encoder.s,s->trn_start/8000.0);
            }
        }
    } else if(s->stage==4 || s->stage==5 || s->stage==6) {
        unsigned n=s->samples-s->data_start;
        if(n%6==0)v90_pcm_frame(&s->encoder,s->frame);
        out=s->frame[n%6];
        if(n==288)fprintf(stderr,"[v90p4] B1d transmitted\n");
    }
    if(s->stage==7 || s->stage==8) {
        out=v90_pcm_level(s->alaw,0);
        if(s->stage==8) {
            if(!s->rt_start && (s->samples-s->data_start)%6==0)
                s->rt_start=s->samples;
            if(s->rt_start) {
                unsigned n=s->samples-s->rt_start;
                int sign=n%6<3?1:-1;if(n>=384)sign=-sign;
                out=sign*v90_pcm_level(s->alaw,s->encoder.map[n%6][0]);
                if(n==407) {
                    /* Rt is uncoded: resume MP with the coding/filter state
                       left after Ed, without a new TRN2d initialization. */
                    s->mp_length=((86+s->encoder.k+s->encoder.s-1)/(s->encoder.k+s->encoder.s))*(s->encoder.k+s->encoder.s);
                    mp_build(s);s->stage=2;s->trn_start=s->samples+1;
                    fprintf(stderr,"[v90p4] Rt/Rt-bar complete; resume MP after silence\n");
                }
            }
        }
    }
    ++s->samples;return out;
}
