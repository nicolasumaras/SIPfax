#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include "v34sdetect.h"
int main(int argc,char **argv)
{
    V34SDetect s;assert(v34_s_detect_init(&s,24000.0/7,96000.0/49));
    for(int i=0;i<8000;i++)assert(!v34_s_detect_sample(&s,0));
    for(int hz=1200;hz<=2400;hz+=600) {
        assert(v34_s_detect_init(&s,24000.0/7,96000.0/49));
        for(int i=0;i<8000;i++)assert(!v34_s_detect_sample(&s,(int16_t)(2000*sin(6.283185307179586*hz*i/8000))));
    }
    unsigned seed=7;assert(v34_s_detect_init(&s,24000.0/7,96000.0/49));
    for(int i=0;i<8000;i++){seed=1664525*seed+1013904223;assert(!v34_s_detect_sample(&s,(int16_t)((int)(seed>>20)-2048)));}
    if(argc==4 || argc==5) {
        FILE*f=fopen(argv[1],"rb");assert(f);double start=atof(argv[2]),end=atof(argv[3]);
        assert(start>=0&&end>start);assert(!fseek(f,(long)(start*8000)*2,SEEK_SET));
        assert(v34_s_detect_init(&s,24000.0/7,96000.0/49));
        int16_t x;for(long n=0;n<(end-start)*8000&&fread(&x,2,1,f)==1;n++)if(v34_s_detect_sample(&s,x))break;
        fclose(f);assert(s.found);
        if(argc==5)assert(fabs(start+s.transition_sample/8000-atof(argv[4]))<.001);
        printf("Sbar %.6f seconds score %.6f\n",start+s.transition_sample/8000,s.score);
    }
    puts("PASS: silence, three unrelated tones and deterministic noise rejected");return 0;
}
