/* V.34 9.3/9.5 inverse for K=6, q=0, b=18 (all high frames). GPL-2.0. */
#include <string.h>
#include "v90qam8.h"
void v90_qam8_frames_init(V90Qam8Frames *s,unsigned previous)
{
    v90_shell_init(&s->shell,2,6);s->previous=previous&3;
}
int v90_qam8_frame(V90Qam8Frames *s,const uint8_t labels[8],uint8_t bits[18])
{
    uint8_t rings[8],decoded[18];uint32_t index;
    for(unsigned i=0;i<8;++i){if(labels[i]>7)return 0;rings[i]=labels[i]>>2;}
    unsigned previous=s->previous;s->previous=labels[6]&3;
    if(!v90_shell_decode(&s->shell,rings,&index))return 0;
    for(unsigned i=0;i<6;++i)decoded[i]=(index>>i)&1;
    for(unsigned pair=0;pair<4;++pair) {
        unsigned a=labels[2*pair]&3,b=labels[2*pair+1]&3;
        unsigned difference=(a+4-previous)&3;
        decoded[6+3*pair]=((b+4-a)&3)>>1;
        decoded[7+3*pair]=difference&1;
        decoded[8+3*pair]=difference>>1;
        previous=a;
    }
    memcpy(bits,decoded,sizeof(decoded));return 1;
}
