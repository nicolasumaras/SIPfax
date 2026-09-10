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
             * supported q=0 constellations those bits are the quadrant;
             * ring-dependent upper subset bits do not affect this encoder. */
            unsigned as0=a&1,bs0=b&1,as1=a>>1,bs1=b>>1;
            unsigned y1=(as0&(bs0^1))^as1^bs1,y2=as0;
            unsigned dest=(state>>1)^y1^(y2<<1)^((y2^u)<<2)^(u<<3);
            double cost=s->metric[state]+ca[a]+cb[b];
            if(cost<next[dest]) {
                next[dest]=cost;s->previous[slot][dest]=(uint8_t)state;
                s->labels[slot][dest]=(uint16_t)(labels_a[a]|(labels_b[b]<<label_bits));
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
    static const double re[96]={1,1,-1,-1,-3,1,3,-1,1,-3,-1,3,-3,-3,3,3,1,5,-1,-5,5,1,-5,-1,-3,5,3,-5,5,-3,-5,3,5,5,-5,-5,-7,1,7,-1,1,-7,-1,7,-7,-3,7,3,-3,-7,3,7,-7,5,7,-5,5,-7,-5,7,1,9,-1,-9,9,1,-9,-1,-3,9,3,-9,9,-3,-9,3,-7,-7,7,7,5,9,-5,-9,9,5,-9,-5,-11,1,11,-1,1,-11,-1,11};
    static const double im[96]={1,-1,-1,1,1,3,-1,-3,-3,-1,3,1,-3,3,3,-3,5,-1,-5,1,1,-5,-1,5,5,3,-5,-3,-3,-5,3,5,5,-5,-5,5,1,7,-1,-7,-7,-1,7,1,-3,7,3,-7,-7,3,7,-3,5,7,-5,-7,-7,-5,7,5,9,-1,-9,1,1,-9,-1,9,9,3,-9,-3,-3,-9,3,9,-7,7,7,-7,9,-5,-9,5,5,-9,-5,9,1,11,-1,-11,-11,-1,11,1};
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
