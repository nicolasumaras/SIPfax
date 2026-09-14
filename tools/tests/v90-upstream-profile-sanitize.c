/* Profile validation and PCM/front-end bounds; waveform correctness is separate. */
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include "v90upstream.h"
int main(void)
{
    V90Upstream *s=malloc(sizeof(*s)),*before=malloc(sizeof(*before));assert(s&&before);
    for(unsigned baud=3000;baud<=3200;baud+=200)
      for(unsigned rate=4800;rate<=(baud==3000?28800u:31200u);rate+=2400)
        for(unsigned high=0;high<2;++high){
            assert(v90_upstream_init_profile(s,rate,baud,high));
            assert(s->rate==rate && s->symbol_rate==baud && s->symbol_period==32000.0/baud);
            assert(s->carrier==(baud==3000?(high?2000:1800):(high?1920:12800.0/7)));
            for(unsigned n=0;n<600;++n)v90_upstream_receive(s,0);
            assert(s->samples==600 && s->frames==0 && !s->b1_seen);
            *before=*s;
            assert(!v90_upstream_init_profile(s,rate,2999,high));
            assert(!memcmp(s,before,sizeof(*s)));
            assert(!v90_upstream_init_profile(s,rate,baud,2));
            assert(!memcmp(s,before,sizeof(*s)));
            assert(!v90_upstream_init_profile(s,rate+1,baud,high));
            assert(!memcmp(s,before,sizeof(*s)));
        }
    assert(!v90_upstream_init_profile(NULL,4800,3000,1));
    assert(!v90_upstream_init_profile(s,31200,3000,1));
    free(s);free(before);return 0;
}
