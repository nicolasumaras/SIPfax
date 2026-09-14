/* V.90 Ja descriptor validation. GPL-2.0. */
#include <string.h>
#include "v90dil.h"
static unsigned get(const uint8_t *b,unsigned start,unsigned width)
{
    unsigned v=0;
    for(unsigned j=0;j<width;++j)v|=(unsigned)b[start+j]<<j;
    return v;
}
int v90_dil_parse(V90Dil *d,const uint8_t *b,unsigned count,unsigned *consumed)
{
    if(consumed)*consumed=0;
    if(count<52)return 0;
    for(unsigned j=0;j<52;++j)if(b[j]>1)return -1;
    for(unsigned j=0;j<17;++j)if(!b[j])return -1;
    if(b[17]||b[34]||b[51])return -1;
    unsigned n=get(b,18,8),lsp=get(b,35,7)+1,ltp=get(b,43,7)+1;
    unsigned alpha=((lsp+15)/16)*17,beta=alpha+((ltp+15)/16)*17;
    unsigned crc_start=188+beta+((n+1)/2)*17;
    unsigned length=(crc_start+18)&~1u;
    if(count<length)return 0;
    for(unsigned j=0;j<length;++j)if(b[j]>1)return -1;
    for(unsigned j=17;j<crc_start;j+=17)if(b[j])return -1;
    if(b[crc_start+16] || ((crc_start+17)<length && b[crc_start+17]))return -1;
    unsigned crc=0xffff;
    for(unsigned j=18;j<crc_start;++j) {
        if(j%17==0)continue;
        unsigned feedback=(crc^b[j])&1;crc>>=1;
        if(feedback)crc^=0x8408;
    }
    if(crc!=get(b,crc_start,16))return -1;
    if(n==0 && (lsp!=1 || ltp!=1))return -1;
    V90Dil result;
    memset(&result,0,sizeof(result));result.n=n;result.lsp=lsp;result.ltp=ltp;
    for(unsigned j=0;j<lsp;++j)result.sp[j]=b[52+(j/16)*17+j%16];
    for(unsigned j=0;j<ltp;++j)result.tp[j]=b[52+alpha+(j/16)*17+j%16];
    for(unsigned j=0;j<8;++j) {
        result.h[j]=get(b,52+beta+(j/2)*17+(j%2)*8,7);
        result.reference[j]=get(b,120+beta+(j/2)*17+(j%2)*8,7);
    }
    for(unsigned j=0;j<n;++j)result.ucodes[j]=get(b,188+beta+(j/2)*17+(j%2)*8,7);
    *d=result;if(consumed)*consumed=length;
    return 1;
}
