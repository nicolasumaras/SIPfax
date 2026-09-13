/* Four/eight/twelve/twenty/thirty-two-point V.34/V.90 16-state soft trellis. GPL-2.0. */
#include <float.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "v90trellis.h"

void v90_trellis_init(V90Trellis *s)
{
    /* Equal initial metrics: do not assume the caller's encoder state. */
    memset(s,0,sizeof(*s));
}

int v90_trellis_peek(const V90Trellis *s,unsigned age,unsigned label_bits,
                     unsigned *a,unsigned *b)
{
    if(age>=V90_TRELLIS_DEPTH || s->pairs<=age || label_bits<2 || label_bits>11 || !a || !b)return 0;
    unsigned state=0,slot=(unsigned)((s->pairs-1)%V90_TRELLIS_DEPTH);
    for(unsigned i=1;i<16;++i)if(s->metric[i]<s->metric[state])state=i;
    for(unsigned i=0;i<age;++i) {
        unsigned k=(slot+V90_TRELLIS_DEPTH-i)%V90_TRELLIS_DEPTH;
        state=s->previous[k][state];
    }
    unsigned k=(slot+V90_TRELLIS_DEPTH-age)%V90_TRELLIS_DEPTH;
    unsigned label=s->labels[k][state];
    *a=label&((1u<<label_bits)-1);*b=label>>label_bits;return 1;
}

