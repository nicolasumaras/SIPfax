/* Experimental upstream V.34 in V.90: 4800 four-point / 7200 eight-point.
 * Decode 4D pairs, GPA, 8N1, then verify PPP FCS before delivering a frame.
 * Ten timing phases and both pair alignments allow CRC-based acquisition.
 * Default receiver hard-slices; SIPFAX_V90_SOFT_RX=1 enables experimental
 * streaming trellis correction. Adaptive timing recovery remains unfinished. GPL-2.0. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "v90upstream.h"
static void soft_pair(void *,unsigned,unsigned);
static void qam_bits(void *,const uint8_t *);
void v90_upstream_init(V90Upstream *s)
{
    memset(s,0,sizeof(*s));s->last_frame_sample=-1000;
    const char *rate=getenv("SIPFAX_V90_UPSTREAM_RATE");
    s->rate=rate && !strcmp(rate,"7200")?7200:4800;
    if(s->rate==7200)for(unsigned i=0;i<V90_UP_PHASES;++i) {
        V90UpQamLane *l=&s->qam[i];l->up=s;l->phase=i;l->lane.crc=0xffff;
        v90_qam8_stream_init(&l->stream);
        l->stream.opaque=l;l->stream.receive_bits=qam_bits;
    }
    /* V.34 10.1.3.1: one frame of scrambled ones, zero encoder state,
       inversion as the last frame of the J=7 superframe. */
    unsigned bits[192],state=0,previous=0;
    for(unsigned i=0;i<192;++i)
        bits[i]=1^(i>=5?bits[i-5]:0)^(i>=23?bits[i-23]:0);
    for(unsigned i=0;i<64;++i) {
        unsigned a=(previous+bits[3*i+1]+2*bits[3*i+2])&3;
        unsigned b=(a+2*bits[3*i]+((state&1)^(i==0)))&3;
        s->b1_labels[2*i]=a;s->b1_labels[2*i+1]=b;previous=a;
        unsigned y1=((a&1)&((b&1)^1))^(a>>1)^(b>>1),y2=a&1,u=state&1;
        state=(state>>1)^y1^(y2<<1)^((y2^u)<<2)^(u<<3);
    }
    const char *option=getenv("SIPFAX_V90_SOFT_RX");
    s->soft_enabled=option && !strcmp(option,"1");
    if(s->soft_enabled)for(unsigned i=0;i<V90_UP_PHASES;++i) {
        V90UpSoftLane *l=&s->soft[i];l->up=s;l->phase=i;l->lane.crc=0xffff;
        v90_trellis_stream_init(&l->stream);
        l->stream.opaque=l;l->stream.receive_pair=soft_pair;
    }
    double beta=.1,sps=2.5;
    for(int fraction=0;fraction<4;++fraction)for(int k=0;k<V90_UP_TAPS;++k) {
        double *taps=s->taps[fraction];
        double t=(k-(V90_UP_TAPS-1)/2-fraction*.25)/sps;
        if(fabs(t)<1e-9)taps[k]=1-beta+4*beta/M_PI;
        else if(fabs(fabs(t)-1/(4*beta))<1e-9)
            taps[k]=beta/sqrt(2)*((1+2/M_PI)*sin(M_PI/(4*beta))+(1-2/M_PI)*cos(M_PI/(4*beta)));
        else taps[k]=(sin(M_PI*t*(1-beta))+4*beta*t*cos(M_PI*t*(1+beta)))/(M_PI*t*(1-16*beta*beta*t*t));
    }
    for(int i=0;i<V90_UP_PHASES;++i)for(int j=0;j<2;++j)s->lanes[i][j].crc=0xffff;
}
static void byte(V90Upstream *s,V90UpLane *l,unsigned value)
{
    if(value==0x7e) {
        if((!s->require_b1 || s->b1_seen) && !l->overflow && !l->escape && l->length>=4 && l->crc==0xf0b8) {
            unsigned duplicate=0;
            for(unsigned i=0;i<s->recent_count;++i)
                if(labs(l->source_sample-s->recent[i].sample)<40 && s->recent[i].length==l->length &&
                   !memcmp(s->recent[i].frame,l->frame,l->length)){duplicate=1;break;}
            if(!duplicate) {
                unsigned i=s->recent_next;
                s->recent[i].sample=l->source_sample;s->recent[i].length=l->length;
                memcpy(s->recent[i].frame,l->frame,l->length);
                s->recent_next=(i+1)%V90_UP_RECENT;
                if(s->recent_count<V90_UP_RECENT)++s->recent_count;
                ++s->frames;s->last_frame_sample=s->samples;s->last_length=l->length;
                memcpy(s->last_frame,l->frame,l->length);
                fprintf(stderr,"[v90data] CRC-valid PPP frame %u bytes at %.6fs\n",l->length,s->samples/8000.0);
                if(s->receive_frame)s->receive_frame(s->opaque,l->frame,l->length);
            }
        }
        l->length=0;l->escape=0;l->overflow=0;l->crc=0xffff;return;
    }
    if(value==0x7d){l->escape=1;return;}
    if(l->escape){value^=0x20;l->escape=0;}
    if(l->length>=V90_UP_FRAME){l->overflow=1;return;}
    l->frame[l->length++]=value;l->crc^=value;
    for(int k=0;k<8;++k)l->crc=(l->crc>>1)^((l->crc&1)?0x8408:0);
}
static void bit(V90Upstream *s,V90UpLane *l,unsigned b)
{
    unsigned plain=((l->scrambler>>22)^b)&1;
    l->scrambler=(l->scrambler<<1)&0x7fffff;
    if(b)l->scrambler^=1|(1<<18);
    if(!l->uart_count) {if(!plain){l->uart_count=1;l->uart_value=0;}return;}
    if(l->uart_count<=8) {l->uart_value|=plain<<(l->uart_count-1);++l->uart_count;return;}
    l->uart_count=0;if(plain)byte(s,l,l->uart_value);
}
static void soft_pair(void *opaque,unsigned a,unsigned b)
{
    V90UpSoftLane *l=opaque;
    l->lane.source_sample=(long)((10*l->stream.output_symbol+l->phase)/4);
    if(l->have_previous) {
        unsigned d=(b-a)&3,q=(a-l->previous)&3;
        bit(l->up,&l->lane,d>>1);bit(l->up,&l->lane,q&1);bit(l->up,&l->lane,q>>1);
    }
    l->previous=a;l->have_previous=1;
}
static void qam_bits(void *opaque,const uint8_t *bits)
{
    V90UpQamLane *l=opaque;
    if(l->stream.output_frames==1 || !bits) {
        memset(&l->lane,0,sizeof(l->lane));l->lane.crc=0xffff;
    }
    if(!bits)return;
    l->lane.source_sample=(long)((10*l->stream.output_symbol+l->phase)/4);
    for(unsigned i=0;i<18;++i)bit(l->up,&l->lane,bits[i]);
}
static unsigned delta(double ar,double ai,double br,double bi)
{
    return (-(int)lrint(atan2(ai*br-ar*bi,ar*br+ai*bi)/(M_PI/2)))&3;
}
static void b1_symbol(V90Upstream *s,unsigned phase,double re,double im)
{
    if(s->b1_seen)return;
    V90UpB1Lane *l=&s->b1[phase];
    if(!isfinite(re) || !isfinite(im)) {memset(l,0,sizeof(*l));return;}
    l->re[l->position]=re;l->im[l->position]=im;
    l->position=(l->position+1)%V90_UP_B1_SYMBOLS;
    if(l->count<V90_UP_B1_SYMBOLS)++l->count;
    if(l->count<V90_UP_B1_SYMBOLS)return;
    double cr=0,ci=0,energy=0;
    for(unsigned i=0;i<V90_UP_B1_SYMBOLS;++i) {
        unsigned j=(l->position+i)%V90_UP_B1_SYMBOLS;
        double ar=l->re[j],ai=l->im[j];
        energy+=ar*ar+ai*ai;
        switch(s->b1_labels[i]) {
        case 0:cr+=ar;ci+=ai;break;
        case 1:cr-=ai;ci+=ar;break;
        case 2:cr-=ar;ci-=ai;break;
        case 3:cr+=ai;ci-=ar;break;
        }
    }
    double power=cr*cr+ci*ci;
    if(energy>0 && isfinite(energy) && isfinite(power) &&
       power>=.9*.9*V90_UP_B1_SYMBOLS*energy) {
        s->b1_seen=1;s->b1_sample=s->samples;
        s->b1_score=sqrt(power/(V90_UP_B1_SYMBOLS*energy));
    }
}
static void symbol(V90Upstream *s,long time,double re,double im)
{
    if(s->rate==7200) {
        V90Qam8Stream *q=&s->qam[time%V90_UP_PHASES].stream;
        int locked=v90_qam8_stream_symbol(q,re,im);
        if(locked==1 && !s->b1_seen) {
            s->b1_seen=1;s->b1_sample=s->samples;s->b1_score=q->score;
        }
        return;
    }
    b1_symbol(s,time%V90_UP_PHASES,re,im);
    if(s->soft_enabled) {
        v90_trellis_stream_symbol(&s->soft[time%V90_UP_PHASES].stream,re,im);
        return;
    }
    for(unsigned pair=0;pair<2;++pair) {
        V90UpLane *l=&s->lanes[time%V90_UP_PHASES][pair];
        l->source_sample=s->samples;
        if(((time/V90_UP_PHASES)&1)==pair){l->a_re=re;l->a_im=im;l->have_a=1;}
        else if(l->have_a) {
            if(l->have_previous) {
                unsigned d=delta(re,im,l->a_re,l->a_im);
                unsigned q=delta(l->a_re,l->a_im,l->previous_re,l->previous_im);
                bit(s,l,d>>1);bit(s,l,q&1);bit(s,l,q>>1);
            }
            l->previous_re=l->a_re;l->previous_im=l->a_im;l->have_previous=1;l->have_a=0;
        }
    }
}
void v90_upstream_receive(V90Upstream *s,int16_t input)
{
    double phase=2*M_PI*1920*s->samples/8000.0;
    s->re[s->position]=input*cos(phase);s->im[s->position]=-input*sin(phase);
    /* Evaluate the matched filter at quarter-sample instants. Averaging
       adjacent outputs attenuates/distorts the wideband baseband signal. */
    for(int fraction=3;fraction>=0;--fraction) {
        double re=0,im=0;
        for(int k=0;k<V90_UP_TAPS;++k) {
            unsigned j=(s->position+V90_UP_TAPS-k)%V90_UP_TAPS;
            re+=s->taps[fraction][k]*s->re[j];
            im+=s->taps[fraction][k]*s->im[j];
        }
        if(4*s->samples>=fraction)
            symbol(s,4*s->samples-fraction,re,im);
    }
    s->position=(s->position+1)%V90_UP_TAPS;
    ++s->samples;
}
