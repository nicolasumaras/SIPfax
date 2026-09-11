/* Independent asymmetric XID negotiation, establishment and restart fixtures. */
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "spandsp/telephony.h"
#include "spandsp/logging.h"
#include "spandsp/async.h"
#include "spandsp/hdlc.h"
#include "spandsp/v42.h"
#include "spandsp/private/logging.h"
#include "spandsp/private/hdlc.h"
#include "spandsp/private/v42.h"
static int empty(void *p,uint8_t *b,int n){(void)p;(void)b;(void)n;return 0;}
static void discard(void *p,const uint8_t *b,int n){(void)p;(void)b;(void)n;}
static v42_state_t *create(void){v42_state_t*s=v42_init(NULL,true,false,empty,discard,NULL);if(!s)exit(3);s->config.comp=0;v42_restart(s);return s;}
int main(void)
{
 v42_state_t *s=create();
 s->config.v42_tx_n401=64; s->config.v42_rx_n401=96;
 s->config.v42_tx_window_size_k=3; s->config.v42_rx_window_size_k=5;
 v42_restart(s);
 /* Remote proposes TX=112, RX=80 octets, TX window=9, RX window=4. */
 uint8_t command[]={1,0xaf,0x82,0x80,0,14,5,2,3,128,6,2,2,128,7,1,9,8,1,4};
 command[0]=s->lapm.rsp_addr;
 lapm_receive(s,command,sizeof(command),true);
 int values=s->lapm.tx_n401==80 && s->lapm.rx_n401==112 && s->lapm.tx_window_size_k==4 && s->lapm.rx_window_size_k==9;
 v42_frame_t *f=&s->lapm.ctrl_buf[0];
 int wire=f->len==26 && f->buf[14]==2 && f->buf[15]==128 && f->buf[18]==3 && f->buf[19]==128 && f->buf[22]==4 && f->buf[25]==9;
 uint8_t sabme[]={s->lapm.rsp_addr,0x7f};
 lapm_receive(s,sabme,sizeof(sabme),true);
 int retained=s->lapm.tx_n401==80 && s->lapm.rx_n401==112 && s->lapm.tx_window_size_k==4 && s->lapm.rx_window_size_k==9;
 v42_restart(s);
 int reset=s->lapm.tx_n401==64 && s->lapm.rx_n401==96 && s->lapm.tx_window_size_k==3 && s->lapm.rx_window_size_k==5;
 printf("{\"opposite_directions\":%s,\"reply_selected_values\":%s,\"sabme_preserves_negotiation\":%s,\"restart_restores_configuration\":%s}\n",values?"true":"false",wire?"true":"false",retained?"true":"false",reset?"true":"false");
 uint8_t absent[]={s->lapm.rsp_addr,0xaf,0x82};
 lapm_receive(s,absent,sizeof(absent),true);
 int defaults=s->lapm.tx_n401==128 && s->lapm.rx_n401==128 && s->lapm.tx_window_size_k==15 && s->lapm.rx_window_size_k==15;
 printf("{\"omitted_parameters_use_standard_defaults\":%s}\n",defaults?"true":"false");
 v42_free(s);
 return !(values && wire && retained && reset && defaults);
}
