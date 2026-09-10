#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "v90lineecho.h"
int main(void){
 V90LineEcho *s=calloc(1,sizeof(*s)),*saved=malloc(sizeof(*s));assert(s&&saved);
 assert(!v90_line_echo_init(NULL,1428));assert(v90_line_echo_init(s,1428));*saved=*s;
 assert(!v90_line_echo_init(s,31));assert(!memcmp(s,saved,sizeof(*s)));
 assert(!v90_line_echo_init(s,7001));assert(!memcmp(s,saved,sizeof(*s)));
 for(unsigned n=0;n<20000;++n)assert(v90_line_echo_rx(s,1234)==1234);
 assert(v90_line_echo_init(s,1428));
 for(unsigned n=0;n<20000;++n)v90_line_echo_tx(s,32767);
 for(unsigned n=0;n<1000;++n)assert(v90_line_echo_rx(s,-32768)==-32768);
 for(unsigned delay=32;delay<=7000;delay+=6968){
  assert(v90_line_echo_init(s,delay));
  for(unsigned n=0;n<100000;++n){
   int16_t v=(int16_t)((n*7919u)&65535u);v90_line_echo_rx(s,v);v90_line_echo_tx(s,v);
  }
  double norm=0;for(unsigned k=0;k<V90_LINE_ECHO_TAPS;++k)norm+=s->coefficients[k]*s->coefficients[k];
  assert(isfinite(norm)&&norm<.01);
 }
 free(s);free(saved);return 0;
}
