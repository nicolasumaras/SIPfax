#include <math.h>
#include <string.h>
#include "v34info1c.h"
int v34_info1c_parse(const uint8_t *b, unsigned length, unsigned *md_ms)
{
    static const uint8_t prefix[12] = {1,1,1,1,0,1,1,1,0,0,1,0};
    unsigned crc=0xffff, received=0, units=0;
    if (!b || !md_ms || length != V34_INFO1C_BITS) return 0;
    for (unsigned i=0;i<length;i++) if (b[i]>1) return 0;
    if (memcmp(b,prefix,sizeof(prefix))) return 0;
    for (unsigned i=12;i<89;i++) {
        unsigned feedback=(crc&1)^b[i];crc>>=1;
        if (feedback) crc^=0x8408;
    }
    for (unsigned i=0;i<16;i++) received|=(unsigned)b[89+i]<<i;
    if (received!=crc) return 0;
    for (unsigned i=0;i<7;i++) units|=(unsigned)b[18+i]<<i;
    *md_ms=units*35;
    return 1;
}
void v34_info1c_init(V34Info1c *s)
{
    memset(s,0,sizeof(*s));
    for (unsigned i=0;i<16;i++) s->lanes[i].clock=i*500;
}
int v34_info1c_receive(V34Info1c *s,const int16_t *pcm,unsigned length)
{
    for (unsigned n=0;n<length;n++,s->samples++) {
        double phase=2*3.14159265358979323846*1200*(s->samples%20)/8000;
        double re=pcm[n]*cos(phase), im=-pcm[n]*sin(phase);
        for (unsigned j=0;j<16;j++) {
            V34Info1cLane *l=&s->lanes[j];
            l->re+=re;l->im+=im;l->count++;l->clock+=600;
            if (l->clock<8000) continue;
            l->clock-=8000;
            if (l->have_previous) {
                uint8_t bit=l->re*l->previous_re+l->im*l->previous_im<0;
                if (l->used==V34_INFO1C_BITS) {
                    memmove(l->bits,l->bits+1,V34_INFO1C_BITS-1);l->used--;
                }
                l->bits[l->used++]=bit;
                unsigned md;
                if (v34_info1c_parse(l->bits,l->used,&md)) {
                    s->md_ms=md;s->valid=1;
                }
            }
            l->have_previous=1;l->previous_re=l->re;l->previous_im=l->im;
            l->re=l->im=0;l->count=0;
        }
    }
    return s->valid;
}
