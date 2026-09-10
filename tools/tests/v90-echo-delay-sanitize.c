#include <assert.h>
#include <stdlib.h>
#include "v90echodelay.h"
int main(void){
 V90LineEcho *echo=calloc(1,sizeof(*echo));V90EchoDelay *s=calloc(1,sizeof(*s));assert(echo&&s);
 v90_echo_delay_init(s);
 for(unsigned n=0;n<120000;++n){
  int16_t rx=(n&1)?25:-25,tx=(n%4<2)?32767:-32768;
  v90_echo_delay_rx(s,rx);assert(v90_line_echo_rx(echo,rx)==rx);
  if(n%160==159){
   unsigned previous=s->next,searching=s->searching;
   v90_echo_delay_step(s,echo);
   assert(!s->locked);
   if(searching)assert(s->next-previous<=V90_ECHO_SEARCH_BUDGET);
  }
  v90_line_echo_tx(echo,tx);
 }
 assert(s->scans>0);
 s->candidate=1428;++echo->rx_samples;
 assert(!v90_echo_delay_step(s,echo));assert(!s->searching&&!s->candidate);
 free(s);free(echo);return 0;
}
