#include <assert.h>
#include <stdlib.h>
#include "v90startup.c"
int main(void)
{
    V90Startup *s=malloc(sizeof(*s));assert(s);
    for(unsigned rate=4800;rate<=31200;rate+=2400)
    for(unsigned mask=0;mask<16;++mask)
    for(unsigned force=0;force<3;++force)
    for(unsigned code=0;code<8;++code) {
        v90_startup_init(s,0);s->upstream_max_rate=rate;s->peer_large_constellations=1;
        s->peer_carriers=mask;s->forced_symbol_rate=force?2800+200*force:0;
        select_upstream_rate(s);s->upstream_rate=code;s->downstream_rate=6;
        unsigned baud=code==3?3000:3200,carriers=(mask>>(code==3?0:2))&3;
        int expected=(code==3 || code==4) && carriers && (!force || s->forced_symbol_rate==baud);
        assert(select_upstream_profile(s)==expected);
        if(expected) {
            assert(s->training.symbol_rate==baud && s->s_detector.symbol_rate==baud);
            assert(s->upstream_high_carrier==!!(carriers&2));
            assert(s->upstream_data_rate==(baud==3000 && rate>28800?28800:rate));
        }
        s->downstream_rate=5;assert(!select_upstream_profile(s));
        unsigned char before[109];memcpy(before,s->info1d,109);
        begin_retrain(s,"sanitizer");assert(!memcmp(before,s->info1d,109));
    }
    free(s);return 0;
}
