/* Exact-allocation malformed-frame and independently specified XID wire checks. */
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
int main(int argc,char **argv){
 if(argc>1 && !strcmp(argv[1],"wire")){
  static const uint8_t expected[]={0x03,0xaf,0x82,0x80,0x00,0x14,0x03,0x04,0x8a,0x89,0x00,0x00,0x05,0x02,0x04,0x00,0x06,0x02,0x04,0x00,0x07,0x01,0x0f,0x08,0x01,0x0f};
  static const uint8_t private_group[]={0xf0,0,15,0,3,'V','4','2',1,1,3,2,2,2,0,3,1,6};
  for(unsigned compression=0;compression<2;compression++){
   v42_state_t*s=create();s->config.comp=compression?3:0;for(unsigned i=0;i<400;i++)v42_tx_bit(s);
   v42_frame_t*f=&s->lapm.ctrl_buf[0];unsigned length=sizeof(expected)+(compression?sizeof(private_group):0);
   int ok=f->len==length && !memcmp(f->buf,expected,sizeof(expected)) && (!compression || !memcmp(f->buf+sizeof(expected),private_group,sizeof(private_group)));
   printf("{\"xid_wire_exact\":%s,\"compression_fixture\":%u,\"frame_length\":%d}\n",ok?"true":"false",compression,f->len);v42_free(s);if(!ok)return 1;
  }
  return 0;
 }
 static const struct {unsigned n;uint8_t b[20];} invalid[]={
  {0,{0}}, {1,{3}}, {2,{3,0}}, {2,{3,1}}, {2,{3,0xaf}},
  {3,{3,0xaf,0}}, {4,{3,0xaf,0x82,0x80}}, {5,{3,0xaf,0x82,0x80,0}},
  {6,{3,0xaf,0x82,0x80,0,1}}, {7,{3,0xaf,0x82,0x80,0,1,5}},
  {9,{3,0xaf,0x82,0x80,0,3,5,2,4}},
  {13,{3,0xaf,0x82,0x80,0,7,5,2,0,64,7,2,1}},
  {7,{3,0xaf,0x82,0xf0,0,1,1}}, {9,{3,0xaf,0x82,0xf0,0,3,2,2,4}}
 };
 for(unsigned i=0;i<sizeof(invalid)/sizeof(invalid[0]);i++){
  v42_state_t*s=create();s->lapm.state=3;s->lapm.configuring=true;
  v42_state_t before=*s;uint8_t*b=malloc(invalid[i].n);if(invalid[i].n && !b)return 3;
  if(invalid[i].n)
   memcpy(b,invalid[i].b,invalid[i].n);
  lapm_receive(s,b,invalid[i].n,true);
  int changed=memcmp(s,&before,sizeof(*s));free(b);v42_free(s);
  if(changed){fprintf(stderr,"Invalid case %u changed protocol state\n",i);return 1;}
 }
 printf("{\"invalid_frames_ignored_without_state_change\":%zu}\n",sizeof(invalid)/sizeof(invalid[0]));return 0;
}
