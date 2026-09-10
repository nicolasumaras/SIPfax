/* Causal delay acquisition with bounded work per audio block. GPL-2.0. */
#include <math.h>
#include <string.h>
#include "v90echodelay.h"
void v90_echo_delay_init(V90EchoDelay *s){memset(s,0,sizeof(*s));}
void v90_echo_delay_rx(V90EchoDelay *s,int16_t value)
{
 s->received[s->samples%V90_ECHO_WINDOW]=value;++s->samples;
}
int v90_echo_delay_step(V90EchoDelay *s,V90LineEcho *echo)
{
 if(s->locked)return 1;
 if(s->samples!=echo->rx_samples){s->searching=0;s->candidate=0;return 0;}
 if(!s->searching){
  uint64_t count=s->samples;
  if(count<V90_ECHO_MAX_DELAY+V90_ECHO_WINDOW || count-s->last_scan<V90_ECHO_WINDOW)return 0;
  s->last_scan=count;
  if(echo->tx_samples<V90_ECHO_WINDOW || echo->tx_samples>count)return 0;
  uint64_t minimum=count-echo->tx_samples;
  if(minimum<32)minimum=32;
  if(minimum>V90_ECHO_MAX_DELAY-129){s->candidate=0;return 0;}
  uint64_t first=count-V90_ECHO_WINDOW-V90_ECHO_MAX_DELAY;
  if(echo->tx_samples-first>V90_LINE_ECHO_RING)return 0;
  double energy=0,tx_energy=0;
  for(unsigned k=0;k<V90_ECHO_WINDOW;++k){
   double a=s->received[(count-V90_ECHO_WINDOW+k)%V90_ECHO_WINDOW];
   double b=echo->history[(echo->tx_samples-V90_ECHO_WINDOW+k)%V90_LINE_ECHO_RING];
   s->window[k]=a;energy+=a*a;tx_energy+=b*b;
  }
  if(tx_energy<V90_ECHO_WINDOW*64. || energy>=.002*tx_energy){s->candidate=0;return 0;}
  unsigned length=V90_ECHO_WINDOW+V90_ECHO_MAX_DELAY-(unsigned)minimum;
  for(unsigned k=0;k<length;++k)s->reference[k]=echo->history[(first+k)%V90_LINE_ECHO_RING];
  s->energy=energy;s->minimum=(unsigned)minimum;s->next=s->minimum;s->searching=1;++s->scans;
 }
 unsigned stop=s->next+V90_ECHO_SEARCH_BUDGET;
 if(stop>V90_ECHO_MAX_DELAY+1)stop=V90_ECHO_MAX_DELAY+1;
 for(unsigned lag=s->next;lag<stop;++lag){
  unsigned offset=V90_ECHO_MAX_DELAY-lag;double cross=0,energy=0;
  for(unsigned k=0;k<V90_ECHO_WINDOW;++k){double b=s->reference[offset+k];cross+=s->window[k]*b;energy+=b*b;}
  double denominator=sqrt(s->energy*energy);
  s->scores[lag]=denominator>1e-9?fabs(cross/denominator):0;
 }
 s->next=stop;if(stop<=V90_ECHO_MAX_DELAY)return 0;
 unsigned best=s->minimum;double second=0;
 for(unsigned k=s->minimum;k<=V90_ECHO_MAX_DELAY;++k)if(s->scores[k]>s->scores[best])best=k;
 for(unsigned k=s->minimum;k<=V90_ECHO_MAX_DELAY;++k)
  if((k>best?k-best:best-k)>64 && s->scores[k]>second)second=s->scores[k];
 s->searching=0;
 if(s->scores[best]>=.2 && s->scores[best]>=3*second){
  if(s->candidate && (best>s->candidate?best-s->candidate:s->candidate-best)<=4){
   s->locked=1;s->delay=best;s->lock_sample=s->samples;
   echo->delay=best;echo->enabled=1;return 1;
  }
  s->candidate=best;
 }else s->candidate=0;
 return 0;
}
