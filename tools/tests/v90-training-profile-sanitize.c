#include <assert.h>
#include <string.h>
#include "v90training.h"
int main(void)
{
    V90Training s,before;int16_t pcm[137]={0};
    v90_training_init(&s);assert(v90_training_init_profile(&before,3200,1));
    assert(!memcmp(&s,&before,sizeof(s)));
    for(unsigned baud=3000;baud<=3200;baud+=200)for(unsigned high=0;high<2;++high){
        assert(v90_training_init_profile(&s,baud,high));s.cp_mode=1;
        assert(s.symbol_period==32000.0/baud);
        assert(s.carrier==(baud==3000?(high?2000:1800):(high?1920:12800.0/7)));
        for(unsigned n=0;n<40;++n)assert(!v90_training_receive(&s,pcm,137));
        assert(s.samples==5480 && !s.e_seen);
        before=s;
        assert(!v90_training_init_profile(&s,2999,high));assert(!memcmp(&s,&before,sizeof(s)));
        assert(!v90_training_init_profile(&s,baud,2));assert(!memcmp(&s,&before,sizeof(s)));
    }
    assert(!v90_training_init_profile(NULL,3000,1));
    return 0;
}
