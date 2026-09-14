/* Experimental causal transmit-reference NLMS. GPL-2.0.
 * Delay is supplied by the experiment; automatic acquisition is not implemented.
 * RX can precede TX within an audio block. Unavailable references bypass safely. */
#include <math.h>
#include <string.h>
#include "v90lineecho.h"
int v90_line_echo_init(V90LineEcho *s,unsigned delay)
{
 if(!s || delay<32 || delay>7000)return 0;
 memset(s,0,sizeof(*s));s->delay=delay;s->enabled=1;return 1;
}
void v90_line_echo_tx(V90LineEcho *s,int16_t value)
{
 s->history[s->tx_samples%V90_LINE_ECHO_RING]=value;++s->tx_samples;
}
int16_t v90_line_echo_rx(V90LineEcho *s,int16_t value)
{
 uint64_t n=s->rx_samples++;
 if(!s->enabled || n<s->delay+32)return value;
 uint64_t newest=n-(s->delay-32),oldest=n-(s->delay+32);
 if(newest>=s->tx_samples || s->tx_samples-oldest>V90_LINE_ECHO_RING)return value;
 double x[V90_LINE_ECHO_TAPS],candidate[V90_LINE_ECHO_TAPS];
 double prediction=0,energy=0,norm=0;
 for(unsigned k=0;k<V90_LINE_ECHO_TAPS;++k){
  x[k]=s->history[(newest-k)%V90_LINE_ECHO_RING];
  prediction+=s->coefficients[k]*x[k];energy+=x[k]*x[k];
 }
 double error=value-prediction;
 if(energy>V90_LINE_ECHO_TAPS*64.){
  for(unsigned k=0;k<V90_LINE_ECHO_TAPS;++k){
   candidate[k]=s->coefficients[k]+.0005*error*x[k]/energy;
   norm+=candidate[k]*candidate[k];
  }
  if(isfinite(norm)&&norm<.01)memcpy(s->coefficients,candidate,sizeof(candidate));
 }
 if(error>32767)error=32767;
 if(error<-32768)error=-32768;
 return (int16_t)lrint(error);
}
