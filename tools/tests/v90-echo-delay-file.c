#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include "v90echodelay.h"
static double now(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return t.tv_sec+t.tv_nsec*1e-9;}
int main(int argc,char **argv){
 if(argc!=4)return 2;
 FILE *rx=fopen(argv[1],"rb"),*tx=fopen(argv[2],"rb"),*out=fopen(argv[3],"wb");if(!rx||!tx||!out)return 3;
 V90LineEcho *echo=calloc(1,sizeof(*echo));V90EchoDelay *delay=calloc(1,sizeof(*delay));if(!echo||!delay)return 4;
 v90_echo_delay_init(delay);int16_t r[160],t[160],z[160];size_t n;double peak=0,total=0;unsigned calls=0;
 while((n=fread(r,2,160,rx))){
  if(fread(t,2,n,tx)!=n)return 5;
  for(size_t i=0;i<n;++i){v90_echo_delay_rx(delay,r[i]);z[i]=v90_line_echo_rx(echo,r[i]);}
  double begin=now();v90_echo_delay_step(delay,echo);double elapsed=now()-begin;total+=elapsed;if(elapsed>peak)peak=elapsed;++calls;
  for(size_t i=0;i<n;++i)v90_line_echo_tx(echo,t[i]);
  if(fwrite(z,2,n,out)!=n)return 6;
 }
 printf("{\"locked\":%u,\"delay\":%u,\"lock_sample\":%llu,\"scans\":%u,\"peak_step_ms\":%.6f,\"total_step_ms\":%.6f,\"calls\":%u}\n",delay->locked,delay->delay,(unsigned long long)delay->lock_sample,delay->scans,peak*1000,total*1000,calls);
 free(echo);free(delay);fclose(rx);fclose(tx);return fclose(out)!=0;
}
