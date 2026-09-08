/* 4800-bit/s upstream V.34 in V.90: b=12,K=0, four-point constellation.
 * Decode 4D pairs, GPA, 8N1, then verify PPP FCS before delivering a frame.
 * Ten timing phases and both pair alignments allow CRC-based acquisition.
 * This initial receiver hard-slices; trellis error correction and adaptive
 * timing recovery remain needed for difficult channels/long packets. GPL-2.0. */
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "v90upstream.h"
void v90_upstream_init(V90Upstream *s)
{
    memset(s,0,sizeof(*s));s->last_frame_sample=-1000;
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
        if(!l->overflow && !l->escape && l->length>=4 && l->crc==0xf0b8) {
            if(s->samples-s->last_frame_sample>=40 || s->last_length!=l->length || memcmp(s->last_frame,l->frame,l->length)) {
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
static unsigned delta(double ar,double ai,double br,double bi)
{
    return (-(int)lrint(atan2(ai*br-ar*bi,ar*br+ai*bi)/(M_PI/2)))&3;
}
static void symbol(V90Upstream *s,long time,double re,double im)
{
    for(unsigned pair=0;pair<2;++pair) {
        V90UpLane *l=&s->lanes[time%V90_UP_PHASES][pair];
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
