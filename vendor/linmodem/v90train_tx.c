/* V.90 digital training signals, clauses8.4.2/4/5. GPL-2.0. */
#include <string.h>
#include "v90train_tx.h"
static int magnitude(int law,int ucode)
{
    /* Positive half of the universal code set, exactly on the G.711 grid. */
    int segment=ucode>>4,part=ucode&15;
    if(!law)return ((part*8+132)<<segment)-132;
    if(segment==0)return part*16+8;
    return (part*16+264)<<(segment-1);
}
void v90_train_tx_init(V90TrainTx *s,int alaw,int uinfo)
{
    memset(s,0,sizeof(*s));s->alaw=alaw;s->uinfo=uinfo;
    memset(s->jd,1,17);
    for(int j=18;j<=33;++j)s->jd[j]=1;
    for(int j=35;j<=40;++j)s->jd[j]=1;
    s->jd[49]=1; /* mandatory lookahead1; four-point upstream training */
    unsigned crc=0xffff;
    for(int j=18;j<=50;++j) {
        if(j==34)continue;
        unsigned feedback=(crc^s->jd[j])&1;crc>>=1;
        if(feedback)crc^=0x8408;
    }
    for(int j=0;j<16;++j)s->jd[52+j]=(crc>>j)&1;
}
static unsigned scramble(V90TrainTx *s,unsigned bit)
{
    unsigned out=((s->scrambler>>22)^bit)&1;
    s->scrambler=(s->scrambler<<1)&0x7fffff;
    if(out)s->scrambler^=1|(1<<5); /* GPC */
    return out;
}
int16_t v90_train_tx_next(V90TrainTx *s)
{
    unsigned n=s->sample++;
    if(s->uinfo<67 || s->uinfo>111)return 0;
    if(n<432) {
        unsigned frame=n%6;
        int level=magnitude(s->alaw,(frame==1 || frame==4)?0:s->uinfo+16);
        int positive=frame<3;
        if(n>=384)positive=!positive;
        return positive?level:-level;
    }
    if(n<2472)s->sign=scramble(s,1);
    else s->sign^=scramble(s,s->jd[(n-2472)%72]);
    int level=magnitude(s->alaw,s->uinfo);
    return s->sign?level:-level;
}
