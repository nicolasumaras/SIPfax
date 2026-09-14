/* V.34 9.4 hierarchical shell order, including rejection of unused shells.
 * Integer-only; 64-bit counts also cover expanded M=18 constellations.
 * GPL-2.0. */
#include <string.h>
#include "v90shell.h"

int v90_shell_init(V90Shell *s,unsigned m,unsigned k)
{
    memset(s,0,sizeof(*s));
    if(m<1 || m>V90_SHELL_MAX_M || k>31)return 0;
    for(unsigned i=0;i<m;++i)s->count[0][i]=1;
    for(unsigned level=1;level<4;++level) {
        unsigned half=(1u<<(level-1))*(m-1);
        for(unsigned a=0;a<=half;++a)for(unsigned b=0;b<=half;++b)
            s->count[level][a+b]+=s->count[level-1][a]*s->count[level-1][b];
    }
    unsigned maximum=8*(m-1);
    for(unsigned sum=0;sum<=maximum;++sum)
        s->prefix[sum+1]=s->prefix[sum]+s->count[3][sum];
    if((UINT64_C(1)<<k)>s->prefix[maximum+1])return 0;
    s->m=m;s->k=k;return 1;
}

static void unrank(const V90Shell *s,unsigned level,unsigned sum,
                   uint64_t rank,uint8_t *rings)
{
    if(!level){rings[0]=(uint8_t)sum;return;}
    unsigned left=0;
    for(;left<=sum;++left) {
        uint64_t block=s->count[level-1][left]*s->count[level-1][sum-left];
        if(rank<block)break;
        rank-=block;
    }
    uint64_t radix=s->count[level-1][left];
    unrank(s,level-1,left,rank%radix,rings);
    unrank(s,level-1,sum-left,rank/radix,rings+(1u<<(level-1)));
}

int v90_shell_encode(const V90Shell *s,uint32_t index,uint8_t rings[8])
{
    if(!s->m || (uint64_t)index>=(UINT64_C(1)<<s->k))return 0;
    unsigned sum=0;
    while(s->prefix[sum+1]<=index)++sum;
    unrank(s,3,sum,index-s->prefix[sum],rings);
    return 1;
}

static uint64_t rank_rings(const V90Shell *s,unsigned level,
                           const uint8_t *rings,unsigned *sum)
{
    if(!level){*sum=rings[0];return 0;}
    unsigned a,b;
    uint64_t left=rank_rings(s,level-1,rings,&a);
    uint64_t right=rank_rings(s,level-1,rings+(1u<<(level-1)),&b);
    *sum=a+b;
    uint64_t rank=right*s->count[level-1][a]+left;
    for(unsigned i=0;i<a;++i)
        rank+=s->count[level-1][i]*s->count[level-1][a+b-i];
    return rank;
}

int v90_shell_decode(const V90Shell *s,const uint8_t rings[8],uint32_t *index)
{
    if(!s->m)return 0;
    for(unsigned i=0;i<8;++i)if(rings[i]>=s->m)return 0;
    unsigned sum;
    uint64_t rank=rank_rings(s,3,rings,&sum);
    rank+=s->prefix[sum];
    if(rank>=(UINT64_C(1)<<s->k))return 0;
    *index=(uint32_t)rank;return 1;
}
