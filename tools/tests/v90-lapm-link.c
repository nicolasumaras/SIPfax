#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "v90lapmlink.h"

#define TOTAL 16384u
struct endpoint {unsigned side,sent,received;};
static unsigned test_sample,max_pending,saw_busy;
static uint8_t datum(unsigned side,unsigned n)
{
    uint32_t x=n+0x9e3779b9u*(side+1);x^=x>>16;x*=0x85ebca6bu;x^=x>>13;
    return (uint8_t)x;
}
static int source(void *opaque,uint8_t *out,int maximum)
{
    struct endpoint *p=opaque;unsigned n=TOTAL-p->sent;
    if(n>(unsigned)maximum)n=(unsigned)maximum;
    for(unsigned i=0;i<n;i++)out[i]=datum(p->side,p->sent++);
    return (int)n;
}
static void caller_sink(void *opaque,const uint8_t *data,int length)
{
    struct endpoint *p=opaque;
    if(length<0)return;
    for(int i=0;i<length;i++){assert(p->received<TOTAL);assert(data[i]==datum(0,p->received));p->received++;}
}
static int answer_sink(void *opaque,const uint8_t *data,int length)
{
    struct endpoint *p=opaque;unsigned accept=(unsigned)length;
    /* Hold the DTE closed, then exercise partial writes and recovery. */
    if(test_sample<3*8000)return 0;
    if(accept>37)accept=37;
    for(unsigned i=0;i<accept;i++){assert(p->received<TOTAL);assert(data[i]==datum(1,p->received));p->received++;}
    return (int)accept;
}
static void caller_status(void *opaque,int code){(void)opaque;(void)code;}
static void verify_ten_adps(void)
{
    struct endpoint unused={0};v42_state_t caller,answerer;
    assert(v42_init(&caller,true,true,source,caller_sink,&unused));
    assert(v42_init(&answerer,false,true,source,caller_sink,&unused));
    v42_set_status_callback(&caller,caller_status,&unused);
    v42_set_status_callback(&answerer,caller_status,&unused);
    for(unsigned i=0;i<1000 && !answerer.neg.odp_seen;i++)
        v42_rx_bit(&answerer,v42_tx_bit(&caller));
    assert(answerer.neg.odp_seen);
    int detect_state=answerer.lapm.state;unsigned bits=0;
    while(answerer.lapm.state==detect_state && bits<500){
        v42_tx_bit(&answerer);
        if(answerer.lapm.state==detect_state)bits++;
    }
    assert(bits==360 && answerer.neg.txadps==10);
    assert(!strcmp(lapm_status_to_str(answerer.lapm.state),"LAPM_IDLE"));
}
int main(void)
{
    verify_ten_adps();
    struct endpoint answer={.side=0},caller_ep={.side=1};
    V90LapmLink link;v90_lapm_link_init(&link,49333,&answer,source,answer_sink);
    v42_state_t caller;assert(v42_init(&caller,true,true,source,caller_sink,&caller_ep));
    caller.config.comp=0;caller.tx_bit_rate=28800;
    v42_set_status_callback(&caller,caller_status,&caller_ep);v42_restart(&caller);
    unsigned up_acc=0,down_acc=0,up_bits=0;
    for(test_sample=0;test_sample<8000*30;test_sample++){
        up_acc+=28800;
        while(up_acc>=8000){up_acc-=8000;v90_lapm_link_candidate_bit(&link,7,v42_tx_bit(&caller),up_bits++);}
        down_acc+=49333;
        while(down_acc>=8000){down_acc-=8000;v42_rx_bit(&caller,v90_lapm_link_tx_bit(&link));}
        v90_lapm_link_drain(&link);
        if(link.pending_count>max_pending)max_pending=link.pending_count;
        if(link.protocol.lapm.local_busy)saw_busy=1;
        if(answer.received==TOTAL && caller_ep.received==TOTAL)break;
    }
    assert(link.detected && link.selection_count==1 && link.selected_candidate==7);
    assert(link.connected && !link.disconnected && !link.errors && !link.overflow);
    assert(saw_busy && max_pending<=V90_LAPM_PENDING_BYTES);
    assert(answer.received==TOTAL && caller_ep.received==TOTAL && !link.pending_count);
    printf("LAPM candidate 7 transferred %u bytes each way\n",TOTAL);
    return 0;
}
