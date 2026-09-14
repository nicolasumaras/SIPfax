/* ITU-T V.42 (03/2002), 7.2.1 and Table 3. GPL-2.0. */
#include "v42detect.h"
int v42_detect_bit(V42Detect *s,unsigned bit)
{
 if(!s || bit>1)return 0;
 if(!s->uart_bits){
  if(bit){if(s->marks<17)++s->marks;return 0;}
  s->gap=s->marks;s->marks=0;s->byte=0;s->uart_bits=1;return 0;
 }
 if(s->uart_bits<=8){s->byte|=bit<<(s->uart_bits-1);++s->uart_bits;return 0;}
 s->uart_bits=0;
 if(!bit || (s->byte!=0x11 && s->byte!=0x91)){
  s->run=0;s->previous=0;return 0;
 }
 if(s->run && s->gap>=8 && s->gap<=16 && (s->previous^s->byte)==0x80){
  if(s->run<4)++s->run;
 }else s->run=1;
 s->previous=s->byte;
 if(s->run==4 && !s->seen){s->seen=1;return 1;}
 return 0;
}
int v42_decline_bit(unsigned n)
{
 if(n>=360)return -1;
 unsigned at=n%36,byte=at<18?0x45:0;
 at%=18;
 if(!at)return 0;
 if(at<=8)return (int)((byte>>(at-1))&1);
 return 1;
}
