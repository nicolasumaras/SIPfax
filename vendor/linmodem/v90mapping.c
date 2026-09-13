/* V.34 8.2, 9.2, 9.3 and 9.5; V.90 disables the auxiliary channel. GPL-2.0. */
#include <string.h>
#include "v90mapping.h"
int v90_mapping_init(V90Mapping *s,unsigned rate,unsigned symbol_rate)
{
    static const unsigned m3000[]={2,2,4,6,11,9,8,14,12,11,9};
    static const unsigned m3200[]={1,2,3,5,8,14,12,10,8,14,12,10};
    if(!s || rate<4800 || rate%2400 ||
       (symbol_rate!=3000 && symbol_rate!=3200) ||
       rate>(symbol_rate==3000?28800u:31200u))return 0;
    V90Mapping v={0};v.rate=rate;v.symbol_rate=symbol_rate;v.j=7;
    v.p=symbol_rate==3000?15:16;
    unsigned n=rate/25; /* Each data frame lasts exactly 40 ms. */
    v.b=(n+v.p-1)/v.p;v.high_frames=n-(v.b-1)*v.p;
    v.k=v.b>12?v.b-12:0;
    while(v.k>=32){v.k-=8;++v.q;}
    v.m=(symbol_rate==3000?m3000:m3200)[rate/2400-2];
    v.low_carrier=symbol_rate==3000?1800:12800.0/7;
    v.high_carrier=symbol_rate==3000?2000:1920;
    if(!v90_shell_init(&v.shell,v.m,v.k))return 0;
    *s=v;return 1;
}
unsigned v90_mapping_frame_bits(const V90Mapping *s,unsigned long long frame)
{
    if(!s || (s->p!=15 && s->p!=16) || !s->high_frames || s->high_frames>s->p)return 0;
    unsigned i=(unsigned)(frame%s->p);
    unsigned high=((i+1)*s->high_frames/s->p)!=(i*s->high_frames/s->p);
    return s->b-1+high;
}
unsigned v90_mapping_decode(const V90Mapping *s,unsigned long long frame,
    unsigned previous,const uint16_t labels[8],uint8_t *bits,unsigned capacity,
    unsigned *next_previous)
{
    unsigned n=v90_mapping_frame_bits(s,frame);
    if(!n || n>78 || !labels || !bits || !next_previous || previous>3 || capacity<n ||
       s->k>31 || s->q>5 || s->b!=s->k+12+8*s->q ||
       s->shell.m!=s->m || s->shell.k!=s->k)return 0;
    uint8_t rings[8],out[78];uint32_t index;
    for(unsigned i=0;i<8;++i){
        if(labels[i]>=(4*s->m<<s->q))return 0;
        rings[i]=(uint8_t)(labels[i]>>(2+s->q));
    }
    if(!v90_shell_decode(&s->shell,rings,&index))return 0;
    unsigned k=s->k;
    if(n<s->b){if(!k || (index>>(k-1)))return 0;--k;}
    unsigned count=0;
    for(unsigned i=0;i<k;++i)out[count++]=(uint8_t)((index>>i)&1);
    for(unsigned pair=0;pair<4;++pair){
        unsigned a=labels[2*pair]&3,b=labels[2*pair+1]&3;
        unsigned diff=(a+4-previous)&3;
        out[count++]=(uint8_t)(((b+4-a)&3)>>1);
        out[count++]=(uint8_t)(diff&1);out[count++]=(uint8_t)(diff>>1);
        for(unsigned j=0;j<s->q;++j)out[count++]=(uint8_t)((labels[2*pair]>>(2+j))&1);
        for(unsigned j=0;j<s->q;++j)out[count++]=(uint8_t)((labels[2*pair+1]>>(2+j))&1);
        previous=a;
    }
    if(count!=n)return 0;
    memcpy(bits,out,n);*next_previous=previous;return n;
}

unsigned v90_mapping_inversion(const V90Mapping *s,unsigned long long pair)
{
    static const unsigned pattern[14]={0,1,1,1,0,1,1,1,1,1,1,1,1,0};
    if(!s || s->j!=7 || (s->p!=15 && s->p!=16))return 0;
    unsigned period=4*s->p*s->j,half=2*s->p;
    unsigned position=((unsigned)(pair%period)+4*s->p*(s->j-1))%period;
    return position%half==0?pattern[position/half]:0;
}
unsigned v90_mapping_b1(const V90Mapping *s,uint16_t *labels,unsigned capacity)
{
    if(!s || !labels || (s->p!=15 && s->p!=16) || s->j!=7 || capacity<8*s->p ||
       s->k>31 || s->q>5 || s->b>78 || s->b!=s->k+12+8*s->q ||
       s->m!=s->shell.m || s->k!=s->shell.k)return 0;
    uint16_t out[128];uint32_t scrambler=0;unsigned state=0,previous=0;
    for(unsigned f=0;f<s->p;++f){
        uint8_t bits[78],rings[8];unsigned n=v90_mapping_frame_bits(s,f);
        if(!n || n>s->b || n+1<s->b)return 0;
        unsigned k=s->k;
        if(n<s->b){if(!k)return 0;--k;}
        for(unsigned i=0;i<n;++i){
            bits[i]=(uint8_t)(1^((scrambler>>4)&1)^((scrambler>>22)&1));
            scrambler=((scrambler<<1)|bits[i])&0x7fffff;
        }
        uint32_t index=0;
        for(unsigned i=0;i<k;++i)index|=(uint32_t)bits[i]<<i;
        if(!v90_shell_encode(&s->shell,index,rings))return 0;
        for(unsigned pair=0;pair<4;++pair){
            unsigned at=k+(3+2*s->q)*pair,i=8*f+2*pair;
            unsigned a=(previous+bits[at+1]+2*bits[at+2])&3,u=state&1;
            unsigned b=(a+2*bits[at]+(u^v90_mapping_inversion(s,4*f+pair)))&3;
            unsigned qa=0,qb=0;
            for(unsigned j=0;j<s->q;++j){qa|=(unsigned)bits[at+3+j]<<j;qb|=(unsigned)bits[at+3+s->q+j]<<j;}
            out[i]=(uint16_t)(a+4*((rings[2*pair]<<s->q)|qa));
            out[i+1]=(uint16_t)(b+4*((rings[2*pair+1]<<s->q)|qb));
            unsigned y1=((a&1)&((b&1)^1))^(a>>1)^(b>>1),y2=a&1;
            state=(state>>1)^y1^(y2<<1)^((y2^u)<<2)^(u<<3);previous=a;
        }
    }
    memcpy(labels,out,8*s->p*sizeof(*labels));return 8*s->p;
}
