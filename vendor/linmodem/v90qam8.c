/* V.34 9.3/9.5 inverse for 7200/9600/12000, q=0, all high frames. GPL-2.0. */
#include <string.h>
#include <math.h>
#include "v90qam8.h"
void v90_qam8_frames_init(V90Qam8Frames *s,unsigned previous)
{
    v90_shell_init(&s->shell,2,6);s->previous=previous&3;s->q=0;
}
static int mapping_frame(V90Qam8Frames *s,const uint8_t labels[8],uint8_t *bits,
                         unsigned m,unsigned k,unsigned q)
{
    uint8_t rings[8],decoded[48];uint32_t index;
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
int v90_qam8_frame(V90Qam8Frames *s,const uint8_t labels[8],uint8_t bits[18])
{
    return mapping_frame(s,labels,bits,2,6,0);
}
void v90_qam12_frames_init(V90Qam12Frames *s,unsigned previous)
{
    v90_shell_init(&s->shell,3,12);s->previous=previous&3;s->q=0;
}
int v90_qam12_frame(V90Qam12Frames *s,const uint8_t labels[8],uint8_t bits[24])
{
    return mapping_frame(s,labels,bits,3,12,0);
}

void v90_qam20_frames_init(V90Qam20Frames *s,unsigned previous)
{
    v90_shell_init(&s->shell,5,18);s->previous=previous&3;s->q=0;
}
int v90_qam20_frame(V90Qam20Frames *s,const uint8_t labels[8],uint8_t bits[30])
{
    return mapping_frame(s,labels,bits,5,18,0);
}

void v90_qam32_frames_init(V90Qam32Frames *s,unsigned previous)
{
    v90_shell_init(&s->shell,8,24);s->previous=previous&3;s->q=0;
}
int v90_qam32_frame(V90Qam32Frames *s,const uint8_t labels[8],uint8_t bits[36])
{
    return mapping_frame(s,labels,bits,8,24,0);
}

void v90_qam56_frames_init(V90Qam56Frames *s,unsigned previous)
{
    v90_shell_init(&s->shell,14,30);s->previous=previous&3;s->q=0;
}
int v90_qam56_frame(V90Qam56Frames *s,const uint8_t labels[8],uint8_t bits[42])
{
    return mapping_frame(s,labels,bits,14,30,0);
}

void v90_qam96_frames_init(V90Qam96Frames *s,unsigned previous)
{
    v90_shell_init(&s->shell,12,28);s->previous=previous&3;s->q=1;
}
int v90_qam96_frame(V90Qam96Frames *s,const uint8_t labels[8],uint8_t bits[48])
{
    return mapping_frame(s,labels,bits,12,28,1);
}

static void point(unsigned label,double *re,double *im)
{
    static const double r[56]={1,1,-1,-1,-3,1,3,-1,1,-3,-1,3,-3,-3,3,3,1,5,-1,-5,5,1,-5,-1,-3,5,3,-5,5,-3,-5,3,5,5,-5,-5,-7,1,7,-1,1,-7,-1,7,-7,-3,7,3,-3,-7,3,7,-7,5,7,-5};
    static const double j[56]={1,-1,-1,1,1,3,-1,-3,-3,-1,3,1,-3,3,3,-3,5,-1,-5,1,1,-5,-1,5,5,3,-5,-3,-3,-5,3,5,5,-5,-5,5,1,7,-1,-7,-7,-1,7,1,-3,7,3,-7,-7,3,7,-3,5,7,-5,-7};
    *re=r[label];*im=j[label];
}
int v90_qam_b1_init_rate(V90Qam8B1 *s,unsigned rate)
{
    /* V.34 10.1.3.1: zero encoders, scrambled ones, last J=7 data frame.
     * 16 mapping frames, 18 through 42 bits / 8 symbols. No auxiliary channel. */
    memset(s,0,sizeof(*s));
    if(rate!=7200 && rate!=9600 && rate!=12000 && rate!=14400 && rate!=16800)return 0;
    s->m=rate==7200?2:rate==9600?3:rate==12000?5:rate==14400?8:14;s->k=rate/400-12;
    unsigned frame_bits=s->k+12;
    V90Shell shell;v90_shell_init(&shell,s->m,s->k);
    uint8_t bits[672];unsigned state=0,previous=0;
    for(unsigned i=0;i<16*frame_bits;++i)
        bits[i]=1^(i>=5?bits[i-5]:0)^(i>=23?bits[i-23]:0);
    for(unsigned f=0;f<16;++f) {
        uint32_t index=0;uint8_t rings[8];
        for(unsigned k=0;k<s->k;++k)index|=(uint32_t)bits[frame_bits*f+k]<<k;
        v90_shell_encode(&shell,index,rings);
        for(unsigned p=0;p<4;++p) {
            unsigned k=frame_bits*f+s->k+3*p,i=8*f+2*p;
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
    if(s->b1.m>=5 && v90_equalizer_symbol(&s->equalizer,*ar,*ai,ar,ai)!=1) {
        c->phase=remainder(c->phase+c->frequency,2*acos(-1.0));return 0;
    }
    double best=1e300,rr=1,ri=1;
    for(unsigned i=0;i<4*s->b1.m;++i) {
        double r,j;point(i,&r,&j);
        double distance=(*ar-r)*(*ar-r)+(*ai-j)*(*ai-j);
        if(distance<best){best=distance;rr=r;ri=j;}
    }
    if(s->b1.m>=14 && best<.25)
        v90_equalizer_adapt(&s->equalizer,rr,ri,.01);
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
    if(!s->have_a){s->a_re=ar;s->a_im=ai;s->have_a=1;return;}
    unsigned a,b;
    /* B1 is the last 64 pairs of J=7. The following data starts at V0[0]. */
    unsigned inv=v90_trellis_inversion((unsigned)((s->pairs+384)%448),0);
    int ready=s->b1.m==14?
        v90_trellis_qam56_pair(&s->trellis,s->a_re,s->a_im,ar,ai,inv,&a,&b):s->b1.m==8?
        v90_trellis_qam32_pair(&s->trellis,s->a_re,s->a_im,ar,ai,inv,&a,&b):s->b1.m==5?
        v90_trellis_qam20_pair(&s->trellis,s->a_re,s->a_im,ar,ai,inv,&a,&b):s->b1.m==3?
        v90_trellis_qam12_pair(&s->trellis,s->a_re,s->a_im,ar,ai,inv,&a,&b):
        v90_trellis_qam8_pair(&s->trellis,s->a_re,s->a_im,ar,ai,inv,&a,&b);
    ++s->pairs;s->have_a=0;
    if(ready!=1)return;
    s->labels[s->count++]=(uint8_t)a;s->labels[s->count++]=(uint8_t)b;
    if(s->count==8) {
        uint8_t bits[42];int valid=s->b1.m==14?
            v90_qam56_frame(&s->frames,s->labels,bits):s->b1.m==8?
            v90_qam32_frame(&s->frames,s->labels,bits):s->b1.m==5?
            v90_qam20_frame(&s->frames,s->labels,bits):s->b1.m==3?
            v90_qam12_frame(&s->frames,s->labels,bits):
            v90_qam8_frame(&s->frames,s->labels,bits);
        s->count=0;++s->output_frames;
        s->output_symbol=s->origin+2*(s->pairs-(V90_TRELLIS_DEPTH-1))-1;
        if(!valid)++s->rejected_frames;
        if(s->receive_bits)s->receive_bits(s->opaque,valid?bits:NULL);
    }
}
/* Estimate carrier slope from the known B1 before replaying it. A single
 * whole-frame phase estimate leaves a transient large enough to misclassify
 * outer points at higher rates. Divide out reference energy so shell weights
 * cannot move the two half-frame time centres. */
static void b1_carrier(V90Qam8Stream *s,double *gain,double *phase,double *frequency)
{
    double re[128],im[128],hr[2]={0,0},hi[2]={0,0};
    for(unsigned i=0;i<128;++i) {
        unsigned j=(s->b1.position+i)%128;
        double rr,ri;point(s->b1.labels[i],&rr,&ri);
        double energy=rr*rr+ri*ri;
        re[i]=(s->b1.re[j]*rr+s->b1.im[j]*ri)/energy;
        im[i]=(s->b1.im[j]*rr-s->b1.re[j]*ri)/energy;
        hr[i/64]+=re[i];hi[i/64]+=im[i];
    }
    *frequency=atan2(hi[1]*hr[0]-hr[1]*hi[0],hr[1]*hr[0]+hi[1]*hi[0])/64;
    double r=0,j=0;
    for(unsigned i=0;i<128;++i) {
        double cs=cos(*frequency*i),sn=sin(*frequency*i);
        r+=re[i]*cs+im[i]*sn;j+=im[i]*cs-re[i]*sn;
    }
    *gain=hypot(r,j)/128;*phase=atan2(j,r);
}
int v90_qam8_stream_symbol(V90Qam8Stream *s,double re,double im)
{
    uint64_t index=s->symbols++;
    double input_limit=s->b1.m>=5?1e6:1e100;
    if(!isfinite(re)||!isfinite(im)||fabs(re)>1e100||fabs(im)>1e100 ||
       (s->locked && (fabs(re/s->carrier.gain)>=input_limit || fabs(im/s->carrier.gain)>=input_limit))) {
        s->locked=s->have_a=s->count=0;s->b1.position=s->b1.count=0;return -1;
    }
    if(s->locked){qam8_locked(s,re,im);return 1;}
    double gain,phase,score;
    if(!v90_qam8_b1_symbol(&s->b1,re,im,&gain,&phase,&score))return 0;
    double frequency; b1_carrier(s,&gain,&phase,&frequency);
    v90_carrier_init(&s->carrier,phase,gain);s->carrier.frequency=frequency;
    v90_equalizer_init(&s->equalizer);
    if(s->b1.m>=5) {
        double xr[128],xi[128],tr[128],ti[128];
        for(unsigned n=0;n<128;++n) {
            unsigned j=(s->b1.position+n)%128;
            double cs=cos(phase+frequency*n),sn=sin(phase+frequency*n);
            xr[n]=(s->b1.re[j]*cs+s->b1.im[j]*sn)/gain;
            xi[n]=(s->b1.im[j]*cs-s->b1.re[j]*sn)/gain;
            point(s->b1.labels[n],&tr[n],&ti[n]);
        }
        v90_equalizer_train(&s->equalizer,xr,xi,tr,ti);
    }
    v90_trellis_init(&s->trellis);
    if(s->b1.m==14)v90_qam56_frames_init(&s->frames,0);
    else if(s->b1.m==8)v90_qam32_frames_init(&s->frames,0);
    else if(s->b1.m==5)v90_qam20_frames_init(&s->frames,0);
    else if(s->b1.m==3)v90_qam12_frames_init(&s->frames,0);
    else v90_qam8_frames_init(&s->frames,0);
    s->origin=index+1-V90_QAM8_B1_SYMBOLS;s->pairs=0;s->count=s->have_a=0;
    s->output_frames=s->rejected_frames=0;s->score=score;s->locked=1;
    for(unsigned i=0;i<V90_QAM8_B1_SYMBOLS;++i) {
        unsigned j=(s->b1.position+i)%V90_QAM8_B1_SYMBOLS;
        qam8_locked(s,s->b1.re[j],s->b1.im[j]);
    }
    return 1;
}
