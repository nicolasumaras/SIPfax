/* Select one ordered upstream stream only after a CRC-valid V.42 XID. GPL-2.0. */
#include <string.h>
#include "v90lapmselect.h"

static void reset_candidate(V90LapmCandidate *c)
{
    memset(&c->detection,0,sizeof(c->detection));
    c->history_position=c->history_count=c->buffered_count=c->active=0;
    c->odp_pending=c->odp_reported=c->trailing_marks=0;
    hdlc_rx_restart(&c->hdlc);
}

static void selected_frame(void *opaque,const uint8_t *frame,int len,int ok)
{
    V90LapmCandidate *c=opaque;
    V90LapmSelect *s=c->owner;
    /* XID is always CRC-16 during parameter negotiation (V.42 7.6.2). */
    if(!ok || len<3 || (frame[1]&0xec)!=0xac || frame[2]!=0x82 ||
       !c->active || s->selected>=0)return;
    s->selected=(int)c->id;
    s->selections++;
    if(s->selection)s->selection(s->opaque,c->id);
    if(s->output)for(unsigned i=0;i<c->buffered_count;i++)
        s->output(s->opaque,c->buffered[i]);
}

void v90_lapm_select_init(V90LapmSelect *s,void *opaque,
                          void (*odp)(void *,unsigned,const uint8_t *,unsigned),
                          void (*output)(void *,int),
                          void (*selection)(void *,unsigned))
{
    memset(s,0,sizeof(*s));s->selected=-1;s->opaque=opaque;
    s->odp=odp;s->output=output;s->selection=selection;
    for(unsigned i=0;i<V90_UP_CANDIDATES;i++){
        V90LapmCandidate *c=&s->candidate[i];c->owner=s;c->id=i;
        hdlc_rx_init(&c->hdlc,false,false,1,selected_frame,c);
    }
}

void v90_lapm_select_reset(V90LapmSelect *s)
{
    if(s->selected>=0 && s->output)s->output(s->opaque,-1);
    s->selected=-1;
    for(unsigned i=0;i<V90_UP_CANDIDATES;i++)reset_candidate(&s->candidate[i]);
}

void v90_lapm_select_bit(void *opaque,unsigned id,int bit,long source_sample)
{
    V90LapmSelect *s=opaque;(void)source_sample;
    if(id>=V90_UP_CANDIDATES)return;
    V90LapmCandidate *c=&s->candidate[id];
    if(bit<0){
        if(s->selected==(int)id){s->invalidations++;v90_lapm_select_reset(s);}
        else reset_candidate(c);
        return;
    }
    bit&=1;
    if(s->selected>=0){
        if(s->selected==(int)id && s->output)s->output(s->opaque,bit);
        return;
    }
    c->history[c->history_position]=bit;
    c->history_position=(c->history_position+1)%V90_LAPM_HISTORY_BITS;
    if(c->history_count<V90_LAPM_HISTORY_BITS)c->history_count++;
    if(!c->active){
        if(!v42_detect_bit(&c->detection,(unsigned)bit))return;
        c->active=1;c->odp_pending=1;s->detections++;
        unsigned start=(c->history_position+V90_LAPM_HISTORY_BITS-c->history_count)%V90_LAPM_HISTORY_BITS;
        for(unsigned i=0;i<c->history_count;i++)
            c->buffered[c->buffered_count++]=c->history[(start+i)%V90_LAPM_HISTORY_BITS];
        return;
    }
    if(c->buffered_count>=V90_LAPM_BUFFER_BITS){
        s->overflows++;reset_candidate(c);return;
    }
    c->buffered[c->buffered_count++]=(uint8_t)bit;
    if(c->odp_pending){
        if(bit){if(c->trailing_marks<17)c->trailing_marks++;}
        else if(c->trailing_marks>=8 && c->trailing_marks<=16){
                c->odp_pending=0;c->odp_reported=1;
                if(s->odp)s->odp(s->opaque,id,c->buffered,c->buffered_count);
            }else{reset_candidate(c);return;}
    }
    hdlc_rx_put_bit(&c->hdlc,bit);
}
