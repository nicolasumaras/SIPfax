#include <math.h>
#include <string.h>
#include "v34sdetect.h"
#define PI 3.14159265358979323846
static double pulse(double t)
{
    const double beta=.15;
    if (fabs(t)<1e-10) return 1+beta*(4/PI-1);
    if (fabs(fabs(4*beta*t)-1)<1e-8)
        return beta/sqrt(2)*((1+2/PI)*sin(PI/(4*beta))+(1-2/PI)*cos(PI/(4*beta)));
    return (sin(PI*t*(1-beta))+4*beta*t*cos(PI*t*(1+beta)))/(PI*t*(1-16*beta*beta*t*t));
}
int v34_s_detect_init(V34SDetect *s,double baud,double carrier)
{
    if (!s || !isfinite(baud) || !isfinite(carrier) || baud<2400 || baud>3430 || carrier<1500 || carrier>2100) return 0;
    memset(s,0,sizeof(*s));s->baud=baud;
    s->length=(unsigned)ceil(136*8000/baud);
    if(s->length>V34_S_DETECT_MAX)return 0;
    double aa=0,ab=0,bb=0;
    for(unsigned n=0;n<s->length;n++) {
        double t=n*baud/8000, re=0,im=0;
        int low=(int)ceil(t-6),high=(int)floor(t+6);
        if(low<0)low=0;
        if(high>143)high=143;
        for(int k=low;k<=high;k++) {
            double p=pulse(t-k),sign=k>=128?-1:1;
            re+=((k&1)?-1:1)*sign*p;im+=sign*p;
        }
        double ph=2*PI*carrier*n/8000;
        s->a[n]=re*cos(ph)-im*sin(ph);s->b[n]=re*sin(ph)+im*cos(ph);
        aa+=s->a[n]*s->a[n];ab+=s->a[n]*s->b[n];bb+=s->b[n]*s->b[n];
    }
    double det=aa*bb-ab*ab;if(det<=0)return 0;
    s->inv00=bb/det;s->inv01=-ab/det;s->inv11=aa/det;return 1;
}
int v34_s_detect_sample(V34SDetect *s,int16_t pcm)
{
    if(!s || !s->length || s->length>V34_S_DETECT_MAX)return 0;
    if(s->found)return 1;
    double old=s->pcm[s->cursor];s->energy+=(double)pcm*pcm-old*old;
    s->pcm[s->cursor]=pcm;s->cursor=(s->cursor+1)%s->length;s->samples++;
    if(s->filled<s->length)s->filled++;
    if(s->filled<s->length || s->energy<s->length*100.0*100.0)return 0;
    double u=0,v=0;
    for(unsigned i=0;i<s->length;i++) {
        double x=s->pcm[(s->cursor+i)%s->length];u+=x*s->a[i];v+=x*s->b[i];
    }
    s->score=(s->inv00*u*u+2*s->inv01*u*v+s->inv11*v*v)/s->energy;
    if(s->score>.86) {
        s->transition_sample=(double)s->samples-s->length+128*8000/s->baud;
        s->found=1;
    }
    return s->found;
}
