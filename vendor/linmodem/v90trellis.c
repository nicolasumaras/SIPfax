/* Restricted four-point V.34/V.90 16-state soft trellis. GPL-2.0. */
#include <float.h>
#include <string.h>
#include "v90trellis.h"

void v90_trellis_init(V90Trellis *s)
{
    /* Equal initial metrics: do not assume the caller's encoder state. */
    memset(s,0,sizeof(*s));
}

int v90_trellis_pair(V90Trellis *s,double ar,double ai,double br,double bi,
                    unsigned inversion,unsigned *out_a,unsigned *out_b)
{
    static const double re[4]={1,0,-1,0},im[4]={0,-1,0,1};
    double ca[4],cb[4],next[16];
    unsigned slot=(unsigned)(s->pairs%V90_TRELLIS_DEPTH);
    for(unsigned i=0;i<4;++i) {
        ca[i]=(ar-re[i])*(ar-re[i])+(ai-im[i])*(ai-im[i]);
        cb[i]=(br-re[i])*(br-re[i])+(bi-im[i])*(bi-im[i]);
    }
    for(unsigned i=0;i<16;++i)next[i]=DBL_MAX;
    for(unsigned state=0;state<16;++state) {
        unsigned u=state&1;
        for(unsigned a=0;a<4;++a)for(unsigned info=0;info<2;++info) {
            unsigned b=(a+2*info+(u^(inversion&1)))&3;
            /* Table13 restricted to four quadrants. The middle subset bit
             * is the real-coordinate bit; no legacy branch table is used. */
            unsigned as0=a&1,bs0=b&1,as1=a>>1,bs1=b>>1;
            unsigned y1=(as0&(bs0^1))^as1^bs1,y2=as0;
            unsigned dest=(state>>1)^y1^(y2<<1)^((y2^u)<<2)^(u<<3);
            double cost=s->metric[state]+ca[a]+cb[b];
            if(cost<next[dest]) {
                next[dest]=cost;s->previous[slot][dest]=(uint8_t)state;
                s->labels[slot][dest]=(uint8_t)(a|(b<<2));
            }
        }
    }
    unsigned state=0;
    for(unsigned i=1;i<16;++i)if(next[i]<next[state])state=i;
    double minimum=next[state];
    for(unsigned i=0;i<16;++i)s->metric[i]=next[i]-minimum;
    ++s->pairs;
    if(s->pairs<V90_TRELLIS_DEPTH)return 0;
    for(unsigned age=0;age<V90_TRELLIS_DEPTH-1;++age) {
        unsigned index=(slot+V90_TRELLIS_DEPTH-age)%V90_TRELLIS_DEPTH;
        state=s->previous[index][state];
    }
    unsigned index=(slot+1)%V90_TRELLIS_DEPTH,label=s->labels[index][state];
    *out_a=label&3;*out_b=label>>2;return 1;
}
