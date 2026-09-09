/* RFC1661 section5.8: observe pppd-owned echo traffic without generating it.
 * Only a previously responsive peer can arm recovery. GPL-2.0. */
#include <string.h>
#include "v90echo.h"
static unsigned fcs(unsigned crc,unsigned b)
{
    crc^=b;
    for(unsigned k=0;k<8;++k)crc=(crc>>1)^((crc&1)?0x8408:0);
    return crc;
}
void v90_echo_init(V90Echo *s){memset(s,0,sizeof(*s));s->crc=0xffff;}
void v90_echo_pause(V90Echo *s)
{
    /* Preserve learned peer support and the one-shot recovery latch. */
    s->count=s->length=0;s->started=s->escape=s->overflow=0;s->crc=0xffff;
    memset(s->pending,0,sizeof(s->pending));
}
static void frame(V90Echo *s,const uint8_t *p,unsigned n,int tx,int64_t now)
{
    if(n>=2 && p[0]==0xff && p[1]==3){p+=2;n-=2;}
    if(n<12 || p[0]!=0xc0 || p[1]!=0x21)return;
    unsigned size=(p[4]<<8)|p[5];
    if(size<8 || size>n-4)return; /* LCP header + magic, exclude protocol/FCS. */
    unsigned id=p[3];uint32_t magic=((uint32_t)p[6]<<24)|((uint32_t)p[7]<<16)|(p[8]<<8)|p[9];
    if(tx && p[2]==9) {
        s->pending[id]=1;s->sent[id]=now;s->magic[id]=magic;
        /* Retransmissions may reuse an identifier. Count separated requests,
           not arbitrary repeated flags or an immediate burst. */
        if(!s->count){s->first=s->last=now;s->count=1;}
        /* Once two requests establish loss, subsequent requests must not
           keep moving the deadline (including a shorter echo interval). */
        else if(s->count<2 && now-s->last>=80000){s->last=now;++s->count;}
    } else if(!tx && p[2]==10 && s->pending[id] && now>=s->sent[id] &&
              now-s->sent[id]<=720000 && (!s->magic[id] || magic!=s->magic[id])) {
        s->armed=1;s->fired=0;s->count=0;
        memset(s->pending,0,sizeof(s->pending));
    }
}
void v90_echo_tx(V90Echo *s,unsigned b,int64_t now)
{
    b&=255;
    if(b==0x7e) {
        if(s->started && !s->escape && !s->overflow && s->length>=4 && s->crc==0xf0b8)
            frame(s,s->prefix,s->length,1,now);
        s->started=1;s->length=0;s->escape=s->overflow=0;s->crc=0xffff;return;
    }
    if(!s->started)return;
    if(b==0x7d && !s->escape){s->escape=1;return;}
    if(s->escape){b^=0x20;s->escape=0;}
    if(s->length>=4096){s->overflow=1;return;}
    if(s->length<sizeof(s->prefix))s->prefix[s->length]=b;
    ++s->length;s->crc=fcs(s->crc,b);
}
void v90_echo_rx(V90Echo *s,const uint8_t *p,unsigned n,int64_t now)
{
    if(n<4 || n>4096)return;
    unsigned crc=0xffff;for(unsigned i=0;i<n;++i)crc=fcs(crc,p[i]);
    if(crc==0xf0b8)frame(s,p,n,0,now);
}
int v90_echo_due(V90Echo *s,int64_t now)
{
    /* With deployment's30s echo interval, trigger40s after the first missing
       reply, leaving time for a full retrain before pppd's4-echo deadline. */
    if(!s->armed || s->fired || s->count<2 || now-s->first<320000 || now-s->last<80000)return 0;
    s->fired=1;return 1;
}
