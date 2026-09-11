/* V.34 9.3/9.5 inverse for q=0/1/2/3/4/5, all high mapping frames. GPL-2.0. */
#include <string.h>
#include <math.h>
#include "v90qam8.h"
void v90_qam8_frames_init(V90Qam8Frames *s,unsigned previous)
{
    v90_shell_init(&s->shell,2,6);s->previous=previous&3;s->q=0;
}
static int mapping_frame(V90Qam8Frames *s,const uint16_t labels[8],uint8_t *bits,
                         unsigned m,unsigned k,unsigned q)
{
    uint8_t rings[8],decoded[78];uint32_t index;
    if(s->shell.m!=m || s->shell.k!=k || s->q!=q)return 0;
    for(unsigned i=0;i<8;++i){if(labels[i]>=(4*m<<q))return 0;rings[i]=labels[i]>>(2+q);}
    unsigned previous=s->previous;s->previous=labels[6]&3;
    if(!v90_shell_decode(&s->shell,rings,&index))return 0;
    for(unsigned i=0;i<k;++i)decoded[i]=(index>>i)&1;
    for(unsigned pair=0;pair<4;++pair) {
        unsigned a=labels[2*pair]&3,b=labels[2*pair+1]&3;
        unsigned difference=(a+4-previous)&3;
        unsigned group=k+(3+2*q)*pair;
        decoded[group]=((b+4-a)&3)>>1;
        decoded[group+1]=difference&1;
        decoded[group+2]=difference>>1;
        for(unsigned j=0;j<q;++j) {
            decoded[group+3+j]=(labels[2*pair]>>(2+j))&1;
            decoded[group+3+q+j]=(labels[2*pair+1]>>(2+j))&1;
        }
        previous=a;
    }
    memcpy(bits,decoded,k+12+8*q);return 1;
}
static int mapping_frame_narrow(V90Qam8Frames *s,const uint8_t labels[8],uint8_t *bits,
                                unsigned m,unsigned k,unsigned q)
{
    uint16_t wide[8];for(unsigned i=0;i<8;++i)wide[i]=labels[i];
    return mapping_frame(s,wide,bits,m,k,q);
}

int v90_qam8_frame(V90Qam8Frames *s,const uint8_t labels[8],uint8_t bits[18])
{
    return mapping_frame_narrow(s,labels,bits,2,6,0);
}
void v90_qam12_frames_init(V90Qam12Frames *s,unsigned previous)
{
    v90_shell_init(&s->shell,3,12);s->previous=previous&3;s->q=0;
}
int v90_qam12_frame(V90Qam12Frames *s,const uint8_t labels[8],uint8_t bits[24])
{
    return mapping_frame_narrow(s,labels,bits,3,12,0);
}

void v90_qam20_frames_init(V90Qam20Frames *s,unsigned previous)
{
    v90_shell_init(&s->shell,5,18);s->previous=previous&3;s->q=0;
}
int v90_qam20_frame(V90Qam20Frames *s,const uint8_t labels[8],uint8_t bits[30])
{
    return mapping_frame_narrow(s,labels,bits,5,18,0);
}

void v90_qam32_frames_init(V90Qam32Frames *s,unsigned previous)
{
    v90_shell_init(&s->shell,8,24);s->previous=previous&3;s->q=0;
}
int v90_qam32_frame(V90Qam32Frames *s,const uint8_t labels[8],uint8_t bits[36])
{
    return mapping_frame_narrow(s,labels,bits,8,24,0);
}

void v90_qam56_frames_init(V90Qam56Frames *s,unsigned previous)
{
    v90_shell_init(&s->shell,14,30);s->previous=previous&3;s->q=0;
}
int v90_qam56_frame(V90Qam56Frames *s,const uint8_t labels[8],uint8_t bits[42])
{
    return mapping_frame_narrow(s,labels,bits,14,30,0);
}

void v90_qam96_frames_init(V90Qam96Frames *s,unsigned previous)
{
    v90_shell_init(&s->shell,12,28);s->previous=previous&3;s->q=1;
}
int v90_qam96_frame(V90Qam96Frames *s,const uint8_t labels[8],uint8_t bits[48])
{
    return mapping_frame_narrow(s,labels,bits,12,28,1);
}

void v90_qam160_frames_init(V90Qam160Frames *s,unsigned previous)
{
    v90_shell_init(&s->shell,10,26);s->previous=previous&3;s->q=2;
}
int v90_qam160_frame(V90Qam160Frames *s,const uint8_t labels[8],uint8_t bits[54])
{
    return mapping_frame_narrow(s,labels,bits,10,26,2);
}

void v90_qam256_frames_init(V90Qam256Frames *s,unsigned previous)
{
    v90_shell_init(&s->shell,8,24);s->previous=previous&3;s->q=3;
}
int v90_qam256_frame(V90Qam256Frames *s,const uint8_t labels[8],uint8_t bits[60])
{
    return mapping_frame_narrow(s,labels,bits,8,24,3);
}

void v90_qam448_frames_init(V90Qam448Frames *s,unsigned previous)
{
    v90_shell_init(&s->shell,14,30);s->previous=previous&3;s->q=3;
}
int v90_qam448_frame(V90Qam448Frames *s,const uint16_t labels[8],uint8_t bits[66])
{
    return mapping_frame(s,labels,bits,14,30,3);
}

