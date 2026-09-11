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

static void selected_frame_status(void *opaque,const uint8_t *frame,int length,int ok)
{
    V90LapmLink *s=opaque;
    if(length<0)return;
    s->selected_frames++;if(ok)s->selected_valid_frames++;
    if(s->selected_frames<=10){
        unsigned addr=length>0?frame[0]:0;
        unsigned control=length>1?frame[1]:0;
        unsigned info0=length>2?frame[2]:0;
        fprintf(stderr,"[v42] selected stream HDLC frame: len=%d crc=%s"
                       " addr=%02x control=%02x info0=%02x\n",
                length,ok?"valid":"invalid",addr,control,info0);
    }
    if(ok && !s->selected_xid_dumped && length>=3
       && (frame[1]&0xec)==0xac && frame[2]==0x82){
        fprintf(stderr,"[v42] first selected XID:");
        for(int i=0;i<length;i++)fprintf(stderr," %02x",frame[i]);
        fputc('\n',stderr);s->selected_xid_dumped=1;
    }
}

static void odp(void *opaque,unsigned candidate,const uint8_t *bits,unsigned count)
{
    V90LapmLink *s=opaque;
    if(!s->detected){
        fprintf(stderr,"[v42] candidate %u completed ODP detection\n",candidate);
        s->adp_bits=0;
    }
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
        s->connected=s->detected=s->selection_count=0;
        s->selected_candidate=(unsigned)-1;s->adp_bits=0;s->restarts++;
        hdlc_rx_restart(&s->selected_hdlc);
        return;
    }
    hdlc_rx_put_bit(&s->selected_hdlc,bit);
    v42_rx_bit(&s->protocol,bit);
}

static void selected(void *opaque,unsigned candidate)
{
    V90LapmLink *s=opaque;
    /* ODP plus a valid XID or continuous flags establishes one ordered
       protocol stream. Leave detection before replaying its bounded tail. */
    int rate=s->protocol.tx_bit_rate;
    bool detect=s->protocol.detect;s->protocol.detect=false;
    v42_restart(&s->protocol);s->protocol.detect=detect;s->protocol.tx_bit_rate=rate;
    hdlc_rx_restart(&s->selected_hdlc);
    s->selected_candidate=candidate;s->selection_count++;
    s->selected_xid_dumped=0;
    fprintf(stderr,"[v42] selected LAPM candidate %u after %s\n",candidate,
            s->selector.selection_by_flags?"continuous flags":"CRC-valid XID");
}

static int supported_adp_bit(unsigned n)
{
    unsigned at=n%36,byte=at<18?0x45:0x43;at%=18;
    if(!at)return 0;
    if(at<=8)return (int)((byte>>(at-1))&1);
    return 1;
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
    hdlc_rx_init(&s->selected_hdlc,false,false,1,selected_frame_status,s);
    v90_lapm_select_init(&s->selector,s,odp,selected_output,selected);
    s->enabled=s->initialized=1;
}

int v90_lapm_link_tx_bit(void *opaque)
{
    V90LapmLink *s=opaque;
    if(!s->enabled)return 1;
    if(s->detected && !s->selection_count){
        int bit=supported_adp_bit(s->adp_bits++);
        if(s->adp_bits==360)
            fprintf(stderr,"[v42] transmitted ten ADPs; continue until protocol evidence\n");
        return bit;
    }
    return v42_tx_bit(&s->protocol);
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
