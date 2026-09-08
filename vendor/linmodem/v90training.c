/* Streaming 3200-baud/1920Hz V.90 upstream Ja receiver. GPL-2.0.
 * Five half-sample timing hypotheses; accept only CRC-validated descriptors.
 */
#include <math.h>
#include <string.h>
#include <stdio.h>
#include "v90training.h"
void v90_training_init(V90Training *s)
{
    memset(s,0,sizeof(*s));
    const double beta=.1,sps=2.5;
    for(int k=0;k<V90_RX_TAPS;++k) {
        double t=(k-(V90_RX_TAPS-1)/2)/sps,v;
        if(fabs(t)<1e-9)v=1-beta+4*beta/M_PI;
        else if(fabs(fabs(t)-1/(4*beta))<1e-9)
            v=beta/sqrt(2)*((1+2/M_PI)*sin(M_PI/(4*beta))+(1-2/M_PI)*cos(M_PI/(4*beta)));
        else v=(sin(M_PI*t*(1-beta))+4*beta*t*cos(M_PI*t*(1+beta)))/(M_PI*t*(1-16*beta*beta*t*t));
        s->taps[k]=v;
    }
}
static void bit(V90Training *s,V90JaLane *lane,unsigned b)
{
    unsigned plain=((lane->scrambler>>22)^b)&1;
    lane->scrambler=(lane->scrambler<<1)&0x7fffff;
    if(b)lane->scrambler^=1|(1<<18); /* GPA */
    if(lane->count) {
        if(lane->count>=V90_JA_MAX_BITS)lane->count=0;
        else {
            lane->bits[lane->count++]=plain;
            unsigned consumed;
            int valid=v90_dil_parse(&s->dil,lane->bits,lane->count,&consumed);
            if(valid==1) {
                s->found=1;
                fprintf(stderr,"[v90p3] CRC-valid live Ja at %.6fs: N=%u LSP=%u LTP=%u\n",
                        s->samples/8000.0,s->dil.n,s->dil.lsp,s->dil.ltp);
            }
            if(valid!=0)lane->count=0;
        }
    }
    if(!lane->count && !plain && lane->ones>=17) {
        memset(lane->bits,1,17);lane->bits[17]=0;lane->count=18;
    }
    lane->ones=plain ? lane->ones+1 : 0;
    if(lane->ones>17)lane->ones=17;
}
static void symbol(V90Training *s,long time,double re,double im)
{
    V90JaLane *lane=&s->lanes[time%5];
    if(lane->have_previous) {
        double dot=re*lane->previous_re+im*lane->previous_im;
        double cross=im*lane->previous_re-re*lane->previous_im;
        int delta=(-(int)lrint(atan2(cross,dot)/(M_PI/2)))&3;
        bit(s,lane,delta&1);
        if(!s->found)bit(s,lane,delta>>1);
    }
    lane->previous_re=re;lane->previous_im=im;lane->have_previous=1;
}
int v90_training_receive(V90Training *s,const int16_t *pcm,int count)
{
    for(int n=0;n<count && !s->found;++n,++s->samples) {
        double phase=2*M_PI*1920*s->samples/8000.0;
        s->re[s->position]=pcm[n]*cos(phase);s->im[s->position]=-pcm[n]*sin(phase);
        double re=0,im=0;
        for(int k=0;k<V90_RX_TAPS;++k) {
            int j=(s->position+V90_RX_TAPS-k)%V90_RX_TAPS;
            re+=s->taps[k]*s->re[j];im+=s->taps[k]*s->im[j];
        }
        s->position=(s->position+1)%V90_RX_TAPS;
        if(s->samples)symbol(s,2*s->samples-1,(s->last_re+re)/2,(s->last_im+im)/2);
        if(!s->found)symbol(s,2*s->samples,re,im);
        s->last_re=re;s->last_im=im;
    }
    return s->found;
}
