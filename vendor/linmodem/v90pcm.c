/* V.90 clause5.4 PCM encoder with exhaustive, bounded spectral lookahead.
 * GPL-2.0. Full frames are prefetched so the first output is the first mapping
 * frame, independent of the shaping lookahead. */
#include <string.h>
#include <float.h>
#include "v90pcm.h"
int v90_pcm_level(int alaw,unsigned u)
{
    unsigned seg=u>>4,p=u&15;
    if(!alaw)return ((p*8+132)<<seg)-132;
    return seg?((p*16+264)<<(seg-1)):p*16+8;
}
int v90_pcm_init(V90Pcm *s,const V90Cp *cp,int (*get_bit)(void *),void *opaque)
{
    V90Pcm result;memset(&result,0,sizeof(result));
    if(cp->sr>3 || cp->lookahead>3 || !cp->drn)return -1;
    result.sr=cp->sr;result.s=6-cp->sr;result.k=cp->drn+(cp->type?20:8)-result.s;
    if(result.k<(cp->type?15u:6u) || result.k>(cp->type?39u:24u))return -1;
    result.width=cp->sr?6/cp->sr:6;result.depth=cp->lookahead;result.alaw=cp->alaw;
    uint64_t product=1;
    for(unsigned i=0;i<6;++i) {
        if(cp->indices[i]>=6)return -1;
        for(int u=127;u>=0;--u)if(cp->mask[0][cp->indices[i]][u])result.map[i][result.m[i]++]=u;
        if(!result.m[i])return -1;
        product*=result.m[i];
    }
    if(product<((uint64_t)1<<result.k))return -1;
    result.a1=cp->filter[0]/64.0;result.a2=cp->filter[1]/64.0;
    result.b1=cp->filter[2]/64.0;result.b2=cp->filter[3]/64.0;
    result.get_bit=get_bit;result.opaque=opaque;*s=result;return 0;
}
static unsigned scramble(V90Pcm *s)
{
    unsigned bit=s->get_bit?(unsigned)s->get_bit(s->opaque)&1:1;
    unsigned out=((s->scrambler>>22)^bit)&1;
    s->scrambler=(s->scrambler<<1)&0x7fffff;
    if(out)s->scrambler^=1|(1<<5);
    return out;
}
static void prepare(V90Pcm *s)
{
    unsigned bits[45];
    for(unsigned j=0;j<s->s+s->k;++j)bits[j]=scramble(s);
    uint64_t value=0;
    for(unsigned j=0;j<s->k;++j)value|=(uint64_t)bits[s->s+j]<<j;
    int mag[6];
    for(unsigned j=0;j<6;++j) {
        unsigned label=value%s->m[j];value/=s->m[j];
        mag[j]=v90_pcm_level(s->alaw,s->map[j][label]);
    }
    if(!s->sr) {
        V90ShapeFrame *f=&s->queue[s->queued++];f->pp=0;
        for(unsigned j=0;j<6;++j) {s->last_sign^=bits[j];f->pp|=s->last_sign<<j;f->magnitude[j]=mag[j];}
        return;
    }
    unsigned input=0;
    for(unsigned j=0;j<s->sr;++j) {
        V90ShapeFrame *f=&s->queue[s->queued++];f->pp=0;
        for(unsigned k=0;k<s->width;++k) {
            unsigned b=k?bits[input++]:0;
            if(k&1){s->odd^=b;b=s->odd;}
            f->pp|=b<<k;f->magnitude[k]=mag[j*s->width+k];
        }
    }
}
void v90_pcm_frame(V90Pcm *s,int16_t out[6])
{
    static const unsigned inversion[4]={0,0x55,0xff,0xaa};
    unsigned frames=s->sr?s->sr:1;
    for(unsigned frame=0;frame<frames;++frame) {
        while(s->queued<=(s->sr?s->depth:0))prepare(s);
        unsigned selected=s->queue[0].pp;
        if(s->sr) {
            double best=DBL_MAX;
            unsigned best_q=0;double best_x=0,best_y=0,best_v=0;
            /* Enumerate every allowed sequence of next states. */
            for(unsigned path=0;path<(1u<<(s->depth+1));++path) {
                unsigned q=s->q,t=s->t,first_t=0,first_q=0;
                double x=s->x,y=s->y,v=s->v,cost=0,first_x=0,first_y=0,first_v=0;
                for(unsigned depth=0;depth<=s->depth;++depth) {
                    unsigned next=(path>>depth)&1;
                    t^=s->queue[depth].pp^inversion[(next<<1)|q];
                    t&=(1u<<s->width)-1;q=next;
                    for(unsigned k=0;k<s->width;++k) {
                        double xn=((t>>k)&1)?s->queue[depth].magnitude[k]:-s->queue[depth].magnitude[k];
                        double yn=xn-s->b1*x+s->a1*y;
                        double vn=yn-s->b2*y+s->a2*v;
                        x=xn;y=yn;v=vn;cost+=vn*vn;
                    }
                    if(!depth){first_t=t;first_q=q;first_x=x;first_y=y;first_v=v;}
                }
                if(cost<best){best=cost;selected=first_t;best_q=first_q;best_x=first_x;best_y=first_y;best_v=first_v;}
            }
            s->q=best_q;s->t=selected;s->x=best_x;s->y=best_y;s->v=best_v;
        }
        for(unsigned k=0;k<s->width;++k)out[frame*s->width+k]=((selected>>k)&1)?s->queue[0].magnitude[k]:-s->queue[0].magnitude[k];
        --s->queued;memmove(s->queue,s->queue+1,s->queued*sizeof(s->queue[0]));
    }
}
