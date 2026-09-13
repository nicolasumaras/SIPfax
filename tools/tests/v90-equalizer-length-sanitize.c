/* Exact-length B1 allocations catch reading the unused 128-symbol tail. */
#include <assert.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "v90equalizer.h"
int main(void)
{
    for(unsigned n=120;n<=128;n+=8)for(unsigned mode=0;mode<3;++mode){
        unsigned taps=mode==0?7:mode==1?15:29,stride=mode==2?2:1;
        double *r=malloc(n*stride*sizeof(double)),*i=malloc(n*stride*sizeof(double));
        double *tr=malloc(n*sizeof(double)),*ti=malloc(n*sizeof(double));
        assert(r&&i&&tr&&ti);
        for(unsigned j=0;j<n;++j){tr[j]=(double)((j*37)%17)-8;ti[j]=(double)((j*13)%19)-9;}
        for(unsigned j=0;j<n*stride;++j){r[j]=tr[j/stride];i[j]=ti[j/stride];}
        V90Equalizer s,before;assert(v90_equalizer_init_taps(&s,taps));
        (void)v90_equalizer_train_symbols(&s,r,i,tr,ti,n,stride);
        before=s;
        const unsigned bad[]={0,1,119,121,127,129,~0u};double short_input=1;
        for(unsigned j=0;j<sizeof(bad)/sizeof(*bad);++j){
            assert(!v90_equalizer_train_symbols(&s,&short_input,&short_input,&short_input,&short_input,bad[j],stride));
            assert(!memcmp(&s,&before,sizeof(s)));
        }
        for(unsigned j=0;j<4;++j){
            unsigned bad_stride=j==3?~0u:j;
            if(bad_stride==stride)continue;
            assert(!v90_equalizer_train_symbols(&s,&short_input,&short_input,&short_input,&short_input,n,bad_stride));
            assert(!memcmp(&s,&before,sizeof(s)));
        }
        r[n*stride-1]=NAN;
        assert(!v90_equalizer_train_symbols(&s,r,i,tr,ti,n,stride));
        assert(!memcmp(&s,&before,sizeof(s)));r[n*stride-1]=0;ti[n-1]=INFINITY;
        assert(!v90_equalizer_train_symbols(&s,r,i,tr,ti,n,stride));
        assert(!memcmp(&s,&before,sizeof(s)));
        free(r);free(i);free(tr);free(ti);
    }
    return 0;
}
