#include <assert.h>
#include <stdint.h>
#include <string.h>
#include "v90lapmselect.h"
#include "spandsp/v42.h"
#include "spandsp/private/logging.h"
#include "spandsp/private/async.h"
#include "spandsp/private/v42.h"
static unsigned odp_events,selection_events,output_bits,output_invalid;
static int chosen=-1;
static v42_state_t *answerer;
static void odp(void *p,unsigned candidate,const uint8_t *bits,unsigned count)
{
    (void)p;(void)candidate;odp_events++;
    for(unsigned i=0;i<count;i++)v42_rx_bit(answerer,bits[i]);
}
static void selection(void *p,unsigned candidate){(void)p;chosen=(int)candidate;selection_events++;}
static void output(void *p,int bit){(void)p;if(bit<0)output_invalid++;else{assert(bit<=1);output_bits++;}}
static int empty(void *p,uint8_t *b,int n){(void)p;(void)b;(void)n;return 0;}
static void discard(void *p,const uint8_t *b,int n){(void)p;(void)b;(void)n;}
static void feed_odp(V90LapmSelect *s,unsigned candidate)
{
    v42_state_t *caller=v42_init(NULL,true,true,empty,discard,NULL);assert(caller);
    for(unsigned i=0;i<400 && !s->candidate[candidate].odp_reported;i++)
        v90_lapm_select_bit(s,candidate,v42_tx_bit(caller),i);
    assert(s->candidate[candidate].odp_reported);v42_free(caller);
}
static void feed_xid(V90LapmSelect *s,unsigned candidate,int corrupt)
{
    hdlc_tx_state_t tx;uint8_t xid[]={1,0xaf,0x82};
    hdlc_tx_init(&tx,false,1,false,NULL,NULL);
    assert(hdlc_tx_flags(&tx,5)==0);
    assert(hdlc_tx_frame(&tx,xid,sizeof(xid))==0);
    if(corrupt)assert(hdlc_tx_corrupt_frame(&tx)==0);
    /* Stop before ten trailing flags can independently satisfy the
       protocol-phase evidence rule. */
    for(unsigned i=0;i<130 && s->selected<0;i++)
        v90_lapm_select_bit(s,candidate,hdlc_tx_get_bit(&tx),500+i);
}
static void feed_flags(V90LapmSelect *s,unsigned candidate,unsigned count)
{
    static const unsigned flag[8]={0,1,1,1,1,1,1,0};
    for(unsigned n=0;n<count;n++)for(unsigned i=0;i<8;i++)
        v90_lapm_select_bit(s,candidate,(int)flag[i],3000+8*n+i);
}
int main(void)
{
    answerer=v42_init(NULL,false,true,empty,discard,NULL);assert(answerer);
    V90LapmSelect s;v90_lapm_select_init(&s,NULL,odp,output,selection);
    /* A CRC-valid XID without prior ODP is not selection evidence. */
    feed_xid(&s,3,0);assert(s.selected<0 && !selection_events);
    feed_odp(&s,7);assert(odp_events==1 && s.selected<0 && answerer->neg.odp_seen);
    /* Another candidate and a bad FCS cannot steal selection. */
    feed_odp(&s,8);feed_xid(&s,8,1);assert(s.selected<0);
    feed_xid(&s,7,0);
    assert(s.selected==7 && chosen==7 && selection_events==1 && output_bits>128);
    v90_lapm_select_bit(&s,8,0,2000);unsigned before=output_bits;
    v90_lapm_select_bit(&s,7,1,2001);assert(output_bits==before+1);
    v90_lapm_select_bit(&s,7,-1,2002);
    assert(s.selected<0 && output_invalid==1 && s.invalidations==1);
    /* Continuous flags are the protocol-phase indication in V.42 7.2.1.3.
       Require a sustained run on the same ODP-qualified candidate. */
    feed_odp(&s,9);
    feed_flags(&s,9,V90_LAPM_FLAG_EVIDENCE-1);assert(s.selected<0);
    feed_flags(&s,9,1);
    assert(s.selected==9 && chosen==9 && selection_events==2 && s.flag_selections==1);
    v90_lapm_select_bit(&s,9,-1,4000);assert(s.selected<0 && output_invalid==2);
    feed_odp(&s,10);
    for(unsigned i=0;i<V90_LAPM_BUFFER_BITS;i++)v90_lapm_select_bit(&s,10,1,5000+i);
    assert(s.overflows==1 && !s.candidate[10].active);
    v42_free(answerer);
    return 0;
}
