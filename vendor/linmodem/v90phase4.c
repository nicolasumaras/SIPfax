/* Digital V.90 Phase4: live CPt/CP, Ri/Ri-bar, TRN2d and MP. GPL-2.0. */
#include <stdio.h>
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
    if(n<340*d)return 1;
    unsigned index=(n-340*d)%s->mp_length;
    if(!s->ed_frame && !index && s->have_ack && s->mp_ack)s->ed_frame=n/d;
    if(s->ed_frame && n>=s->ed_frame*d)return 0;
    if(!index && s->have_cp && !s->mp_ack){s->mp_ack=1;mp_build(s);}
    return s->mp[index];
}
static int data_bit(void *opaque)
{
    V90Phase4 *s=opaque;
    if(s->data_bits++<48*(s->encoder.k+s->encoder.s))return 1;
    return s->get_data_bit?s->get_data_bit(s->data_opaque):1;
}
void v90_phase4_init(V90Phase4 *s,int alaw,int uinfo)
{
    memset(s,0,sizeof(*s));s->alaw=alaw;s->uinfo=uinfo;
    v90_training_init(&s->rx);s->rx.cp_mode=1;v90_upstream_init(&s->upstream);
    fprintf(stderr,"[v90p4] transmit Ri; receive CPt\n");
}
int16_t v90_phase4_next(V90Phase4 *s,int16_t input)
{
    int count=s->rx.found;v90_training_receive(&s->rx,&input,1);
    if(count!=s->rx.found) {
        if(!s->rx.cp.type && !s->have_cpt) {
            s->cpt=s->rx.cp;s->have_cpt=1;
            fprintf(stderr,"[v90p4] live CRC-valid CPt: drn=%u Sr=%u ld=%u gain=%u masks=%u at %.6fs\n",s->cpt.drn,s->cpt.sr,s->cpt.lookahead,s->cpt.gain,s->cpt.count,s->samples/8000.0);
        } else if(s->rx.cp.type) {
            s->cp=s->rx.cp;
            if(!s->have_cp || (!s->have_ack && s->cp.ack))
                fprintf(stderr,"[v90p4] live CRC-valid CP%s: downstream=%u/3 bit/s Sr=%u ack=%u at %.6fs\n",s->cp.ack?"-prime":"",(s->cp.drn+20)*4000,s->cp.sr,s->cp.ack,s->samples/8000.0);
            s->have_cp=1;if(s->cp.ack)s->have_ack=1;
        }
    }
    if(s->rx.e_seen && !s->rx_e_logged) {
        s->rx_e_logged=1;fprintf(stderr,"[v90p4] upstream E detected at %.6fs; starting upstream B1/data receiver\n",s->samples/8000.0);
    }
    if(s->stage==2 && s->ed_frame && s->samples-s->trn_start==(s->ed_frame+2)*6) {
        if(v90_pcm_init(&s->encoder,&s->cp,data_bit,s)==0) {
            s->stage=4;s->data_start=s->samples;
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
    int out=0;
    if(s->stage<=1) {
        int sign=s->samples%6<3?1:-1;if(s->stage==1)sign=-sign;
        out=sign*v90_pcm_level(s->alaw,s->uinfo);
        if(s->stage==1 && s->samples+1==s->rbar_end){s->stage=2;s->trn_start=s->samples+1;}
    } else if(s->stage==2) {
        unsigned n=s->samples-s->trn_start;
        if(n%6==0)v90_pcm_frame(&s->encoder,s->frame);
        out=s->frame[n%6];
        if(n>=2040 && !s->mp_announced){s->mp_announced=1;fprintf(stderr,"[v90p4] transmit MP at %.6fs\n",s->samples/8000.0);}
    }
    if(s->stage==4) {
        unsigned n=s->samples-s->data_start;
        if(s->rx_e_logged)v90_upstream_receive(&s->upstream,input);
        if(n%6==0)v90_pcm_frame(&s->encoder,s->frame);
        out=s->frame[n%6];
        if(n==288)fprintf(stderr,"[v90p4] B1d transmitted; bidirectional PPP data path enabled\n");
    }
    ++s->samples;return out;
}
