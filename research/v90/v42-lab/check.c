#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include "spandsp/telephony.h"
#include "spandsp/logging.h"
#include "spandsp/async.h"
#include "spandsp/hdlc.h"
#include "spandsp/v42.h"
#include "spandsp/private/logging.h"
#include "spandsp/private/hdlc.h"
#include "spandsp/private/v42.h"
#define TOTAL 65536u
#ifndef TEST_SECONDS
#define TEST_SECONDS 120
#endif
#ifndef ERROR_STOP_SECONDS
#define ERROR_STOP_SECONDS 120
#endif
struct peer {v42_state_t *v;unsigned side,sent,received,up,down,errors,down_sample;};
static unsigned sample, busy_violation, timer_violation;
struct delayed {unsigned due;int bit;};
static struct delayed queue[2][8192];static unsigned head[2],tail[2],max_queue[2];
static uint8_t data(unsigned side,unsigned n){uint32_t x=n+0x9e3779b9u*(side+1);x^=x>>16;x*=0x85ebca6bu;x^=x>>13;return x;}
static int source(void *opaque,uint8_t *out,int max){struct peer*p=opaque;unsigned n=TOTAL-p->sent;if(n>(unsigned)max)n=max;
#ifdef VARIABLE_FRAMES
unsigned limit=(p->sent*37u%127)+1;if(n>limit)n=limit;
#endif
for(unsigned i=0;i<n;i++){out[i]=data(p->side,p->sent++);}
return n;}
static void status(void *opaque,int code){struct peer*p=opaque;if(code==SIG_STATUS_LINK_CONNECTED)p->up++;if(code==SIG_STATUS_LINK_DISCONNECTED){p->down++;p->down_sample=sample;}if(code==SIG_STATUS_LINK_ERROR)p->errors++;}
static void sink(void *opaque,const uint8_t *in,int n){struct peer*p=opaque;if(n<0){status(p,n);return;}if(p->down){fprintf(stderr,"Payload delivered after disconnect\n");exit(5);}if(p->v->lapm.local_busy)busy_violation++;for(int i=0;i<n;i++){if(p->received>=TOTAL || in[i]!=data(p->side^1,p->received)){fprintf(stderr,"Mismatch at sample %u side %u byte %u\n",sample,p->side,p->received);exit(2);}p->received++;}}
int main(int argc,char**argv){
 unsigned corrupt=argc>1?atoi(argv[1]):0,asymmetric=argc>2?atoi(argv[2]):1,detect=argc>3?atoi(argv[3]):1;
 unsigned delay=argc>4?atoi(argv[4]):0,busy=argc>5?atoi(argv[5]):0,burst=argc>6?atoi(argv[6]):0;
 unsigned complete_sample=0;
 struct peer p[2]={{.side=0},{.side=1}};unsigned acc[2]={0},bits[2]={0},flips[2]={0},rates[2]={86400,asymmetric?148000:86400};
 for(unsigned i=0;i<2;i++){p[i].v=v42_init(NULL,i==0,detect,source,sink,p+i);if(!p[i].v)return 3;p[i].v->config.comp=0;p[i].v->tx_bit_rate=rates[i]/3;v42_set_status_callback(p[i].v,status,p+i);
#ifdef NEGOTIATED_LIMITS
p[i].v->config.v42_tx_n401=i?64:112;
p[i].v->config.v42_rx_n401=i?96:80;
p[i].v->config.v42_tx_window_size_k=i?3:9;
p[i].v->config.v42_rx_window_size_k=i?5:4;
#endif
v42_restart(p[i].v);}
 for(sample=0;sample<8000*TEST_SECONDS;sample++){
  if(busy){if(sample==5*8000)v42_set_local_busy_status(p[0].v,true);if(sample==7*8000)v42_set_local_busy_status(p[0].v,false);if(sample==9*8000)v42_set_local_busy_status(p[1].v,true);if(sample==11*8000)v42_set_local_busy_status(p[1].v,false);}
  for(unsigned i=0;i<2;i++){acc[i]+=rates[i];while(acc[i]>=24000){acc[i]-=24000;unsigned before_vs=p[i].v->lapm.vs;int bit=v42_tx_bit(p[i].v);if(p[i].v->lapm.vs!=before_vs && (p[i].v->bit_timer<=0 || p[i].v->bit_timer>p[i].v->tx_bit_rate))timer_violation++;bits[i]++;if(corrupt && sample>8000*3 && sample<8000*ERROR_STOP_SECONDS && bits[i]%(corrupt+i*997)==0){bit^=1;flips[i]++;}if(burst && sample>8000*3 && bits[i]%(50000+i*997)<burst){bit=1;flips[i]++;}
#ifdef OUTAGE_SECONDS
   if(sample>=5*8000 && sample<(5+OUTAGE_SECONDS)*8000)bit=1;
#endif
   if(tail[i]-head[i]>=8192){fprintf(stderr,"Delay queue overflow\n");return 4;}queue[i][tail[i]%8192]=(struct delayed){sample+delay*8,bit};tail[i]++;if(tail[i]-head[i]>max_queue[i])max_queue[i]=tail[i]-head[i];}
   while(head[i]!=tail[i] && queue[i][head[i]%8192].due<=sample){v42_rx_bit(p[i^1].v,queue[i][head[i]%8192].bit);head[i]++;}}

#ifdef OUTAGE_DISCONNECT
  if(sample>=40*8000)break;
#endif
  if(p[0].received==TOTAL && p[1].received==TOTAL){if(!complete_sample)complete_sample=sample;if(sample-complete_sample>=8000)break;}
 }
 printf("{\"seconds\":%.6f,\"period\":%u,\"asymmetric\":%u,\"detect\":%u,\"rx\":[%u,%u],\"tx\":[%u,%u],\"up\":[%u,%u],\"errors\":[%u,%u],\"flips\":[%u,%u],\"state\":[%d,%d]}\n",sample/8000.0,corrupt,asymmetric,detect,p[0].received,p[1].received,p[0].sent,p[1].sent,p[0].up,p[1].up,p[0].errors,p[1].errors,flips[0],flips[1],p[0].v->lapm.state,p[1].v->lapm.state);
 for(unsigned i=0;i<2;i++)fprintf(stderr,"peer%u vs=%u va=%u vr=%u put=%d get=%d acked=%d busy=%d/%d timer=%d retry=%d ctrl=%d/%d\n",i,p[i].v->lapm.vs,p[i].v->lapm.va,p[i].v->lapm.vr,p[i].v->lapm.info_put,p[i].v->lapm.info_get,p[i].v->lapm.info_acked,p[i].v->lapm.local_busy,p[i].v->lapm.far_busy,p[i].v->bit_timer,p[i].v->lapm.retry_count,p[i].v->lapm.ctrl_put,p[i].v->lapm.ctrl_get);
 for(unsigned i=0;i<2;i++)fprintf(stderr,"peer%u cfg=%d hdlc len=%zu flags=%d report=%d pos=%zu\n",i,p[i].v->lapm.configuring,p[i].v->lapm.hdlc_tx.len,p[i].v->lapm.hdlc_tx.flag_octets,p[i].v->lapm.hdlc_tx.report_flag_underflow,p[i].v->lapm.hdlc_tx.pos);
 printf("{\"delay_ms\":%u,\"busy\":%u,\"burst\":%u,\"busy_delivery_violations\":%u,\"max_queue\":[%u,%u]}\n",delay,busy,burst,busy_violation,max_queue[0],max_queue[1]);
 printf("{\"acknowledgement_timer_violations\":%u}\n",timer_violation);
 unsigned ok=p[0].received==TOTAL&&p[1].received==TOTAL&&!busy_violation&&!timer_violation;
 for(unsigned i=0;i<2;i++)ok=ok && p[i].v->lapm.va==p[i].v->lapm.vs && p[i].up==1 && p[i].down==0 && p[i].errors==0 && !strcmp(lapm_status_to_str(p[i].v->lapm.state),"LAPM_DATA");
#ifdef NEGOTIATED_LIMITS
 unsigned matched=p[0].v->lapm.tx_n401==112 && p[0].v->lapm.rx_n401==80 && p[0].v->lapm.tx_window_size_k==9 && p[0].v->lapm.rx_window_size_k==4 && p[1].v->lapm.tx_n401==80 && p[1].v->lapm.rx_n401==112 && p[1].v->lapm.tx_window_size_k==4 && p[1].v->lapm.rx_window_size_k==9;
 printf("{\"negotiated_limits_match\":%s}\n",matched?"true":"false");
 ok=ok && matched;
#endif
#ifdef OUTAGE_SECONDS
 printf("{\"outage_seconds\":%u,\"disconnect_count\":[%u,%u],\"disconnect_seconds\":[%.6f,%.6f]}\n",OUTAGE_SECONDS,p[0].down,p[1].down,p[0].down_sample/8000.0,p[1].down_sample/8000.0);
#ifdef OUTAGE_DISCONNECT
 ok=1;
 for(unsigned i=0;i<2;i++)ok=ok && p[i].up==1 && p[i].down==1 && p[i].down_sample>5*8000 && p[i].down_sample<35*8000 && !strcmp(lapm_status_to_str(p[i].v->lapm.state),"LAPM_IDLE") && p[i].received<TOTAL;
#endif
#endif
 for(unsigned i=0;i<2;i++)v42_free(p[i].v);
 return ok?0:1;
}