static int pair_costs(V90Trellis *s,const double ca[4],const double cb[4],
                      const unsigned labels_a[4],const unsigned labels_b[4],
                      unsigned label_bits,unsigned inversion,unsigned *out_a,unsigned *out_b)
{
    double next[16];
    unsigned slot=(unsigned)(s->pairs%V90_TRELLIS_DEPTH);
    for(unsigned i=0;i<16;++i)next[i]=DBL_MAX;
    for(unsigned state=0;state<16;++state) {
        unsigned u=state&1;
        for(unsigned a=0;a<4;++a)for(unsigned info=0;info<2;++info) {
            unsigned b=(a+2*info+(u^(inversion&1)))&3;
            /* Table13 Y1/Y2 depend on the low two subset bits. For the
             * supported constellations those bits are the quadrant;
             * ring-dependent upper subset bits do not affect this encoder. */
            unsigned as0=a&1,bs0=b&1,as1=a>>1,bs1=b>>1;
            unsigned y1=(as0&(bs0^1))^as1^bs1,y2=as0;
            unsigned dest=(state>>1)^y1^(y2<<1)^((y2^u)<<2)^(u<<3);
            double cost=s->metric[state]+ca[a]+cb[b];
            if(cost<next[dest]) {
                next[dest]=cost;s->previous[slot][dest]=(uint8_t)state;
                s->labels[slot][dest]=(uint32_t)(labels_a[a]|(labels_b[b]<<label_bits));
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
    *out_a=label&((1u<<label_bits)-1);*out_b=label>>label_bits;return 1;
}

int v90_trellis_pair(V90Trellis *s,double ar,double ai,double br,double bi,
                    unsigned inversion,unsigned *out_a,unsigned *out_b)
{
    static const double re[4]={1,0,-1,0},im[4]={0,-1,0,1};
    static const unsigned labels[4]={0,1,2,3};
    double ca[4],cb[4];
    for(unsigned i=0;i<4;++i) {
        ca[i]=(ar-re[i])*(ar-re[i])+(ai-im[i])*(ai-im[i]);
        cb[i]=(br-re[i])*(br-re[i])+(bi-im[i])*(bi-im[i]);
    }
    return pair_costs(s,ca,cb,labels,labels,2,inversion,out_a,out_b);
}

static int qam_pair(V90Trellis *s,double ar,double ai,double br,double bi,
                    unsigned inversion,unsigned rings,unsigned label_bits,
                    unsigned *out_a,unsigned *out_b)
{
    static const double re[1280]={1,1,-1,-1,-3,1,3,-1,1,-3,-1,3,-3,-3,3,3,1,5,-1,-5,5,1,-5,-1,-3,5,3,-5,5,-3,-5,3,5,5,-5,-5,-7,1,7,-1,1,-7,-1,7,-7,-3,7,3,-3,-7,3,7,-7,5,7,-5,5,-7,-5,7,1,9,-1,-9,9,1,-9,-1,-3,9,3,-9,9,-3,-9,3,-7,-7,7,7,5,9,-5,-9,9,5,-9,-5,-11,1,11,-1,1,-11,-1,11,-7,9,7,-9,-11,-3,11,3,9,-7,-9,7,-3,-11,3,11,-11,5,11,-5,5,-11,-5,11,9,9,-9,-9,1,13,-1,-13,13,1,-13,-1,-11,-7,11,7,-7,-11,7,11,-3,13,3,-13,13,-3,-13,3,5,13,-5,-13,13,5,-13,-5,-11,9,11,-9,9,-11,-9,11,-7,13,7,-13,13,-7,-13,7,-15,1,15,-1,1,-15,-1,15,-15,-3,15,3,-3,-15,3,15,-11,-11,11,11,9,13,-9,-13,13,9,-13,-9,-15,5,15,-5,5,-15,-5,15,-15,-7,15,7,-7,-15,7,15,1,17,-1,-17,-11,13,11,-13,17,1,-17,-1,13,-11,-13,11,-3,17,3,-17,17,-3,-17,3,-15,9,15,-9,9,-15,-9,15,5,17,-5,-17,17,5,-17,-5,-7,17,7,-17,13,13,-13,-13,17,-7,-17,7,-15,-11,15,11,-11,-15,11,15,-19,1,19,-1,1,-19,-1,19,9,17,-9,-17,17,9,-17,-9,-19,-3,19,3,-3,-19,3,19,-19,5,19,-5,5,-19,-5,19,-15,13,15,-13,13,-15,-13,15,-11,17,11,-17,-19,-7,19,7,17,-11,-17,11,-7,-19,7,19,1,21,-1,-21,-19,9,19,-9,21,1,-21,-1,9,-19,-9,19,-3,21,3,-21,21,-3,-21,3,-15,-15,15,15,13,17,-13,-17,17,13,-17,-13,5,21,-5,-21,21,5,-21,-5,-19,-11,19,11,-11,-19,11,19,-7,21,7,-21,21,-7,-21,7,-15,17,15,-17,17,-15,-17,15,9,21,-9,-21,21,9,-21,-9,-19,13,19,-13,-23,1,23,-1,13,-19,-13,19,1,-23,-1,23,-23,-3,23,3,-3,-23,3,23,-23,5,23,-5,5,-23,-5,23,-11,21,11,-21,21,-11,-21,11,17,17,-17,-17,-23,-7,23,7,-7,-23,7,23,-19,-15,19,15,-15,-19,15,19,13,21,-13,-21,21,13,-21,-13,-23,9,23,-9,9,-23,-9,23,1,25,-1,-25,25,1,-25,-1,-3,25,3,-25,25,-3,-25,3,5,25,-5,-25,-19,17,19,-17,25,5,-25,-5,-23,-11,23,11,17,-19,-17,19,-11,-23,11,23,-15,21,15,-21,21,-15,-21,15,-7,25,7,-25,25,-7,-25,7,-23,13,23,-13,13,-23,-13,23,9,25,-9,-25,25,9,-25,-9,-19,-19,19,19,17,21,-17,-21,21,17,-21,-17,-27,1,27,-1,1,-27,-1,27,-27,-3,27,3,-3,-27,3,27,-11,25,11,-25,25,-11,-25,11,-27,5,27,-5,-23,-15,23,15,-15,-23,15,23,5,-27,-5,27,-27,-7,27,7,-7,-27,7,27,13,25,-13,-25,25,13,-25,-13,-19,21,19,-21,21,-19,-21,19,-27,9,27,-9,9,-27,-9,27,-23,17,23,-17,17,-23,-17,23,1,29,-1,-29,29,1,-29,-1,-3,29,3,-29,-15,25,15,-25,29,-3,-29,3,-27,-11,27,11,25,-15,-25,15,-11,-27,11,27,5,29,-5,-29,29,5,-29,-5,21,21,-21,-21,-7,29,7,-29,29,-7,-29,7,-23,-19,23,19,-19,-23,19,23,-27,13,27,-13,13,-27,-13,27,17,25,-17,-25,25,17,-25,-17,9,29,-9,-29,29,9,-29,-9,-27,-15,27,15,-15,-27,15,27,-11,29,11,-29,-31,1,31,-1,29,-11,-29,11,1,-31,-1,31,-23,21,23,-21,-31,-3,31,3,21,-23,-21,23,-3,-31,3,31,-19,25,19,-25,-31,5,31,-5,25,-19,-25,19,5,-31,-5,31,13,29,-13,-29,29,13,-29,-13,-31,-7,31,7,-7,-31,7,31,-27,17,27,-17,17,-27,-17,27,-31,9,31,-9,9,-31,-9,31,-23,-23,23,23,-15,29,15,-29,21,25,-21,-25,25,21,-25,-21,29,-15,-29,15,-31,-11,31,11,-11,-31,11,31,1,33,-1,-33,33,1,-33,-1,-27,-19,27,19,-19,-27,19,27,-3,33,3,-33,33,-3,-33,3,5,33,-5,-33,33,5,-33,-5,17,29,-17,-29,29,17,-29,-17,-31,13,31,-13,13,-31,-13,31,-7,33,7,-33,33,-7,-33,7,-23,25,23,-25,25,-23,-25,23,9,33,-9,-33,-27,21,27,-21,33,9,-33,-9,21,-27,-21,27,-31,-15,31,15,-15,-31,15,31,-19,29,19,-29,29,-19,-29,19,-11,33,11,-33,33,-11,-33,11,-35,1,35,-1,1,-35,-1,35,-35,-3,35,3,-3,-35,3,35,25,25,-25,-25,-31,17,31,-17,-35,5,35,-5,17,-31,-17,31,5,-35,-5,35,13,33,-13,-33,33,13,-33,-13,-27,-23,27,23,-23,-27,23,27,-35,-7,35,7,-7,-35,7,35,21,29,-21,-29,29,21,-29,-21,-35,9,35,-9,9,-35,-9,35,-15,33,15,-33,33,-15,-33,15,-31,-19,31,19,-19,-31,19,31,-35,-11,35,11,-11,-35,11,35,-27,25,27,-25,25,-27,-25,27,1,37,-1,-37,-23,29,23,-29,37,1,-37,-1,29,-23,-29,23,-3,37,3,-37,17,33,-17,-33,33,17,-33,-17,37,-3,-37,3,5,37,-5,-37,-35,13,35,-13,37,5,-37,-5,13,-35,-13,35,-31,21,31,-21,21,-31,-21,31,-7,37,7,-37,37,-7,-37,7,9,37,-9,-37,-19,33,19,-33,37,9,-37,-9,-35,-15,35,15,33,-19,-33,19,-15,-35,15,35,-27,-27,27,27,25,29,-25,-29,29,25,-29,-25,-11,37,11,-37,37,-11,-37,11,-31,-23,31,23,-23,-31,23,31,-35,17,35,-17,17,-35,-17,35,-39,1,39,-1,1,-39,-1,39,21,33,-21,-33,33,21,-33,-21,-39,-3,39,3,-3,-39,3,39,13,37,-13,-37,37,13,-37,-13,-39,5,39,-5,5,-39,-5,39,-27,29,27,-29,-39,-7,39,7,29,-27,-29,27,-7,-39,7,39,-31,25,31,-25,-35,-19,35,19,25,-31,-25,31,-19,-35,19,35,-15,37,15,-37,37,-15,-37,15,-39,9,39,-9,9,-39,-9,39,-23,33,23,-33,33,-23,-33,23};
    static const double im[1280]={1,-1,-1,1,1,3,-1,-3,-3,-1,3,1,-3,3,3,-3,5,-1,-5,1,1,-5,-1,5,5,3,-5,-3,-3,-5,3,5,5,-5,-5,5,1,7,-1,-7,-7,-1,7,1,-3,7,3,-7,-7,3,7,-3,5,7,-5,-7,-7,-5,7,5,9,-1,-9,1,1,-9,-1,9,9,3,-9,-3,-3,-9,3,9,-7,7,7,-7,9,-5,-9,5,5,-9,-5,9,1,11,-1,-11,-11,-1,11,1,9,7,-9,-7,-3,11,3,-11,-7,-9,7,9,-11,3,11,-3,5,11,-5,-11,-11,-5,11,5,9,-9,-9,9,13,-1,-13,1,1,-13,-1,13,-7,11,7,-11,-11,7,11,-7,13,3,-13,-3,-3,-13,3,13,13,-5,-13,5,5,-13,-5,13,9,11,-9,-11,-11,-9,11,9,13,7,-13,-7,-7,-13,7,13,1,15,-1,-15,-15,-1,15,1,-3,15,3,-15,-15,3,15,-3,-11,11,11,-11,13,-9,-13,9,9,-13,-9,13,5,15,-5,-15,-15,-5,15,5,-7,15,7,-15,-15,7,15,-7,17,-1,-17,1,13,11,-13,-11,1,-17,-1,17,-11,-13,11,13,17,3,-17,-3,-3,-17,3,17,9,15,-9,-15,-15,-9,15,9,17,-5,-17,5,5,-17,-5,17,17,7,-17,-7,13,-13,-13,13,-7,-17,7,17,-11,15,11,-15,-15,11,15,-11,1,19,-1,-19,-19,-1,19,1,17,-9,-17,9,9,-17,-9,17,-3,19,3,-19,-19,3,19,-3,5,19,-5,-19,-19,-5,19,5,13,15,-13,-15,-15,-13,15,13,17,11,-17,-11,-7,19,7,-19,-11,-17,11,17,-19,7,19,-7,21,-1,-21,1,9,19,-9,-19,1,-21,-1,21,-19,-9,19,9,21,3,-21,-3,-3,-21,3,21,-15,15,15,-15,17,-13,-17,13,13,-17,-13,17,21,-5,-21,5,5,-21,-5,21,-11,19,11,-19,-19,11,19,-11,21,7,-21,-7,-7,-21,7,21,17,15,-17,-15,-15,-17,15,17,21,-9,-21,9,9,-21,-9,21,13,19,-13,-19,1,23,-1,-23,-19,-13,19,13,-23,-1,23,1,-3,23,3,-23,-23,3,23,-3,5,23,-5,-23,-23,-5,23,5,21,11,-21,-11,-11,-21,11,21,17,-17,-17,17,-7,23,7,-23,-23,7,23,-7,-15,19,15,-19,-19,15,19,-15,21,-13,-21,13,13,-21,-13,21,9,23,-9,-23,-23,-9,23,9,25,-1,-25,1,1,-25,-1,25,25,3,-25,-3,-3,-25,3,25,25,-5,-25,5,17,19,-17,-19,5,-25,-5,25,-11,23,11,-23,-19,-17,19,17,-23,11,23,-11,21,15,-21,-15,-15,-21,15,21,25,7,-25,-7,-7,-25,7,25,13,23,-13,-23,-23,-13,23,13,25,-9,-25,9,9,-25,-9,25,-19,19,19,-19,21,-17,-21,17,17,-21,-17,21,1,27,-1,-27,-27,-1,27,1,-3,27,3,-27,-27,3,27,-3,25,11,-25,-11,-11,-25,11,25,5,27,-5,-27,-15,23,15,-23,-23,15,23,-15,-27,-5,27,5,-7,27,7,-27,-27,7,27,-7,25,-13,-25,13,13,-25,-13,25,21,19,-21,-19,-19,-21,19,21,9,27,-9,-27,-27,-9,27,9,17,23,-17,-23,-23,-17,23,17,29,-1,-29,1,1,-29,-1,29,29,3,-29,-3,25,15,-25,-15,-3,-29,3,29,-11,27,11,-27,-15,-25,15,25,-27,11,27,-11,29,-5,-29,5,5,-29,-5,29,21,-21,-21,21,29,7,-29,-7,-7,-29,7,29,-19,23,19,-23,-23,19,23,-19,13,27,-13,-27,-27,-13,27,13,25,-17,-25,17,17,-25,-17,25,29,-9,-29,9,9,-29,-9,29,-15,27,15,-27,-27,15,27,-15,29,11,-29,-11,1,31,-1,-31,-11,-29,11,29,-31,-1,31,1,21,23,-21,-23,-3,31,3,-31,-23,-21,23,21,-31,3,31,-3,25,19,-25,-19,5,31,-5,-31,-19,-25,19,25,-31,-5,31,5,29,-13,-29,13,13,-29,-13,29,-7,31,7,-31,-31,7,31,-7,17,27,-17,-27,-27,-17,27,17,9,31,-9,-31,-31,-9,31,9,-23,23,23,-23,29,15,-29,-15,25,-21,-25,21,21,-25,-21,25,-15,-29,15,29,-11,31,11,-31,-31,11,31,-11,33,-1,-33,1,1,-33,-1,33,-19,27,19,-27,-27,19,27,-19,33,3,-33,-3,-3,-33,3,33,33,-5,-33,5,5,-33,-5,33,29,-17,-29,17,17,-29,-17,29,13,31,-13,-31,-31,-13,31,13,33,7,-33,-7,-7,-33,7,33,25,23,-25,-23,-23,-25,23,25,33,-9,-33,9,21,27,-21,-27,9,-33,-9,33,-27,-21,27,21,-15,31,15,-31,-31,15,31,-15,29,19,-29,-19,-19,-29,19,29,33,11,-33,-11,-11,-33,11,33,1,35,-1,-35,-35,-1,35,1,-3,35,3,-35,-35,3,35,-3,25,-25,-25,25,17,31,-17,-31,5,35,-5,-35,-31,-17,31,17,-35,-5,35,5,33,-13,-33,13,13,-33,-13,33,-23,27,23,-27,-27,23,27,-23,-7,35,7,-35,-35,7,35,-7,29,-21,-29,21,21,-29,-21,29,9,35,-9,-35,-35,-9,35,9,33,15,-33,-15,-15,-33,15,33,-19,31,19,-31,-31,19,31,-19,-11,35,11,-35,-35,11,35,-11,25,27,-25,-27,-27,-25,27,25,37,-1,-37,1,29,23,-29,-23,1,-37,-1,37,-23,-29,23,29,37,3,-37,-3,33,-17,-33,17,17,-33,-17,33,-3,-37,3,37,37,-5,-37,5,13,35,-13,-35,5,-37,-5,37,-35,-13,35,13,21,31,-21,-31,-31,-21,31,21,37,7,-37,-7,-7,-37,7,37,37,-9,-37,9,33,19,-33,-19,9,-37,-9,37,-15,35,15,-35,-19,-33,19,33,-35,15,35,-15,-27,27,27,-27,29,-25,-29,25,25,-29,-25,29,37,11,-37,-11,-11,-37,11,37,-23,31,23,-31,-31,23,31,-23,17,35,-17,-35,-35,-17,35,17,1,39,-1,-39,-39,-1,39,1,33,-21,-33,21,21,-33,-21,33,-3,39,3,-39,-39,3,39,-3,37,-13,-37,13,13,-37,-13,37,5,39,-5,-39,-39,-5,39,5,29,27,-29,-27,-7,39,7,-39,-27,-29,27,29,-39,7,39,-7,25,31,-25,-31,-19,35,19,-35,-31,-25,31,25,-35,19,35,-19,37,15,-37,-15,-15,-37,15,37,9,39,-9,-39,-39,-9,39,9,33,23,-33,-23,-23,-33,23,33};
    if(!isfinite(ar)||!isfinite(ai)||!isfinite(br)||!isfinite(bi)||
       fabs(ar)>1e100||fabs(ai)>1e100||fabs(br)>1e100||fabs(bi)>1e100)return -1;
    double ca[4],cb[4];unsigned la[4],lb[4];
    for(unsigned q=0;q<4;++q) {
        ca[q]=cb[q]=DBL_MAX;la[q]=lb[q]=q;
        for(unsigned ring=0;ring<rings;++ring) {
            unsigned i=q+4*ring;
            double a=(ar-re[i])*(ar-re[i])+(ai-im[i])*(ai-im[i]);
            double b=(br-re[i])*(br-re[i])+(bi-im[i])*(bi-im[i]);
            if(a<ca[q]){ca[q]=a;la[q]=i;}
            if(b<cb[q]){cb[q]=b;lb[q]=i;}
        }
    }
    return pair_costs(s,ca,cb,la,lb,label_bits,inversion,out_a,out_b);
}
int v90_trellis_qam8_pair(V90Trellis *s,double ar,double ai,double br,double bi,
                         unsigned inversion,unsigned *out_a,unsigned *out_b)
{
    return qam_pair(s,ar,ai,br,bi,inversion,2,3,out_a,out_b);
}
int v90_trellis_qam12_pair(V90Trellis *s,double ar,double ai,double br,double bi,
                          unsigned inversion,unsigned *out_a,unsigned *out_b)
{
    return qam_pair(s,ar,ai,br,bi,inversion,3,4,out_a,out_b);
}

int v90_trellis_qam20_pair(V90Trellis *s,double ar,double ai,double br,double bi,
                          unsigned inversion,unsigned *out_a,unsigned *out_b)
{
    return qam_pair(s,ar,ai,br,bi,inversion,5,5,out_a,out_b);
}

int v90_trellis_qam32_pair(V90Trellis *s,double ar,double ai,double br,double bi,
                          unsigned inversion,unsigned *out_a,unsigned *out_b)
{
    return qam_pair(s,ar,ai,br,bi,inversion,8,5,out_a,out_b);
}

int v90_trellis_qam56_pair(V90Trellis *s,double ar,double ai,double br,double bi,
                          unsigned inversion,unsigned *out_a,unsigned *out_b)
{
    return qam_pair(s,ar,ai,br,bi,inversion,14,6,out_a,out_b);
}

int v90_trellis_qam96_pair(V90Trellis *s,double ar,double ai,double br,double bi,
                          unsigned inversion,unsigned *out_a,unsigned *out_b)
{
    return qam_pair(s,ar,ai,br,bi,inversion,24,7,out_a,out_b);
}

unsigned v90_trellis_inversion(unsigned pair,unsigned offset)
{
    static const uint8_t pattern[14]={0,1,1,1,0,1,1,1,1,1,1,1,1,0};
    unsigned position=(pair%448+448-offset%448)%448;
    return position%32==0?pattern[position/32]:0;
}

static unsigned parity(unsigned label)
{
    return ((label>>2)-label)&1;
}
static unsigned converter_y1(unsigned label)
{
    unsigned a=label&3,b=label>>2;
    return ((a&1)&((b&1)^1))^(a>>1)^(b>>1);
}

int v90_trellis_sync(const uint8_t *labels,unsigned n,unsigned *offset,unsigned *errors)
{
    unsigned ones[448]={0},total[448]={0};
    if(n<896 || n>16384)return 0;
    for(unsigned i=0;i<n;++i)if(labels[i]>15)return 0;
    for(unsigned i=4;i<n;++i) {
        /* Eliminate the unknown four encoder memory bits. For u=Y0:
         * u[i]^u[i-3]^u[i-4] = Y2[i-3]^Y2[i-2]^Y1[i-1].
         * Observed quadrant parity is u^V0, leaving a local syndrome
         * V0[i]^V0[i-3]^V0[i-4]. Decision errors therefore stay local. */
        unsigned bit=parity(labels[i])^parity(labels[i-3])^parity(labels[i-4])^
                     (labels[i-3]&1)^(labels[i-2]&1)^converter_y1(labels[i-1]);
        ++total[i%448];ones[i%448]+=bit;
    }
    unsigned best=n,second=n,best_offset=0;
    for(unsigned candidate=0;candidate<448;++candidate) {
        unsigned score=0;
        for(unsigned i=0;i<448;++i) {
            unsigned expected=v90_trellis_inversion(i,candidate)^
                v90_trellis_inversion(i+448-3,candidate)^
                v90_trellis_inversion(i+448-4,candidate);
            score+=expected?total[i]-ones[i]:ones[i];
        }
        if(score<best){second=best;best=score;best_offset=candidate;}
        else if(score<second)second=score;
    }
    *errors=best;
    /* Reject constant/random inputs and ambiguous alignments. These are
     * acquisition thresholds, not a proof that arbitrary data is valid. */
    if(best>(n-4)/16 || second-best<(n-4)/128+1)return 0;
    *offset=best_offset;return 1;
}

int v90_trellis_acquire(const double *re,const double *im,unsigned symbols,
                       V90TrellisAcquisition *result)
{
    if(symbols<1793 || symbols>32768)return 0;
    double fourth_re=0,fourth_im=0,gain=0;
    unsigned nonzero=0;
    for(unsigned i=0;i<symbols;++i) {
        if(!isfinite(re[i]) || !isfinite(im[i]))return 0;
        double magnitude=hypot(re[i],im[i]);
        if(!isfinite(magnitude))return 0;
        gain+=magnitude;
        if(magnitude<1e-12)continue;
        double a=re[i]/magnitude,b=im[i]/magnitude;
        fourth_re+=a*a*a*a-6*a*a*b*b+b*b*b*b;
        fourth_im+=4*a*b*(a*a-b*b);++nonzero;
    }
    if(nonzero<symbols*3/4 || !isfinite(gain) || gain<1e-9)return 0;
    double coherence=hypot(fourth_re,fourth_im)/nonzero;
    if(coherence<0.5)return 0;
    double phase=atan2(fourth_im,fourth_re)/4;
    double c=cos(phase),s=sin(phase);
    uint8_t *quadrants=malloc(symbols),*labels=malloc(symbols/2);
    if(!quadrants || !labels){free(quadrants);free(labels);return 0;}
    for(unsigned i=0;i<symbols;++i) {
        double a=re[i]*c+im[i]*s,b=im[i]*c-re[i]*s;
        quadrants[i]=(uint8_t)(fabs(a)>=fabs(b)?(a>=0?0:2):(b<0?1:3));
    }
    int found=0;V90TrellisAcquisition best={0};
    for(unsigned pair=0;pair<2;++pair) {
        unsigned n=(symbols-pair)/2,offset,errors;
        for(unsigned i=0;i<n;++i)
            labels[i]=quadrants[pair+2*i]|(quadrants[pair+2*i+1]<<2);
        if(v90_trellis_sync(labels,n,&offset,&errors) &&
           (!found || (uint64_t)errors*best.pairs<(uint64_t)best.errors*n)) {
            best.phase=phase;best.gain=gain/symbols;best.coherence=coherence;
            best.pair_alignment=pair;best.offset=offset;best.errors=errors;best.pairs=n;
            found=1;
        }
    }
    free(quadrants);free(labels);
    if(found)*result=best;
    return found;
}

int v90_carrier_init(V90Carrier *s,double phase,double gain)
{
    memset(s,0,sizeof(*s));
    if(!isfinite(phase) || !isfinite(gain) || gain<=0)return 0;
    s->phase=remainder(phase,2*acos(-1.0));s->gain=gain;s->initialized=1;
    return 1;
}

int v90_carrier_normalize(V90Carrier *s,double re,double im,double *out_re,double *out_im)
{
    if(!s->initialized || !isfinite(re) || !isfinite(im))return 0;
    double magnitude=hypot(re,im);
    if(!isfinite(magnitude))return 0;
    double c=cos(s->phase),sn=sin(s->phase);
    double a=(re*c+im*sn)/s->gain,b=(im*c-re*sn)/s->gain;
    if(!isfinite(a) || !isfinite(b))return 0;
    *out_re=a;*out_im=b;
    double error=0;
    /* Ignore fades and extreme amplitude outliers in the tracking loops.
     * Keep predicting carrier phase during a fade. */
    if(magnitude>s->gain*.25 && magnitude<s->gain*4) {
        double rotation=atan2(b,a);
        error=remainder(rotation,acos(-1.0)/2);
        s->frequency+=0.00001*error;
        if(s->frequency>.02)s->frequency=.02;
        if(s->frequency<-.02)s->frequency=-.02;
        s->gain+=.001*(magnitude-s->gain);
    }
    s->phase=remainder(s->phase+s->frequency+.005*error,2*acos(-1.0));
    return 1;
}

void v90_trellis_stream_init(V90TrellisStream *s)
{
    memset(s,0,sizeof(*s));
}
static void stream_locked(V90TrellisStream *s,double re,double im)
{
    double ar,ai;
    if(!v90_carrier_normalize(&s->carrier,re,im,&ar,&ai)) {
        s->locked=0;s->count=0;s->have_a=0;return;
    }
    if(!s->have_a){s->a_re=ar;s->a_im=ai;s->have_a=1;return;}
    unsigned a,b;
    unsigned inversion=v90_trellis_inversion((unsigned)(s->pair_index%448),s->acquisition.offset);
    int ready=v90_trellis_pair(&s->trellis,s->a_re,s->a_im,ar,ai,inversion,&a,&b);
    ++s->pair_index;s->have_a=0;
    if(ready) {
        s->output_symbol=s->pair_origin+2*(s->pair_index-(V90_TRELLIS_DEPTH-1))-1;
        if(s->receive_pair)s->receive_pair(s->opaque,a,b);
    }
}
void v90_trellis_stream_symbol(V90TrellisStream *s,double re,double im)
{
    uint64_t index=s->symbols++;
    if(!isfinite(re) || !isfinite(im)) {
        /* Do not silently lose a symbol and shift all subsequent 4D pairs. */
        s->locked=0;s->count=0;s->have_a=0;return;
    }
    if(s->locked){stream_locked(s,re,im);return;}
    if(!s->count)s->pair_origin=index;
    s->re[s->count]=re;s->im[s->count++]=im;
    if(s->count<V90_STREAM_BUFFER)return;
    if(!v90_trellis_acquire(s->re,s->im,s->count,&s->acquisition)) {
        s->count=V90_STREAM_BUFFER/2;
        s->pair_origin+=s->count;
        memmove(s->re,s->re+s->count,s->count*sizeof(double));
        memmove(s->im,s->im+s->count,s->count*sizeof(double));
        return;
    }
    s->pair_origin+=s->acquisition.pair_alignment;
    v90_carrier_init(&s->carrier,s->acquisition.phase,s->acquisition.gain);
    v90_trellis_init(&s->trellis);s->pair_index=0;s->have_a=0;s->locked=1;
    for(unsigned i=s->acquisition.pair_alignment;i<s->count;++i)
        stream_locked(s,s->re[i],s->im[i]);
    s->count=0;
}

int v90_trellis_qam160_pair(V90Trellis *s,double ar,double ai,double br,double bi,
                           unsigned inversion,unsigned *out_a,unsigned *out_b)
{
    return qam_pair(s,ar,ai,br,bi,inversion,40,8,out_a,out_b);
}

int v90_trellis_qam256_pair(V90Trellis *s,double ar,double ai,double br,double bi,
                           unsigned inversion,unsigned *out_a,unsigned *out_b)
{
    return qam_pair(s,ar,ai,br,bi,inversion,64,8,out_a,out_b);
}

int v90_trellis_qam448_pair(V90Trellis *s,double ar,double ai,double br,double bi,
                           unsigned inversion,unsigned *out_a,unsigned *out_b)
{
    return qam_pair(s,ar,ai,br,bi,inversion,112,9,out_a,out_b);
}

int v90_trellis_qam768_pair(V90Trellis *s,double ar,double ai,double br,double bi,unsigned inversion,unsigned *a,unsigned *b) {return qam_pair(s,ar,ai,br,bi,inversion,192,10,a,b);}

int v90_trellis_qam1280_pair(V90Trellis*s,double ar,double ai,double br,double bi,unsigned inversion,unsigned*a,unsigned*b){return qam_pair(s,ar,ai,br,bi,inversion,320,11,a,b);}

int v90_trellis_qam_constellation_pair(V90Trellis *s,double ar,double ai,double br,double bi,
    unsigned inversion,unsigned points,unsigned *a,unsigned *b)
{
    if(!s || !a || !b || inversion>1 || points<4 || points>1280 || points%4)return -1;
    unsigned bits=2;while((1u<<bits)<points)++bits;
    return qam_pair(s,ar,ai,br,bi,inversion,points/4,bits,a,b);
}
