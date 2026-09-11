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