static void point(unsigned label,double *re,double *im)
{
    static const double r[1280]={1,1,-1,-1,-3,1,3,-1,1,-3,-1,3,-3,-3,3,3,1,5,-1,-5,5,1,-5,-1,-3,5,3,-5,5,-3,-5,3,5,5,-5,-5,-7,1,7,-1,1,-7,-1,7,-7,-3,7,3,-3,-7,3,7,-7,5,7,-5,5,-7,-5,7,1,9,-1,-9,9,1,-9,-1,-3,9,3,-9,9,-3,-9,3,-7,-7,7,7,5,9,-5,-9,9,5,-9,-5,-11,1,11,-1,1,-11,-1,11,-7,9,7,-9,-11,-3,11,3,9,-7,-9,7,-3,-11,3,11,-11,5,11,-5,5,-11,-5,11,9,9,-9,-9,1,13,-1,-13,13,1,-13,-1,-11,-7,11,7,-7,-11,7,11,-3,13,3,-13,13,-3,-13,3,5,13,-5,-13,13,5,-13,-5,-11,9,11,-9,9,-11,-9,11,-7,13,7,-13,13,-7,-13,7,-15,1,15,-1,1,-15,-1,15,-15,-3,15,3,-3,-15,3,15,-11,-11,11,11,9,13,-9,-13,13,9,-13,-9,-15,5,15,-5,5,-15,-5,15,-15,-7,15,7,-7,-15,7,15,1,17,-1,-17,-11,13,11,-13,17,1,-17,-1,13,-11,-13,11,-3,17,3,-17,17,-3,-17,3,-15,9,15,-9,9,-15,-9,15,5,17,-5,-17,17,5,-17,-5,-7,17,7,-17,13,13,-13,-13,17,-7,-17,7,-15,-11,15,11,-11,-15,11,15,-19,1,19,-1,1,-19,-1,19,9,17,-9,-17,17,9,-17,-9,-19,-3,19,3,-3,-19,3,19,-19,5,19,-5,5,-19,-5,19,-15,13,15,-13,13,-15,-13,15,-11,17,11,-17,-19,-7,19,7,17,-11,-17,11,-7,-19,7,19,1,21,-1,-21,-19,9,19,-9,21,1,-21,-1,9,-19,-9,19,-3,21,3,-21,21,-3,-21,3,-15,-15,15,15,13,17,-13,-17,17,13,-17,-13,5,21,-5,-21,21,5,-21,-5,-19,-11,19,11,-11,-19,11,19,-7,21,7,-21,21,-7,-21,7,-15,17,15,-17,17,-15,-17,15,9,21,-9,-21,21,9,-21,-9,-19,13,19,-13,-23,1,23,-1,13,-19,-13,19,1,-23,-1,23,-23,-3,23,3,-3,-23,3,23,-23,5,23,-5,5,-23,-5,23,-11,21,11,-21,21,-11,-21,11,17,17,-17,-17,-23,-7,23,7,-7,-23,7,23,-19,-15,19,15,-15,-19,15,19,13,21,-13,-21,21,13,-21,-13,-23,9,23,-9,9,-23,-9,23,1,25,-1,-25,25,1,-25,-1,-3,25,3,-25,25,-3,-25,3,5,25,-5,-25,-19,17,19,-17,25,5,-25,-5,-23,-11,23,11,17,-19,-17,19,-11,-23,11,23,-15,21,15,-21,21,-15,-21,15,-7,25,7,-25,25,-7,-25,7,-23,13,23,-13,13,-23,-13,23,9,25,-9,-25,25,9,-25,-9,-19,-19,19,19,17,21,-17,-21,21,17,-21,-17,-27,1,27,-1,1,-27,-1,27,-27,-3,27,3,-3,-27,3,27,-11,25,11,-25,25,-11,-25,11,-27,5,27,-5,-23,-15,23,15,-15,-23,15,23,5,-27,-5,27,-27,-7,27,7,-7,-27,7,27,13,25,-13,-25,25,13,-25,-13,-19,21,19,-21,21,-19,-21,19,-27,9,27,-9,9,-27,-9,27,-23,17,23,-17,17,-23,-17,23,1,29,-1,-29,29,1,-29,-1,-3,29,3,-29,-15,25,15,-25,29,-3,-29,3,-27,-11,27,11,25,-15,-25,15,-11,-27,11,27,5,29,-5,-29,29,5,-29,-5,21,21,-21,-21,-7,29,7,-29,29,-7,-29,7,-23,-19,23,19,-19,-23,19,23,-27,13,27,-13,13,-27,-13,27,17,25,-17,-25,25,17,-25,-17,9,29,-9,-29,29,9,-29,-9,-27,-15,27,15,-15,-27,15,27,-11,29,11,-29,-31,1,31,-1,29,-11,-29,11,1,-31,-1,31,-23,21,23,-21,-31,-3,31,3,21,-23,-21,23,-3,-31,3,31,-19,25,19,-25,-31,5,31,-5,25,-19,-25,19,5,-31,-5,31,13,29,-13,-29,29,13,-29,-13,-31,-7,31,7,-7,-31,7,31,-27,17,27,-17,17,-27,-17,27,-31,9,31,-9,9,-31,-9,31,-23,-23,23,23,-15,29,15,-29,21,25,-21,-25,25,21,-25,-21,29,-15,-29,15,-31,-11,31,11,-11,-31,11,31,1,33,-1,-33,33,1,-33,-1,-27,-19,27,19,-19,-27,19,27,-3,33,3,-33,33,-3,-33,3,5,33,-5,-33,33,5,-33,-5,17,29,-17,-29,29,17,-29,-17,-31,13,31,-13,13,-31,-13,31,-7,33,7,-33,33,-7,-33,7,-23,25,23,-25,25,-23,-25,23,9,33,-9,-33,-27,21,27,-21,33,9,-33,-9,21,-27,-21,27,-31,-15,31,15,-15,-31,15,31,-19,29,19,-29,29,-19,-29,19,-11,33,11,-33,33,-11,-33,11,-35,1,35,-1,1,-35,-1,35,-35,-3,35,3,-3,-35,3,35,25,25,-25,-25,-31,17,31,-17,-35,5,35,-5,17,-31,-17,31,5,-35,-5,35,13,33,-13,-33,33,13,-33,-13,-27,-23,27,23,-23,-27,23,27,-35,-7,35,7,-7,-35,7,35,21,29,-21,-29,29,21,-29,-21,-35,9,35,-9,9,-35,-9,35,-15,33,15,-33,33,-15,-33,15,-31,-19,31,19,-19,-31,19,31,-35,-11,35,11,-11,-35,11,35,-27,25,27,-25,25,-27,-25,27,1,37,-1,-37,-23,29,23,-29,37,1,-37,-1,29,-23,-29,23,-3,37,3,-37,17,33,-17,-33,33,17,-33,-17,37,-3,-37,3,5,37,-5,-37,-35,13,35,-13,37,5,-37,-5,13,-35,-13,35,-31,21,31,-21,21,-31,-21,31,-7,37,7,-37,37,-7,-37,7,9,37,-9,-37,-19,33,19,-33,37,9,-37,-9,-35,-15,35,15,33,-19,-33,19,-15,-35,15,35,-27,-27,27,27,25,29,-25,-29,29,25,-29,-25,-11,37,11,-37,37,-11,-37,11,-31,-23,31,23,-23,-31,23,31,-35,17,35,-17,17,-35,-17,35,-39,1,39,-1,1,-39,-1,39,21,33,-21,-33,33,21,-33,-21,-39,-3,39,3,-3,-39,3,39,13,37,-13,-37,37,13,-37,-13,-39,5,39,-5,5,-39,-5,39,-27,29,27,-29,-39,-7,39,7,29,-27,-29,27,-7,-39,7,39,-31,25,31,-25,-35,-19,35,19,25,-31,-25,31,-19,-35,19,35,-15,37,15,-37,37,-15,-37,15,-39,9,39,-9,9,-39,-9,39,-23,33,23,-33,33,-23,-33,23};
    static const double j[1280]={1,-1,-1,1,1,3,-1,-3,-3,-1,3,1,-3,3,3,-3,5,-1,-5,1,1,-5,-1,5,5,3,-5,-3,-3,-5,3,5,5,-5,-5,5,1,7,-1,-7,-7,-1,7,1,-3,7,3,-7,-7,3,7,-3,5,7,-5,-7,-7,-5,7,5,9,-1,-9,1,1,-9,-1,9,9,3,-9,-3,-3,-9,3,9,-7,7,7,-7,9,-5,-9,5,5,-9,-5,9,1,11,-1,-11,-11,-1,11,1,9,7,-9,-7,-3,11,3,-11,-7,-9,7,9,-11,3,11,-3,5,11,-5,-11,-11,-5,11,5,9,-9,-9,9,13,-1,-13,1,1,-13,-1,13,-7,11,7,-11,-11,7,11,-7,13,3,-13,-3,-3,-13,3,13,13,-5,-13,5,5,-13,-5,13,9,11,-9,-11,-11,-9,11,9,13,7,-13,-7,-7,-13,7,13,1,15,-1,-15,-15,-1,15,1,-3,15,3,-15,-15,3,15,-3,-11,11,11,-11,13,-9,-13,9,9,-13,-9,13,5,15,-5,-15,-15,-5,15,5,-7,15,7,-15,-15,7,15,-7,17,-1,-17,1,13,11,-13,-11,1,-17,-1,17,-11,-13,11,13,17,3,-17,-3,-3,-17,3,17,9,15,-9,-15,-15,-9,15,9,17,-5,-17,5,5,-17,-5,17,17,7,-17,-7,13,-13,-13,13,-7,-17,7,17,-11,15,11,-15,-15,11,15,-11,1,19,-1,-19,-19,-1,19,1,17,-9,-17,9,9,-17,-9,17,-3,19,3,-19,-19,3,19,-3,5,19,-5,-19,-19,-5,19,5,13,15,-13,-15,-15,-13,15,13,17,11,-17,-11,-7,19,7,-19,-11,-17,11,17,-19,7,19,-7,21,-1,-21,1,9,19,-9,-19,1,-21,-1,21,-19,-9,19,9,21,3,-21,-3,-3,-21,3,21,-15,15,15,-15,17,-13,-17,13,13,-17,-13,17,21,-5,-21,5,5,-21,-5,21,-11,19,11,-19,-19,11,19,-11,21,7,-21,-7,-7,-21,7,21,17,15,-17,-15,-15,-17,15,17,21,-9,-21,9,9,-21,-9,21,13,19,-13,-19,1,23,-1,-23,-19,-13,19,13,-23,-1,23,1,-3,23,3,-23,-23,3,23,-3,5,23,-5,-23,-23,-5,23,5,21,11,-21,-11,-11,-21,11,21,17,-17,-17,17,-7,23,7,-23,-23,7,23,-7,-15,19,15,-19,-19,15,19,-15,21,-13,-21,13,13,-21,-13,21,9,23,-9,-23,-23,-9,23,9,25,-1,-25,1,1,-25,-1,25,25,3,-25,-3,-3,-25,3,25,25,-5,-25,5,17,19,-17,-19,5,-25,-5,25,-11,23,11,-23,-19,-17,19,17,-23,11,23,-11,21,15,-21,-15,-15,-21,15,21,25,7,-25,-7,-7,-25,7,25,13,23,-13,-23,-23,-13,23,13,25,-9,-25,9,9,-25,-9,25,-19,19,19,-19,21,-17,-21,17,17,-21,-17,21,1,27,-1,-27,-27,-1,27,1,-3,27,3,-27,-27,3,27,-3,25,11,-25,-11,-11,-25,11,25,5,27,-5,-27,-15,23,15,-23,-23,15,23,-15,-27,-5,27,5,-7,27,7,-27,-27,7,27,-7,25,-13,-25,13,13,-25,-13,25,21,19,-21,-19,-19,-21,19,21,9,27,-9,-27,-27,-9,27,9,17,23,-17,-23,-23,-17,23,17,29,-1,-29,1,1,-29,-1,29,29,3,-29,-3,25,15,-25,-15,-3,-29,3,29,-11,27,11,-27,-15,-25,15,25,-27,11,27,-11,29,-5,-29,5,5,-29,-5,29,21,-21,-21,21,29,7,-29,-7,-7,-29,7,29,-19,23,19,-23,-23,19,23,-19,13,27,-13,-27,-27,-13,27,13,25,-17,-25,17,17,-25,-17,25,29,-9,-29,9,9,-29,-9,29,-15,27,15,-27,-27,15,27,-15,29,11,-29,-11,1,31,-1,-31,-11,-29,11,29,-31,-1,31,1,21,23,-21,-23,-3,31,3,-31,-23,-21,23,21,-31,3,31,-3,25,19,-25,-19,5,31,-5,-31,-19,-25,19,25,-31,-5,31,5,29,-13,-29,13,13,-29,-13,29,-7,31,7,-31,-31,7,31,-7,17,27,-17,-27,-27,-17,27,17,9,31,-9,-31,-31,-9,31,9,-23,23,23,-23,29,15,-29,-15,25,-21,-25,21,21,-25,-21,25,-15,-29,15,29,-11,31,11,-31,-31,11,31,-11,33,-1,-33,1,1,-33,-1,33,-19,27,19,-27,-27,19,27,-19,33,3,-33,-3,-3,-33,3,33,33,-5,-33,5,5,-33,-5,33,29,-17,-29,17,17,-29,-17,29,13,31,-13,-31,-31,-13,31,13,33,7,-33,-7,-7,-33,7,33,25,23,-25,-23,-23,-25,23,25,33,-9,-33,9,21,27,-21,-27,9,-33,-9,33,-27,-21,27,21,-15,31,15,-31,-31,15,31,-15,29,19,-29,-19,-19,-29,19,29,33,11,-33,-11,-11,-33,11,33,1,35,-1,-35,-35,-1,35,1,-3,35,3,-35,-35,3,35,-3,25,-25,-25,25,17,31,-17,-31,5,35,-5,-35,-31,-17,31,17,-35,-5,35,5,33,-13,-33,13,13,-33,-13,33,-23,27,23,-27,-27,23,27,-23,-7,35,7,-35,-35,7,35,-7,29,-21,-29,21,21,-29,-21,29,9,35,-9,-35,-35,-9,35,9,33,15,-33,-15,-15,-33,15,33,-19,31,19,-31,-31,19,31,-19,-11,35,11,-35,-35,11,35,-11,25,27,-25,-27,-27,-25,27,25,37,-1,-37,1,29,23,-29,-23,1,-37,-1,37,-23,-29,23,29,37,3,-37,-3,33,-17,-33,17,17,-33,-17,33,-3,-37,3,37,37,-5,-37,5,13,35,-13,-35,5,-37,-5,37,-35,-13,35,13,21,31,-21,-31,-31,-21,31,21,37,7,-37,-7,-7,-37,7,37,37,-9,-37,9,33,19,-33,-19,9,-37,-9,37,-15,35,15,-35,-19,-33,19,33,-35,15,35,-15,-27,27,27,-27,29,-25,-29,25,25,-29,-25,29,37,11,-37,-11,-11,-37,11,37,-23,31,23,-31,-31,23,31,-23,17,35,-17,-35,-35,-17,35,17,1,39,-1,-39,-39,-1,39,1,33,-21,-33,21,21,-33,-21,33,-3,39,3,-39,-39,3,39,-3,37,-13,-37,13,13,-37,-13,37,5,39,-5,-39,-39,-5,39,5,29,27,-29,-27,-7,39,7,-39,-27,-29,27,29,-39,7,39,-7,25,31,-25,-31,-19,35,19,-35,-31,-25,31,25,-35,19,35,-19,37,15,-37,-15,-15,-37,15,37,9,39,-9,-39,-39,-9,39,9,33,23,-33,-23,-23,-33,23,33};
    *re=r[label];*im=j[label];
}
int v90_qam_b1_init_rate(V90Qam8B1 *s,unsigned rate)
{
    /* V.34 10.1.3.1: zero encoders, scrambled ones, last J=7 data frame.
     * 16 mapping frames, 18 through 60 bits / 8 symbols. No auxiliary channel. */
    memset(s,0,sizeof(*s));
    if(rate!=7200 && rate!=9600 && rate!=12000 && rate!=14400 && rate!=16800 && rate!=19200 && rate!=21600 && rate!=24000 && rate!=26400 && rate!=28800 && rate!=31200)return 0;
    s->m=rate==31200?10:rate==28800?12:rate==7200?2:rate==9600?3:rate==12000?5:rate==14400?8:rate==16800?14:rate==19200?12:rate==21600?10:rate==24000?8:14;s->q=rate==31200?5:rate==28800?4:rate>=24000?3:rate==21600?2:rate==19200?1:0;
    s->k=rate/400-12-8*s->q;
    unsigned frame_bits=s->k+12+8*s->q;
    V90Shell shell;v90_shell_init(&shell,s->m,s->k);
    uint8_t bits[1248];unsigned state=0,previous=0;
    for(unsigned i=0;i<16*frame_bits;++i)
        bits[i]=1^(i>=5?bits[i-5]:0)^(i>=23?bits[i-23]:0);
    for(unsigned f=0;f<16;++f) {
        uint32_t index=0;uint8_t rings[8];
        for(unsigned k=0;k<s->k;++k)index|=(uint32_t)bits[frame_bits*f+k]<<k;
        v90_shell_encode(&shell,index,rings);
        for(unsigned p=0;p<4;++p) {
            unsigned k=frame_bits*f+s->k+(3+2*s->q)*p,i=8*f+2*p;
            unsigned a=(previous+bits[k+1]+2*bits[k+2])&3;
            unsigned u=state&1,b=(a+2*bits[k]+(u^(i==0)))&3;
            unsigned qa=0,qb=0;
            for(unsigned j=0;j<s->q;++j){qa|=(unsigned)bits[k+3+j]<<j;qb|=(unsigned)bits[k+3+s->q+j]<<j;}
            s->labels[i]=a+4*((rings[2*p]<<s->q)|qa);
            s->labels[i+1]=b+4*((rings[2*p+1]<<s->q)|qb);
            unsigned y1=((a&1)&((b&1)^1))^(a>>1)^(b>>1),y2=a&1;
            state=(state>>1)^y1^(y2<<1)^((y2^u)<<2)^(u<<3);previous=a;
        }
    }
    for(unsigned i=0;i<V90_QAM8_B1_SYMBOLS;++i) {
        double re,im;point(s->labels[i],&re,&im);
        s->reference_energy+=re*re+im*im;
    }
    return 1;
}
void v90_qam8_b1_init(V90Qam8B1 *s)
{
    v90_qam_b1_init_rate(s,7200);
}
int v90_qam8_b1_symbol(V90Qam8B1 *s,double re,double im,
                      double *gain,double *phase,double *score)
{
    if(!s->m || !isfinite(re)||!isfinite(im)||fabs(re)>1e100||fabs(im)>1e100) {
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

void v90_qam8_stream_init(V90Qam8Stream *s)
{
    v90_qam_stream_init_rate(s,7200);
}
int v90_qam_stream_init_rate(V90Qam8Stream *s,unsigned rate)
{
    memset(s,0,sizeof(*s));return v90_qam_b1_init_rate(&s->b1,rate);
}
static int normalized(V90Qam8Stream *s,double re,double im,double *ar,double *ai)
{
    V90Carrier *c=&s->carrier;
    double cs=cos(c->phase),sn=sin(c->phase);
    *ar=(re*cs+im*sn)/c->gain;*ai=(im*cs-re*sn)/c->gain;
    /* Normalize the earlier midpoint with its own carrier phase. Keep only
     * symbol-time FIR outputs, preserving the seven-symbol output delay. */
    if(s->equalizer.taps==V90_EQ_HALF_TAPS) {
        double mcs=cos(c->phase-.5*c->frequency),msn=sin(c->phase-.5*c->frequency),ignored_re,ignored_im;
        v90_equalizer_symbol(&s->equalizer,(s->mid_re*mcs+s->mid_im*msn)/c->gain,
            (s->mid_im*mcs-s->mid_re*msn)/c->gain,&ignored_re,&ignored_im);
    }
    if(s->b1.m>=5 && v90_equalizer_symbol(&s->equalizer,*ar,*ai,ar,ai)!=1) {
        c->phase=remainder(c->phase+c->frequency,2*acos(-1.0));return 0;
    }
    double best=1e300,rr=1,ri=1;
    for(unsigned i=0;i<(4*s->b1.m<<s->b1.q);++i) {
        double r,j;point(i,&r,&j);
        double distance=(*ar-r)*(*ar-r)+(*ai-j)*(*ai-j);
        if(distance<best){best=distance;rr=r;ri=j;}
    }
    /* Higher-rate tracking uses a wider confidence gate (still inside half
     * the minimum point spacing) and a faster bounded NLMS update. */
    if(s->b1.q<3 && (s->b1.m>=14 || s->b1.q) && best<(s->b1.q>=2?.5:.25))
        v90_equalizer_adapt(&s->equalizer,rr,ri,s->b1.q>=2?.05:.01);
    double error=0;
    /* Track against the decided point, not average magnitude: ring energy
     * carries shell bits. Ignore fades/outliers while predicting phase. */
    double ratio=hypot(*ar,*ai)/hypot(rr,ri);
    if(ratio>.5 && ratio<1.5 && best<1) {
        error=atan2(*ai*rr-*ar*ri,*ar*rr+*ai*ri);
        c->frequency+=.00001*error;
        if(c->frequency>.02)c->frequency=.02;
        if(c->frequency<-.02)c->frequency=-.02;
        c->gain*=1+.001*(ratio-1);
    }
    c->phase=remainder(c->phase+c->frequency+.005*error,2*acos(-1.0));
    return 1;
}
static void qam8_locked(V90Qam8Stream *s,double re,double im)
{
    double ar,ai;if(!normalized(s,re,im,&ar,&ai))return;
    if(s->b1.q>=3) {
        unsigned h=(unsigned)(2*s->pairs+s->have_a)%V90_QAM_FEEDBACK_HISTORY;
        for(unsigned j=0;j<s->equalizer.taps;++j) {
            unsigned k=(s->equalizer.position+j)%s->equalizer.taps;
            s->history_re[h][j]=s->equalizer.re[k];
            s->history_im[h][j]=s->equalizer.im[k];
        }
    }
    if(!s->have_a){s->a_re=ar;s->a_im=ai;s->have_a=1;return;}
    unsigned a,b;
    /* B1 is the last 64 pairs of J=7. The following data starts at V0[0]. */
    unsigned inv=v90_trellis_inversion((unsigned)((s->pairs+384)%448),0);
    int ready=s->b1.q==5?v90_trellis_qam1280_pair(&s->trellis,s->a_re,s->a_im,ar,ai,inv,&a,&b):s->b1.q>=4?v90_trellis_qam768_pair(&s->trellis,s->a_re,s->a_im,ar,ai,inv,&a,&b):s->b1.q==3 && s->b1.m==14?
        v90_trellis_qam448_pair(&s->trellis,s->a_re,s->a_im,ar,ai,inv,&a,&b):s->b1.q>=3?
        v90_trellis_qam256_pair(&s->trellis,s->a_re,s->a_im,ar,ai,inv,&a,&b):s->b1.q==2?
        v90_trellis_qam160_pair(&s->trellis,s->a_re,s->a_im,ar,ai,inv,&a,&b):s->b1.q==1?
        v90_trellis_qam96_pair(&s->trellis,s->a_re,s->a_im,ar,ai,inv,&a,&b):s->b1.m==14?
        v90_trellis_qam56_pair(&s->trellis,s->a_re,s->a_im,ar,ai,inv,&a,&b):s->b1.m==8?
        v90_trellis_qam32_pair(&s->trellis,s->a_re,s->a_im,ar,ai,inv,&a,&b):s->b1.m==5?
        v90_trellis_qam20_pair(&s->trellis,s->a_re,s->a_im,ar,ai,inv,&a,&b):s->b1.m==3?
        v90_trellis_qam12_pair(&s->trellis,s->a_re,s->a_im,ar,ai,inv,&a,&b):
        v90_trellis_qam8_pair(&s->trellis,s->a_re,s->a_im,ar,ai,inv,&a,&b);
    ++s->pairs;s->have_a=0;
    /* Provisional survivor decisions train after four pairs at 26.4/28.8/31.2 kbit/s,
       eight pairs at 24 kbit/s;
       final decoded data still retains the full 63-pair lookahead. */
    unsigned early_a,early_b;
    unsigned feedback_age=(s->b1.q>=4 || (s->b1.q==3 && s->b1.m==14))?4:V90_QAM_FEEDBACK_AGE;
    if(s->b1.q>=3 && v90_trellis_peek(&s->trellis,feedback_age,s->b1.q==5?11:s->b1.q>=4?10:s->b1.m==14?9:8,&early_a,&early_b)) {
        for(unsigned j=0;j<2;++j) {
            unsigned long long n=2*(s->pairs-1-feedback_age)+j;
            if(n<s->equalizer.taps)continue;
            unsigned h=(unsigned)n%V90_QAM_FEEDBACK_HISTORY;
            /* Evaluate the current coefficients against the saved input
             * window; preserve live FIR samples and its ring position. */
            V90Equalizer training=s->equalizer;training.position=0;
            memcpy(training.re,s->history_re[h],sizeof(training.re));
            memcpy(training.im,s->history_im[h],sizeof(training.im));
            double r,i;point(j?early_b:early_a,&r,&i);
            if(v90_equalizer_adapt(&training,r,i,s->equalizer.taps==V90_EQ_HALF_TAPS?.2:.1)) {
                memcpy(s->equalizer.cr,training.cr,sizeof(training.cr));
                memcpy(s->equalizer.ci,training.ci,sizeof(training.ci));
            }
        }
    }
    if(ready!=1)return;
    s->labels[s->count++]=(uint16_t)a;s->labels[s->count++]=(uint16_t)b;
    if(s->count==8) {
        uint8_t bits[78];
        int valid=mapping_frame(&s->frames,s->labels,bits,s->b1.m,s->b1.k,s->b1.q);
        s->count=0;++s->output_frames;
        s->output_symbol=s->origin+2*(s->pairs-(V90_TRELLIS_DEPTH-1))-1;
        if(!valid)++s->rejected_frames;
        if(s->receive_bits)s->receive_bits(s->opaque,valid?bits:NULL);
    }
}
/* Fit gain, phase and carrier slope to the known B1 by minimizing waveform
 * squared error. Dividing each observation by its reference point amplifies
 * noise and intersymbol distortion on inner points and biases the slope.
 * Maximizing complex cross-correlation gives the least-squares fit for each
 * frequency, with gain/phase referenced to the first B1 symbol. */
static double b1_fit(V90Qam8Stream *s,double frequency,double *gain,double *phase)
{
    double r=0,j=0,energy=0;
    for(unsigned i=0;i<128;++i) {
        unsigned n=(s->b1.position+i)%128;
        double rr,ri;point(s->b1.labels[i],&rr,&ri);
        double ar=s->b1.re[n]*rr+s->b1.im[n]*ri;
        double ai=s->b1.im[n]*rr-s->b1.re[n]*ri;
        double cs=cos(frequency*i),sn=sin(frequency*i);
        r+=ar*cs+ai*sn;j+=ai*cs-ar*sn;energy+=rr*rr+ri*ri;
    }
    *gain=hypot(r,j)/energy;*phase=atan2(j,r);return r*r+j*j;
}
static double b1_equalized_error(V90Qam8Stream *s,double frequency)
{
    double gain,phase,xr[128],xi[128],tr[128],ti[128];
    b1_fit(s,frequency,&gain,&phase);
    for(unsigned n=0;n<128;++n) {
        unsigned j=(s->b1.position+n)%128;
        double cs=cos(phase+frequency*n),sn=sin(phase+frequency*n);
        xr[n]=(s->b1.re[j]*cs+s->b1.im[j]*sn)/gain;
        xi[n]=(s->b1.im[j]*cs-s->b1.re[j]*sn)/gain;
        point(s->b1.labels[n],&tr[n],&ti[n]);
    }
    V90Equalizer eq;v90_equalizer_init_taps(&eq,V90_EQ_LONG_TAPS);
    v90_equalizer_train(&eq,xr,xi,tr,ti);
    double error=0;
    for(unsigned n=87;n<121;++n) {
        double r=0,i=0;
        for(unsigned k=0;k<15;++k) {
            r+=eq.cr[k]*xr[n+k-7]-eq.ci[k]*xi[n+k-7];
            i+=eq.cr[k]*xi[n+k-7]+eq.ci[k]*xr[n+k-7];
        }
        error+=(r-tr[n])*(r-tr[n])+(i-ti[n])*(i-ti[n]);
    }
    return error;
}
static void b1_carrier(V90Qam8Stream *s,double *gain,double *phase,double *frequency)
{
    /* Search the carrier loop's supported interval, then refine the best
     * coarse cell. The high-correlation B1 gate bounds the initial offset. */
    double best=-1,center=0;
    for(int step=-20;step<=20;++step) {
        double f=step*.001,value=b1_fit(s,f,gain,phase);
        if(value>best){best=value;center=f;}
    }
    double lo=fmax(-.02,center-.001),hi=fmin(.02,center+.001);
    for(unsigned iteration=0;iteration<24;++iteration) {
        double a=lo+(hi-lo)/3,b=hi-(hi-lo)/3;
        double va=b1_fit(s,a,gain,phase),vb=b1_fit(s,b,gain,phase);
        if(va<vb)lo=a;else hi=b;
    }
    *frequency=(lo+hi)/2;
    if((s->b1.q>=4 || (s->b1.q==3 && s->b1.m==14))) {
        double best_error=1e300,center=*frequency;
        for(int step=-12;step<=12;++step) {
            double f=fmax(-.02,fmin(.02,*frequency+step*.0005));
            double error=b1_equalized_error(s,f);
            if(error<best_error){best_error=error;center=f;}
        }
        lo=fmax(-.02,center-.0005);hi=fmin(.02,center+.0005);
        for(unsigned iteration=0;iteration<20;++iteration) {
            double a=lo+(hi-lo)/3,b=hi-(hi-lo)/3;
            if(b1_equalized_error(s,a)>b1_equalized_error(s,b))lo=a;else hi=b;
        }
        *frequency=(lo+hi)/2;
    }
    b1_fit(s,*frequency,gain,phase);
}

int v90_qam8_stream_symbol(V90Qam8Stream *s,double re,double im)
{
    uint64_t index=s->symbols++;
    double input_limit=s->b1.m>=5?1e6:1e100;
    if(!isfinite(re)||!isfinite(im)||fabs(re)>1e100||fabs(im)>1e100 ||
       (s->have_mid && (!isfinite(s->mid_re)||!isfinite(s->mid_im)||fabs(s->mid_re)>1e100||fabs(s->mid_im)>1e100)) ||
       (s->locked && (fabs(re/s->carrier.gain)>=input_limit || fabs(im/s->carrier.gain)>=input_limit ||
        (s->have_mid && (fabs(s->mid_re/s->carrier.gain)>=input_limit || fabs(s->mid_im/s->carrier.gain)>=input_limit))))) {
        s->locked=s->have_a=s->count=0;s->b1.position=s->b1.count=0;return -1;
    }
    if(s->locked){qam8_locked(s,re,im);return 1;}
    double gain,phase,score;
    if(s->have_mid){s->mid_b1_re[s->b1.position]=s->mid_re;s->mid_b1_im[s->b1.position]=s->mid_im;}
    if(!v90_qam8_b1_symbol(&s->b1,re,im,&gain,&phase,&score))return 0;
    double frequency; b1_carrier(s,&gain,&phase,&frequency);
    v90_carrier_init(&s->carrier,phase,gain);s->carrier.frequency=frequency;
    v90_equalizer_init_taps(&s->equalizer,(s->b1.q>=4 || (s->b1.q==3 && s->b1.m==14)) && s->have_mid?V90_EQ_HALF_TAPS:s->b1.q>=3?V90_EQ_LONG_TAPS:V90_EQ_TAPS);
    if(s->b1.m>=5) {
        double xr[128],xi[128],tr[128],ti[128];
        for(unsigned n=0;n<128;++n) {
            unsigned j=(s->b1.position+n)%128;
            double cs=cos(phase+frequency*n),sn=sin(phase+frequency*n);
            xr[n]=(s->b1.re[j]*cs+s->b1.im[j]*sn)/gain;
            xi[n]=(s->b1.im[j]*cs-s->b1.re[j]*sn)/gain;
            point(s->b1.labels[n],&tr[n],&ti[n]);
        }
        if(s->equalizer.taps==V90_EQ_HALF_TAPS) {
            double hr[256],hi[256];
            for(unsigned n=0;n<128;++n) {
                unsigned j=(s->b1.position+n)%128;
                double cs=cos(phase+frequency*(n-.5)),sn=sin(phase+frequency*(n-.5));
                hr[2*n]=(s->mid_b1_re[j]*cs+s->mid_b1_im[j]*sn)/gain;
                hi[2*n]=(s->mid_b1_im[j]*cs-s->mid_b1_re[j]*sn)/gain;
                hr[2*n+1]=xr[n];hi[2*n+1]=xi[n];
            }
            v90_equalizer_train_half(&s->equalizer,hr,hi,tr,ti);
        } else v90_equalizer_train(&s->equalizer,xr,xi,tr,ti);
    }
    v90_trellis_init(&s->trellis);
    if(s->b1.q==5)v90_qam1280_frames_init(&s->frames,0);
    else if(s->b1.q>=4)v90_qam768_frames_init(&s->frames,0);
    else if(s->b1.q==3 && s->b1.m==14)v90_qam448_frames_init(&s->frames,0);
    else if(s->b1.q>=3)v90_qam256_frames_init(&s->frames,0);
    else if(s->b1.q==2)v90_qam160_frames_init(&s->frames,0);
    else if(s->b1.q==1)v90_qam96_frames_init(&s->frames,0);
    else if(s->b1.m==14)v90_qam56_frames_init(&s->frames,0);
    else if(s->b1.m==8)v90_qam32_frames_init(&s->frames,0);
    else if(s->b1.m==5)v90_qam20_frames_init(&s->frames,0);
    else if(s->b1.m==3)v90_qam12_frames_init(&s->frames,0);
    else v90_qam8_frames_init(&s->frames,0);
    s->origin=index+1-V90_QAM8_B1_SYMBOLS;s->pairs=0;s->count=s->have_a=0;
    s->output_frames=s->rejected_frames=0;s->score=score;s->locked=1;
    for(unsigned i=0;i<V90_QAM8_B1_SYMBOLS;++i) {
        unsigned j=(s->b1.position+i)%V90_QAM8_B1_SYMBOLS;
        s->mid_re=s->mid_b1_re[j];s->mid_im=s->mid_b1_im[j];
        qam8_locked(s,s->b1.re[j],s->b1.im[j]);
    }
    return 1;
}

void v90_qam768_frames_init(V90Qam768Frames *s,unsigned previous){v90_shell_init(&s->shell,12,28);s->previous=previous&3;s->q=4;}
int v90_qam768_frame(V90Qam768Frames *s,const uint16_t labels[8],uint8_t bits[72]){return mapping_frame(s,labels,bits,12,28,4);}

void v90_qam1280_frames_init(V90Qam1280Frames*s,unsigned previous){v90_shell_init(&s->shell,10,26);s->previous=previous&3;s->q=5;}
int v90_qam1280_frame(V90Qam1280Frames*s,const uint16_t labels[8],uint8_t bits[78]){return mapping_frame(s,labels,bits,10,26,5);}
