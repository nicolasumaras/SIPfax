/* Replay-only boundary check; outputs counts, never decoded payloads.
 * Captured PCM is supplied explicitly and is not stored in the repository. */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include "v90upstream.h"
typedef struct {
 V42Detect detection;
 unsigned bits,word,escape,length,overflow,frames,lcp,last_valid_length;
 uint8_t frame[V90_UP_FRAME];
 unsigned crc;
} Candidate;
static Candidate candidates[V90_UP_CANDIDATES];
static int selected=-1;
static unsigned resets,selected_resets,native_frames,matched_frames,selected_frames,selected_lcp;
static void receive_bit(void *opaque,unsigned id,int value,long sample)
{
 (void)opaque;(void)sample;
 if(id>=V90_UP_CANDIDATES)abort();
 Candidate*c=&candidates[id];
 if(value<0){memset(c,0,sizeof(*c));c->crc=0xffff;resets++;if(selected==(int)id){selected=-1;selected_resets++;}return;}
 if(value>1)abort();
 if(v42_detect_bit(&c->detection,(unsigned)value) && selected<0)selected=(int)id;
 if(!c->bits){if(!value){c->bits=1;c->word=0;}return;}
 if(c->bits<=8){c->word|=(unsigned)value<<(c->bits-1);c->bits++;return;}
 c->bits=0;if(!value)return;
 unsigned b=c->word;
 if(b==0x7e){
  if(!c->overflow && !c->escape && c->length>=4 && c->crc==0xf0b8){
   c->frames++;
   c->last_valid_length=c->length;
   unsigned lcp=c->length>=10 && !memcmp(c->frame,"\xff\x03\xc0\x21",4) && c->frame[4]>=1 && c->frame[4]<=4;
   c->lcp+=lcp;
   if(selected==(int)id){selected_frames++;selected_lcp+=lcp;}
  }
  /* Keep bytes until the native frame callback, which follows this bit. */
  c->length=0;c->escape=0;c->overflow=0;c->crc=0xffff;return;
 }
 if(b==0x7d){c->escape=1;return;}
 if(c->escape){b^=0x20;c->escape=0;}
 if(c->length==V90_UP_FRAME){c->overflow=1;return;}
 c->frame[c->length++]=(uint8_t)b;c->crc^=b;
 for(unsigned i=0;i<8;i++)c->crc=(c->crc>>1)^((c->crc&1)?0x8408:0);
}
static void receive_frame(void *opaque,const uint8_t *frame,unsigned length)
{
 (void)opaque;native_frames++;
 for(unsigned i=0;i<V90_UP_CANDIDATES;i++)
  if(candidates[i].frames && candidates[i].last_valid_length==length &&
     !memcmp(candidates[i].frame,frame,length)){matched_frames++;return;}
 fprintf(stderr,"Native frame did not match observer bytes\n");exit(2);
}
int main(int argc,char **argv)
{
 if(argc!=6){fprintf(stderr,"usage: replay recording rate baud start_sample count\n");return 2;}
 FILE*f=fopen(argv[1],"rb");if(!f)return 2;
 unsigned rate=(unsigned)strtoul(argv[2],NULL,10),baud=(unsigned)strtoul(argv[3],NULL,10);
 long start=strtol(argv[4],NULL,10),count=strtol(argv[5],NULL,10);
 if(start<0 || count<=0 || fseek(f,start*2,SEEK_SET)){fclose(f);return 2;}
 V90Upstream*s=calloc(1,sizeof(*s));if(!s || !v90_upstream_init_profile(s,rate,baud,1))return 2;
 for(unsigned i=0;i<V90_UP_CANDIDATES;i++)candidates[i].crc=0xffff;
 s->require_b1=1;s->receive_bit=receive_bit;s->receive_frame=receive_frame;
 for(long i=0;i<count;i++){unsigned char b[2];if(fread(b,1,2,f)!=2){fprintf(stderr,"Recording too short\n");return 2;}v90_upstream_receive(s,(int16_t)((unsigned)b[0]|((unsigned)b[1]<<8)));}
 printf("{\"native_frames\":%u,\"exact_observer_matches\":%u,\"selected_candidate\":%d,\"selected_frames\":%u,\"selected_lcp\":%u,\"reset_events\":%u,\"selected_resets\":%u}\n",native_frames,matched_frames,selected,selected_frames,selected_lcp,resets,selected_resets);
 free(s);fclose(f);
 /* Candidate selection is diagnostic here. Exact boundary reconstruction is
  * the interface contract; the caller must implement a stronger selector. */
 return native_frames==matched_frames?0:1;
}
