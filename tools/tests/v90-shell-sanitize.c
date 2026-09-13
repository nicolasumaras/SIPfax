#include <assert.h>
#include <stdint.h>
#include "v90shell.h"
int main(void){
 V90Shell s;uint32_t seed=1;
 for(unsigned m=1;m<=18;++m)for(unsigned k=0;k<=31;++k){
  if(!v90_shell_init(&s,m,k))continue;
  for(unsigned n=0;n<200;++n){
   seed=1664525u*seed+1013904223u;
   uint32_t index=seed&((UINT64_C(1)<<k)-1),decoded=0;
   uint8_t rings[8];assert(v90_shell_encode(&s,index,rings));
   assert(v90_shell_decode(&s,rings,&decoded));assert(index==decoded);
  }
 }
 return 0;
}
