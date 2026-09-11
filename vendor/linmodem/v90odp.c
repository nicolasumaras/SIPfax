/* Causal V.42-pattern equalizer targets; experimental, GPL-2.0. */
#include <string.h>
#include "v90odp.h"
void v90_odp_observe(V90ODPTrainer*p,const V90Mapping*m,unsigned long long frame,const uint16_t labels[8],const uint8_t*bits,unsigned frame_bits){
 if(!p || !m || !labels)return;
 if(!bits){memset(&p->detector,0,sizeof(p->detector));p->scrambler=0;p->have_anchor=0;p->gap_run=0;return;}
 long long at=(frame/m->p)*(m->rate/25);
 for(unsigned j=0;j<frame%m->p;++j)at+=v90_mapping_frame_bits(m,j);
 for(unsigned i=0;i<frame_bits;++i,++at){
  unsigned plain=bits[i]^((p->scrambler>>4)&1)^((p->scrambler>>22)&1);
  p->scrambler=((p->scrambler<<1)|bits[i])&0x7fffff;
  unsigned stop=p->detector.uart_bits==9,byte=p->detector.byte,gap=p->detector.gap;
  v42_detect_bit(&p->detector,plain);
  if(stop){
   if(plain && gap==14 && p->detector.run>=2){if(p->gap_run<3)++p->gap_run;}
   else p->gap_run=0;
  }
  if(stop && p->gap_run>=3 && p->detector.run>=4){
   p->have_anchor=1;p->epoch=at-9-(byte==145?24:0);p->last_confirm=at;
  }
 }
 if(!p->have_anchor || at-p->last_confirm>24)return;
 unsigned state=0,previous=labels[6]&3,reg=p->scrambler;
 /* Four observed input/output pairs reconstruct the convolutional state.
    The actual parity supplies u, so no unknown pre-frame state is assumed. */
 for(unsigned j=0;j<4;++j){
  unsigned a=labels[2*j]&3,b=labels[2*j+1]&3;
  unsigned u=((b+4-a)&1)^v90_mapping_inversion(m,4*frame+j);
  unsigned y1=((a&1)&((b&1)^1))^(a>>1)^(b>>1),y2=a&1;
  state=(state>>1)^y1^(y2<<1)^((y2^u)<<2)^(u<<3);
 }
 for(unsigned f=1;f<=24;++f){
  unsigned n=v90_mapping_frame_bits(m,frame+f),k=m->k-(n<m->b);
  unsigned char v[78],rings[8];unsigned index=0;
  for(unsigned i=0;i<n;++i,++at){
   unsigned phase=(unsigned)((at-p->epoch)%48),value=phase<24?17:145;phase%=24;
   unsigned plain=!phase?0:phase<=8?(value>>(phase-1))&1:1;
   v[i]=plain^((reg>>4)&1)^((reg>>22)&1);reg=((reg<<1)|v[i])&0x7fffff;
  }
  for(unsigned i=0;i<k;++i)index|=(unsigned)v[i]<<i;
  if(!v90_shell_encode(&m->shell,index,rings))return;
  for(unsigned j=0;j<4;++j){
   unsigned offset=k+(3+2*m->q)*j,a=(previous+v[offset+1]+2*v[offset+2])&3,u=state&1;
   unsigned b=(a+2*v[offset]+(u^v90_mapping_inversion(m,4*(frame+f)+j)))&3,qa=0,qb=0;
   for(unsigned i=0;i<m->q;++i){qa|=(unsigned)v[offset+3+i]<<i;qb|=(unsigned)v[offset+3+m->q+i]<<i;}
   unsigned long long symbol=8*(frame+f)+2*j;
   p->labels[symbol%512]=a+4*((rings[2*j]<<m->q)|qa);p->tags[symbol%512]=symbol+1;
   ++symbol;p->labels[symbol%512]=b+4*((rings[2*j+1]<<m->q)|qb);p->tags[symbol%512]=symbol+1;
   unsigned y1=((a&1)&((b&1)^1))^(a>>1)^(b>>1),y2=a&1;
   state=(state>>1)^y1^(y2<<1)^((y2^u)<<2)^(u<<3);previous=a;
  }
 }
 ++p->updates;
}
int v90_odp_label(V90ODPTrainer*p,unsigned long long n,unsigned*label){
 if(!p || !label || n==UINT64_MAX || p->tags[n%512]!=n+1)return 0;
 *label=p->labels[n%512];++p->used;return 1;
}
