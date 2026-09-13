/* Answer-side V.8 CJ acquisition: three 8N1 zero octets at 300 baud.
 * Parallel integrate-and-dump phases avoid dependence on the legacy V.21 PLL.
 * Used only after CM/JM selection, not as a replacement for menu validation.
 * GPL-2.0. */
#include <math.h>
#include <string.h>
#include "v8cj.h"
void v8_cj_init(V8Cj *s)
{
    memset(s,0,sizeof(*s));s->found_at=-1;
    for(unsigned j=0;j<27;++j)s->lanes[j].clock=j*300;
}
int v8_cj_receive(V8Cj *s,const int16_t *input,unsigned length)
{
    for(unsigned i=0;i<length;++i,++s->samples) {
        double re[2],im[2],value=input[i];
        for(unsigned k=0;k<2;++k) {
            double phase=2*M_PI*(k?980:1180)*s->samples/8000.0;
            re[k]=value*cos(phase);im[k]=-value*sin(phase);
        }
        for(unsigned j=0;j<27;++j) {
            V8CjLane *l=&s->lanes[j];
            for(unsigned k=0;k<2;++k){l->re[k]+=re[k];l->im[k]+=im[k];}
            l->energy+=value*value;++l->count;l->clock+=300;
            if(l->clock<8000)continue;
            l->clock-=8000;
            double p0=l->re[0]*l->re[0]+l->im[0]*l->im[0];
            double p1=l->re[1]*l->re[1]+l->im[1]*l->im[1];
            unsigned bit=p1>p0;double power=bit?p1:p0;
            /* Reject silence, off-band tones and poorly aligned symbols. */
            if(l->count>=26 && l->energy>l->count*100.0 &&
               2*power>.6*l->count*l->energy) ++l->valid;
            else l->valid=0;
            l->bits=((l->bits<<1)|bit)&0x3fffffff;
            if(l->valid>=30 && l->bits==0x100401 && s->found_at<0)
                s->found_at=s->samples+1;
            memset(l->re,0,sizeof(l->re));memset(l->im,0,sizeof(l->im));
            l->energy=0;l->count=0;
        }
    }
    return s->found_at>=0;
}
