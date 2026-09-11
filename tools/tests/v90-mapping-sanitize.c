/* Bounds and transactional rejection for initialized mapping profiles. */
#include <assert.h>
#include <stdint.h>
#include <string.h>
#include "v90mapping.h"
int main(void)
{
    V90Mapping s,before;
    for(unsigned baud=3000;baud<=3200;baud+=200)
        for(unsigned rate=4800;rate<=(baud==3000?28800u:31200u);rate+=2400){
            assert(v90_mapping_init(&s,rate,baud));before=s;
            assert(!v90_mapping_init(&s,rate,2999));assert(!memcmp(&s,&before,sizeof(s)));
            for(unsigned f=0;f<7*s.p;++f){
                unsigned n=v90_mapping_frame_bits(&s,f),previous=99;
                uint8_t out[78];uint16_t labels[8]={0};memset(out,0xa5,sizeof(out));
                assert(v90_mapping_decode(&s,f,0,labels,out,n,&previous)==n);
                for(unsigned j=0;j<n;++j)assert(out[j]==0);
                for(unsigned j=n;j<78;++j)assert(out[j]==0xa5);
                previous=99;memset(out,0xa5,sizeof(out));
                assert(!v90_mapping_decode(&s,f,0,labels,out,n-1,&previous));
                assert(previous==99);
                labels[7]=UINT16_MAX;
                assert(!v90_mapping_decode(&s,f,0,labels,out,78,&previous));
                for(unsigned j=0;j<78;++j)assert(out[j]==0xa5);
                assert(previous==99);
            }
            uint16_t training[128];memset(training,0xff,sizeof(training));
            assert(v90_mapping_b1(&s,training,8*s.p)==8*s.p);
            for(unsigned i=0;i<8*s.p;++i)assert(training[i]<(4*s.m<<s.q));
            for(unsigned i=8*s.p;i<128;++i)assert(training[i]==UINT16_MAX);
            memset(training,0xff,sizeof(training));
            assert(!v90_mapping_b1(&s,training,8*s.p-1));
            for(unsigned i=0;i<128;++i)assert(training[i]==UINT16_MAX);
            assert(v90_mapping_inversion(&s,0)==1);
            assert(v90_mapping_inversion(&s,2*s.p)==0);
            assert(v90_mapping_inversion(&s,UINT64_MAX)<=1);
            assert(v90_mapping_frame_bits(&s,UINT64_MAX)>0);
        }
    assert(!v90_mapping_init(NULL,4800,3000));
    assert(!v90_mapping_decode(NULL,0,0,NULL,NULL,0,NULL));
    return 0;
}
