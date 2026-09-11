/* B1-trained bounded-length complex least-squares equalizer. GPL-2.0. */
#include <complex.h>
#include <math.h>
#include <string.h>
#include "v90equalizer.h"

int v90_equalizer_init_taps(V90Equalizer *s,unsigned taps)
{
    if(taps!=V90_EQ_TAPS && taps!=V90_EQ_LONG_TAPS && taps!=V90_EQ_MAX_TAPS)return 0;
    memset(s,0,sizeof(*s));s->taps=taps;s->cr[(taps-1)/2]=1;return 1;
}
void v90_equalizer_init(V90Equalizer *s)
{
    v90_equalizer_init_taps(s,V90_EQ_TAPS);
}
static int bounded(double x)
{
    return isfinite(x) && fabs(x)<1e6;
}
static int train(V90Equalizer *s,const double *re,const double *im,
                       const double *tr,const double *ti,unsigned stride)
{
    if(!s||!re||!im||!tr||!ti)return 0;
    unsigned taps=s->taps;
    if((stride==1 && taps!=V90_EQ_TAPS && taps!=V90_EQ_LONG_TAPS) ||
       (stride==2 && taps!=V90_EQ_HALF_TAPS))return 0;
    unsigned center=(taps-1)/2,delay=center/stride;
    double complex x[256],y[128],a[V90_EQ_MAX_TAPS][V90_EQ_MAX_TAPS+1]={{0}};
    for(unsigned n=0;n<128*stride;++n) {
        if(!bounded(re[n])||!bounded(im[n]))return 0;
        x[n]=re[n]+I*im[n];
    }
    for(unsigned n=0;n<128;++n) {
        if(!bounded(tr[n])||!bounded(ti[n]))return 0;
        y[n]=tr[n]+I*ti[n];
    }
    for(unsigned n=delay;n<delay+80;++n)for(unsigned j=0;j<taps;++j) {
        double complex v=conj(x[stride*n+stride-1+j-center]);
        for(unsigned k=0;k<taps;++k)a[j][k]+=v*x[stride*n+stride-1+k-center];
        a[j][taps]+=v*y[n];
    }
    double trace=0;
    for(unsigned j=0;j<taps;++j)trace+=creal(a[j][j]);
    if(!(trace>1e-12))return 0;
    /* Regularize toward the identity filter, not an attenuated signal. */
    /* Oversampled inputs are strongly correlated: regularize the half-symbol
     * fit more strongly to avoid amplifying poorly observed directions. */
    double ridge=trace*(stride==2?1e-3:1e-6)/taps;
    for(unsigned j=0;j<taps;++j){a[j][j]+=ridge;if(j==center)a[j][taps]+=ridge;}
    for(unsigned j=0;j<taps;++j) {
        unsigned pivot=j;
        for(unsigned k=j+1;k<taps;++k)if(cabs(a[k][j])>cabs(a[pivot][j]))pivot=k;
        if(cabs(a[pivot][j])<trace*1e-12)return 0;
        for(unsigned k=0;k<taps+1;++k){double complex v=a[j][k];a[j][k]=a[pivot][k];a[pivot][k]=v;}
        double complex divisor=a[j][j];
        for(unsigned k=j;k<taps+1;++k)a[j][k]/=divisor;
        for(unsigned row=0;row<taps;++row)if(row!=j) {
            double complex v=a[row][j];
            for(unsigned k=j;k<taps+1;++k)a[row][k]-=v*a[j][k];
        }
    }
    double norm=0,before=0,after=0;
    for(unsigned j=0;j<taps;++j)norm+=creal(a[j][taps]*conj(a[j][taps]));
    for(unsigned n=delay+80;n<128-delay;++n) {
        double complex z=0;
        for(unsigned j=0;j<taps;++j)z+=a[j][taps]*x[stride*n+stride-1+j-center];
        double complex e=z-y[n],b=x[stride*n+stride-1]-y[n];
        after+=creal(e*conj(e));before+=creal(b*conj(b));
    }
    if(!isfinite(norm)||norm>4||!isfinite(after)||!(after<.8*before))return 0;
    V90Equalizer result;v90_equalizer_init_taps(&result,taps);
    for(unsigned j=0;j<taps;++j){result.cr[j]=creal(a[j][taps]);result.ci[j]=cimag(a[j][taps]);}
    *s=result;return 1;
}
int v90_equalizer_train(V90Equalizer *s,const double *re,const double *im,
                        const double *tr,const double *ti)
{
    return train(s,re,im,tr,ti,1);
}
int v90_equalizer_train_half(V90Equalizer *s,const double *re,const double *im,
                             const double *tr,const double *ti)
{
    return train(s,re,im,tr,ti,2);
}
int v90_equalizer_symbol(V90Equalizer *s,double re,double im,double *orr,double *oi)
{
    unsigned taps=s->taps,delay=(taps-1)/2;
    if(!bounded(re)||!bounded(im))return -1;
    s->re[s->position]=re;s->im[s->position]=im;
    s->position=(s->position+1)%taps;
    if(++s->samples<=delay)return 0;
    double r=0,i=0;
    for(unsigned j=0;j<taps;++j) {
        unsigned k=(s->position+j)%taps;
        r+=s->cr[j]*s->re[k]-s->ci[j]*s->im[k];
        i+=s->cr[j]*s->im[k]+s->ci[j]*s->re[k];
    }
    *orr=r;*oi=i;return 1;
}
int v90_equalizer_adapt(V90Equalizer *s,double tr,double ti,double step)
{
    unsigned taps=s->taps,delay=(taps-1)/2;
    if(s->samples<=delay||!bounded(tr)||!bounded(ti)||!isfinite(step)||step<=0||step>(taps==V90_EQ_HALF_TAPS?.2:.1))return 0;
    double r=0,i=0,energy=0,cr[V90_EQ_MAX_TAPS]={0},ci[V90_EQ_MAX_TAPS]={0},norm=0;
    for(unsigned j=0;j<taps;++j) {
        unsigned k=(s->position+j)%taps;
        r+=s->cr[j]*s->re[k]-s->ci[j]*s->im[k];
        i+=s->cr[j]*s->im[k]+s->ci[j]*s->re[k];
        energy+=s->re[k]*s->re[k]+s->im[k]*s->im[k];
    }
    if(!(energy>1e-12))return 0;
    double er=tr-r,ei=ti-i,mu=step/energy;
    for(unsigned j=0;j<taps;++j) {
        unsigned k=(s->position+j)%taps;
        cr[j]=s->cr[j]+mu*(er*s->re[k]+ei*s->im[k]);
        ci[j]=s->ci[j]+mu*(ei*s->re[k]-er*s->im[k]);
        norm+=cr[j]*cr[j]+ci[j]*ci[j];
    }
    if(!isfinite(norm)||norm>4)return 0;
    memcpy(s->cr,cr,sizeof(cr));memcpy(s->ci,ci,sizeof(ci));return 1;
}
