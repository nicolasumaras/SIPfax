/* B1-trained seven-tap complex least-squares equalizer. GPL-2.0. */
#include <complex.h>
#include <math.h>
#include <string.h>
#include "v90equalizer.h"

void v90_equalizer_init(V90Equalizer *s)
{
    memset(s,0,sizeof(*s));s->cr[V90_EQ_DELAY]=1;
}
static int bounded(double x)
{
    return isfinite(x) && fabs(x)<1e6;
}
int v90_equalizer_train(V90Equalizer *s,const double *re,const double *im,
                       const double *tr,const double *ti)
{
    double complex x[128],y[128],a[7][8]={{0}};
    for(unsigned n=0;n<128;++n) {
        if(!bounded(re[n])||!bounded(im[n])||!bounded(tr[n])||!bounded(ti[n]))return 0;
        x[n]=re[n]+I*im[n];y[n]=tr[n]+I*ti[n];
    }
    for(unsigned n=3;n<83;++n)for(unsigned j=0;j<7;++j) {
        double complex v=conj(x[n+j-3]);
        for(unsigned k=0;k<7;++k)a[j][k]+=v*x[n+k-3];
        a[j][7]+=v*y[n];
    }
    double trace=0;
    for(unsigned j=0;j<7;++j)trace+=creal(a[j][j]);
    if(!(trace>1e-12))return 0;
    /* Regularize toward the identity filter, not an attenuated signal. */
    double ridge=trace*1e-6/7;
    for(unsigned j=0;j<7;++j){a[j][j]+=ridge;if(j==3)a[j][7]+=ridge;}
    for(unsigned j=0;j<7;++j) {
        unsigned pivot=j;
        for(unsigned k=j+1;k<7;++k)if(cabs(a[k][j])>cabs(a[pivot][j]))pivot=k;
        if(cabs(a[pivot][j])<trace*1e-12)return 0;
        for(unsigned k=0;k<8;++k){double complex v=a[j][k];a[j][k]=a[pivot][k];a[pivot][k]=v;}
        double complex divisor=a[j][j];
        for(unsigned k=j;k<8;++k)a[j][k]/=divisor;
        for(unsigned row=0;row<7;++row)if(row!=j) {
            double complex v=a[row][j];
            for(unsigned k=j;k<8;++k)a[row][k]-=v*a[j][k];
        }
    }
    double norm=0,before=0,after=0;
    for(unsigned j=0;j<7;++j)norm+=creal(a[j][7]*conj(a[j][7]));
    for(unsigned n=83;n<125;++n) {
        double complex z=0;
        for(unsigned j=0;j<7;++j)z+=a[j][7]*x[n+j-3];
        double complex e=z-y[n],b=x[n]-y[n];
        after+=creal(e*conj(e));before+=creal(b*conj(b));
    }
    if(!isfinite(norm)||norm>4||!isfinite(after)||!(after<.8*before))return 0;
    V90Equalizer result;v90_equalizer_init(&result);
    for(unsigned j=0;j<7;++j){result.cr[j]=creal(a[j][7]);result.ci[j]=cimag(a[j][7]);}
    *s=result;return 1;
}
int v90_equalizer_symbol(V90Equalizer *s,double re,double im,double *orr,double *oi)
{
    if(!bounded(re)||!bounded(im))return -1;
    s->re[s->position]=re;s->im[s->position]=im;
    s->position=(s->position+1)%7;
    if(++s->samples<=3)return 0;
    double r=0,i=0;
    for(unsigned j=0;j<7;++j) {
        unsigned k=(s->position+j)%7;
        r+=s->cr[j]*s->re[k]-s->ci[j]*s->im[k];
        i+=s->cr[j]*s->im[k]+s->ci[j]*s->re[k];
    }
    *orr=r;*oi=i;return 1;
}
int v90_equalizer_adapt(V90Equalizer *s,double tr,double ti,double step)
{
    if(s->samples<=3||!bounded(tr)||!bounded(ti)||!isfinite(step)||step<=0||step>.1)return 0;
    double r=0,i=0,energy=0,cr[7],ci[7],norm=0;
    for(unsigned j=0;j<7;++j) {
        unsigned k=(s->position+j)%7;
        r+=s->cr[j]*s->re[k]-s->ci[j]*s->im[k];
        i+=s->cr[j]*s->im[k]+s->ci[j]*s->re[k];
        energy+=s->re[k]*s->re[k]+s->im[k]*s->im[k];
    }
    if(!(energy>1e-12))return 0;
    double er=tr-r,ei=ti-i,mu=step/energy;
    for(unsigned j=0;j<7;++j) {
        unsigned k=(s->position+j)%7;
        cr[j]=s->cr[j]+mu*(er*s->re[k]+ei*s->im[k]);
        ci[j]=s->ci[j]+mu*(ei*s->re[k]-er*s->im[k]);
        norm+=cr[j]*cr[j]+ci[j]*ci[j];
    }
    if(!isfinite(norm)||norm>4)return 0;
    memcpy(s->cr,cr,sizeof(cr));memcpy(s->ci,ci,sizeof(ci));return 1;
}
