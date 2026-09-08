/* V.90 clause8.5.2 CP/CPt parser. GPL-2.0. */
#include <string.h>
#include "v90cp.h"
static unsigned get(const uint8_t *b,unsigned start,unsigned width)
{
    unsigned v=0;
    for(unsigned j=0;j<width;++j)v|=(unsigned)b[start+j]<<j;
    return v;
}
int v90_cp_parse(V90Cp *out,const uint8_t *b,unsigned count,unsigned *consumed)
{
    static const unsigned positions[6]={103,107,111,115,120,124};
    if(consumed)*consumed=0;
    if(count<136)return 0;
    for(unsigned j=0;j<136;++j)if(b[j]>1)return -1;
    for(unsigned j=0;j<17;++j)if(!b[j])return -1;
    unsigned highest=0;
    for(unsigned j=0;j<6;++j) {
        unsigned index=get(b,positions[j],4);
        if(index>5)return -1;
        if(index>highest)highest=index;
    }
    unsigned gamma=136*highest,delta=b[128]?2*gamma+136:gamma;
    unsigned length=292+delta,cs=273+delta;
    if(count<length)return 0;
    for(unsigned j=0;j<length;++j)if(b[j]>1)return -1;
    for(unsigned j=17;j<cs;j+=17)if(b[j])return -1;
    for(unsigned j=cs+16;j<length;++j)if(b[j])return -1;
    unsigned crc=0xffff;
    for(unsigned j=18;j<cs;++j) {
        if(j%17==0)continue;
        unsigned feedback=(crc^b[j])&1;crc>>=1;
        if(feedback)crc^=0x8408;
    }
    if(crc!=get(b,cs,16))return -1;
    V90Cp result;memset(&result,0,sizeof(result));
    result.type=b[19];result.drn=get(b,20,5);result.sr=get(b,31,2);
    result.ack=b[33];result.alaw=b[35];result.upstream_mask=get(b,36,13);
    result.lookahead=get(b,49,2);result.gain=get(b,52,16);
    if(result.drn>22)return -1;
    for(unsigned j=0;j<4;++j) {
        unsigned v=get(b,(unsigned[]){69,77,86,94}[j],8);
        result.filter[j]=v<128?(int)v:(int)v-256;
    }
    result.count=highest+1;result.codec_masks=b[128];
    for(unsigned j=0;j<6;++j)result.indices[j]=get(b,positions[j],4);
    for(unsigned side=0;side<=result.codec_masks;++side)
        for(unsigned m=0;m<result.count;++m)
            for(unsigned u=0;u<128;++u)
                result.mask[side][m][u]=b[137+(side*result.count+m)*136+(u/16)*17+u%16];
    if(!result.codec_masks)memcpy(result.mask[1],result.mask[0],sizeof(result.mask[0]));
    if(result.drn)for(unsigned j=0;j<6;++j) {
        unsigned size=0;
        for(unsigned u=0;u<128;++u)size+=result.mask[0][result.indices[j]][u];
        if(!size)return -1;
    }
    *out=result;if(consumed)*consumed=length;return 1;
}
