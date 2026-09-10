/* V.34 9.3/9.5 inverse for K=6, q=0, b=18 (all high frames). GPL-2.0. */
#include <string.h>
#include <math.h>
#include "v90qam8.h"
void v90_qam8_frames_init(V90Qam8Frames *s,unsigned previous)
{
    v90_shell_init(&s->shell,2,6);s->previous=previous&3;
}
int v90_qam8_frame(V90Qam8Frames *s,const uint8_t labels[8],uint8_t bits[18])
{
    uint8_t rings[8],decoded[18];uint32_t index;
    for(unsigned i=0;i<8;++i){if(labels[i]>7)return 0;rings[i]=labels[i]>>2;}
    unsigned previous=s->previous;s->previous=labels[6]&3;
    if(!v90_shell_decode(&s->shell,rings,&index))return 0;
    for(unsigned i=0;i<6;++i)decoded[i]=(index>>i)&1;
    for(unsigned pair=0;pair<4;++pair) {
        unsigned a=labels[2*pair]&3,b=labels[2*pair+1]&3;
        unsigned difference=(a+4-previous)&3;
        decoded[6+3*pair]=((b+4-a)&3)>>1;
        decoded[7+3*pair]=difference&1;
        decoded[8+3*pair]=difference>>1;
        previous=a;
    }
    memcpy(bits,decoded,sizeof(decoded));return 1;
}

static void point(unsigned label,double *re,double *im)
{
    static const double r[8]={1,1,-1,-1,-3,1,3,-1};
    static const double j[8]={1,-1,-1,1,1,3,-1,-3};
    *re=r[label];*im=j[label];
}
void v90_qam8_b1_init(V90Qam8B1 *s)
{
    /* V.34 10.1.3.1: zero encoders, scrambled ones, last J=7 data frame.
     * 16 mapping frames, each 18 bits / 8 symbols. No auxiliary channel. */
    memset(s,0,sizeof(*s));
    V90Shell shell;v90_shell_init(&shell,2,6);
    uint8_t bits[288];unsigned state=0,previous=0;
    for(unsigned i=0;i<288;++i)
        bits[i]=1^(i>=5?bits[i-5]:0)^(i>=23?bits[i-23]:0);
    for(unsigned f=0;f<16;++f) {
        uint32_t index=0;uint8_t rings[8];
        for(unsigned k=0;k<6;++k)index|=(uint32_t)bits[18*f+k]<<k;
        v90_shell_encode(&shell,index,rings);
        for(unsigned p=0;p<4;++p) {
            unsigned k=18*f+6+3*p,i=8*f+2*p;
            unsigned a=(previous+bits[k+1]+2*bits[k+2])&3;
            unsigned u=state&1,b=(a+2*bits[k]+(u^(i==0)))&3;
            s->labels[i]=a+4*rings[2*p];s->labels[i+1]=b+4*rings[2*p+1];
            unsigned y1=((a&1)&((b&1)^1))^(a>>1)^(b>>1),y2=a&1;
            state=(state>>1)^y1^(y2<<1)^((y2^u)<<2)^(u<<3);previous=a;
        }
    }
    for(unsigned i=0;i<V90_QAM8_B1_SYMBOLS;++i) {
        double re,im;point(s->labels[i],&re,&im);
        s->reference_energy+=re*re+im*im;
    }
}
int v90_qam8_b1_symbol(V90Qam8B1 *s,double re,double im,
                      double *gain,double *phase,double *score)
{
    if(!isfinite(re)||!isfinite(im)||fabs(re)>1e100||fabs(im)>1e100) {
        s->position=s->count=0;return 0;
    }
    s->re[s->position]=re;s->im[s->position]=im;
    s->position=(s->position+1)%V90_QAM8_B1_SYMBOLS;
    if(s->count<V90_QAM8_B1_SYMBOLS)++s->count;
    if(s->count<V90_QAM8_B1_SYMBOLS)return 0;
    double cr=0,ci=0,energy=0;
    for(unsigned i=0;i<V90_QAM8_B1_SYMBOLS;++i) {
        unsigned j=(s->position+i)%V90_QAM8_B1_SYMBOLS;
        double rr,ri;point(s->labels[i],&rr,&ri);
        double ar=s->re[j],ai=s->im[j];
        cr+=ar*rr+ai*ri;ci+=ai*rr-ar*ri;energy+=ar*ar+ai*ai;
    }
    double magnitude=hypot(cr,ci),denominator=sqrt(energy)*sqrt(s->reference_energy);
    if(!(denominator>0)||!isfinite(denominator)||magnitude<.95*denominator)return 0;
    *gain=magnitude/s->reference_energy;*phase=atan2(ci,cr);
    *score=magnitude/denominator;return 1;
}
