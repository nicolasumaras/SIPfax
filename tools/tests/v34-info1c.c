#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "v34info1c.h"
int main(int argc,char **argv)
{
    const char *capture="1111011100100000000010100000000100101000010101000110101000001101000101101000011000000000100100100010110100010";
    uint8_t b[V34_INFO1C_BITS];unsigned md=999;
    for (unsigned i=0;i<sizeof(b);i++) b[i]=capture[i]-'0';
    assert(v34_info1c_parse(b,sizeof(b),&md) && md==700);
    for(unsigned i=0;i<sizeof(b);i++) {
        b[i]^=1;md=999;assert(!v34_info1c_parse(b,sizeof(b),&md));assert(md==999);b[i]^=1;
    }
    for(unsigned n=0;n<sizeof(b);n++) assert(!v34_info1c_parse(b,n,&md));
    for (unsigned units=0;units<128;units++) {
        unsigned crc=0xffff;
        for(unsigned i=0;i<7;i++) b[18+i]=(units>>i)&1;
        for(unsigned i=12;i<89;i++) {
            unsigned feedback=(crc&1)^b[i];crc>>=1;if(feedback)crc^=0x8408;
        }
        for(unsigned i=0;i<16;i++) b[89+i]=(crc>>i)&1;
        assert(v34_info1c_parse(b,sizeof(b),&md) && md==units*35);
    }
    V34Info1c s;v34_info1c_init(&s);int16_t pcm[160]={0};
    for(unsigned i=0;i<100;i++) assert(!v34_info1c_receive(&s,pcm,160));
    if(argc==2) {
        const unsigned chunks[]={1,13,160};
        for(unsigned k=0;k<sizeof(chunks)/sizeof(chunks[0]);k++) {
            FILE*f=fopen(argv[1],"rb");assert(f);size_t n;
            v34_info1c_init(&s);
            while((n=fread(pcm,sizeof(*pcm),chunks[k],f))) v34_info1c_receive(&s,pcm,(unsigned)n);
            fclose(f);assert(s.valid && s.md_ms==700);
        }
        printf("Captured INFO1c: CRC valid, MD=%u ms\n",s.md_ms);
    }
    puts("PASS: captured frame, all single-bit corruptions, truncation, silence");
    return 0;
}
