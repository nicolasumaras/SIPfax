/* V.42 LAPM bridge for the digital V.90 server. GPL-2.0. */
#include <stdio.h>
#include <string.h>
#include "v90lapmlink.h"

static int iframe_get(void *opaque,uint8_t *data,int maximum)
{
    V90LapmLink *s=opaque;
    return s->dte_get?s->dte_get(s->opaque,data,maximum):0;
}

static void iframe_put(void *opaque,const uint8_t *data,int length)
{
    V90LapmLink *s=opaque;
    if(length<=0)return;
    if((unsigned)length>V90_LAPM_PENDING_BYTES-s->pending_count){
        /* LAPM's negotiated maximum outstanding payload is 15*128 bytes,
           so reaching this guard indicates a broken embedding contract. */
        s->overflow=1;
        v42_set_local_busy_status(&s->protocol,true);
        fprintf(stderr,"[v42] receive queue invariant failed; link held busy\n");
        return;
    }
    unsigned tail=(s->pending_head+s->pending_count)%V90_LAPM_PENDING_BYTES;
    for(int i=0;i<length;i++){
        s->pending[tail]=data[i];
        tail=(tail+1)%V90_LAPM_PENDING_BYTES;
    }
    s->pending_count+=(unsigned)length;
    if(s->pending_count>V90_LAPM_PENDING_BYTES-128)
        v42_set_local_busy_status(&s->protocol,true);
}

static void status(void *opaque,int code)
{
    V90LapmLink *s=opaque;
    if(code==SIG_STATUS_LINK_CONNECTED){
        if(!s->connected)fprintf(stderr,"[v42] LAPM link connected\n");
        s->connected=1;
    }else if(code==SIG_STATUS_LINK_DISCONNECTED){
        s->connected=0;s->disconnected++;
        fprintf(stderr,"[v42] LAPM link disconnected\n");
    }else if(code==SIG_STATUS_LINK_ERROR){
        s->errors++;
        fprintf(stderr,"[v42] LAPM link error\n");
    }
}

static void odp(void *opaque,unsigned candidate,const uint8_t *bits,unsigned count)
{
    V90LapmLink *s=opaque;
    if(!s->detected)
        fprintf(stderr,"[v42] candidate %u completed ODP detection\n",candidate);
    s->detected=1;
    for(unsigned i=0;i<count;i++)v42_rx_bit(&s->protocol,bits[i]);
}

static void selected_output(void *opaque,int bit)
{
    V90LapmLink *s=opaque;
    if(bit<0){
        int rate=s->protocol.tx_bit_rate;
        v42_restart(&s->protocol);
        s->protocol.tx_bit_rate=rate;
        s->connected=s->detected=0;s->restarts++;
        return;
    }
    v42_rx_bit(&s->protocol,bit);
}

static void selected(void *opaque,unsigned candidate)
{
    V90LapmLink *s=opaque;
    s->selected_candidate=candidate;s->selection_count++;
    fprintf(stderr,"[v42] selected CRC-valid LAPM candidate %u\n",candidate);
}

void v90_lapm_link_init(V90LapmLink *s,int tx_bit_rate,void *opaque,
                        v90_lapm_get_func get,v90_lapm_put_func put)
{
    memset(s,0,sizeof(*s));s->opaque=opaque;s->dte_get=get;s->dte_put=put;
    s->selected_candidate=(unsigned)-1;
    v42_init(&s->protocol,false,true,iframe_get,iframe_put,s);
    s->protocol.config.comp=0;
    s->protocol.tx_bit_rate=tx_bit_rate;
    v42_set_status_callback(&s->protocol,status,s);
    v42_restart(&s->protocol);
    v90_lapm_select_init(&s->selector,s,odp,selected_output,selected);
    s->enabled=s->initialized=1;
}

int v90_lapm_link_tx_bit(void *opaque)
{
    V90LapmLink *s=opaque;
    return s->enabled?v42_tx_bit(&s->protocol):1;
}

void v90_lapm_link_candidate_bit(void *opaque,unsigned candidate,int bit,long source_sample)
{
    V90LapmLink *s=opaque;
    if(s->enabled)v90_lapm_select_bit(&s->selector,candidate,bit,source_sample);
}

void v90_lapm_link_drain(V90LapmLink *s)
{
    if(!s->enabled || !s->dte_put)return;
    while(s->pending_count){
        unsigned run=V90_LAPM_PENDING_BYTES-s->pending_head;
        if(run>s->pending_count)run=s->pending_count;
        int n=s->dte_put(s->opaque,s->pending+s->pending_head,(int)run);
        if(n<=0)break;
        if((unsigned)n>run)n=(int)run;
        s->pending_head=(s->pending_head+(unsigned)n)%V90_LAPM_PENDING_BYTES;
        s->pending_count-=(unsigned)n;
    }
    if(s->pending_count<=(V90_LAPM_PENDING_BYTES/2) && !s->overflow)
        v42_set_local_busy_status(&s->protocol,false);
}
