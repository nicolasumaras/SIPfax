/* Profile retention and callback lifetime across the delayed-E reset. */
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include "v90phase4.h"
static void delivered(void*p,const uint8_t*b,unsigned n){(void)p;(void)b;(void)n;}
static int ones(void*p){(void)p;return 1;}
int main(void)
{
    V90Phase4 *s=malloc(sizeof(*s)),*before=malloc(sizeof(*before));assert(s&&before);
    for(unsigned baud=3000;baud<=3200;baud+=200)
      for(unsigned rate=4800;rate<=(baud==3000?28800u:31200u);rate+=2400)
        for(unsigned high=0;high<2;++high){
            assert(v90_phase4_init_profile(s,0,78,rate,baud,high));
            assert(s->rx.symbol_rate==baud && s->upstream.symbol_rate==baud);
            assert(s->upstream.high_carrier==high && s->rate_detector.symbol_rate==baud);
            assert(s->rx.carrier==s->upstream.carrier && s->rate_detector.carrier==s->upstream.carrier);
            s->cpt.drn=9;s->cpt.sr=1;s->cpt.lookahead=1;s->cpt.count=1;s->cpt.filter[0]=63;
            const unsigned u[]={53,78,88,96};
            for(unsigned i=0;i<4;++i)s->cpt.mask[0][0][u[i]]=1;
            assert(!v90_pcm_init(&s->encoder,&s->cpt,ones,s));
            s->stage=2;s->mp_length=102;
            s->upstream.receive_frame=delivered;s->upstream.opaque=before;
            s->upstream_history_count=s->upstream_history_position=3;
            s->rx.e_seen=1;(void)v90_phase4_next(s,0);
            assert(s->rx_e_logged && s->upstream.require_b1 && s->upstream.samples==4);
            assert(s->upstream.rate==rate && s->upstream.symbol_rate==baud && s->upstream.high_carrier==high);
            assert(s->upstream.receive_frame==delivered && s->upstream.opaque==before);
            unsigned window=s->rate_detector.window,block=s->rate_detector.block;
            s->rate_detector.samples=83;s->rate_detector.latched=1;
            v90_s_detect_reset(&s->rate_detector);
            assert(s->rate_detector.symbol_rate==baud && s->rate_detector.carrier==s->upstream.carrier);
            assert(s->rate_detector.window==window && s->rate_detector.block==block);
            assert(!s->rate_detector.samples && !s->rate_detector.latched);
            *before=*s;
            assert(!v90_phase4_init_profile(s,0,78,rate,2999,high));assert(!memcmp(s,before,sizeof(*s)));
            assert(!v90_phase4_init_profile(s,0,78,rate,baud,2));assert(!memcmp(s,before,sizeof(*s)));
            assert(!v90_s_detect_init_profile(&s->rate_detector,2999,high));assert(!memcmp(s,before,sizeof(*s)));
        }
    assert(!v90_phase4_init_profile(NULL,0,78,4800,3000,1));
    free(s);free(before);return 0;
}
