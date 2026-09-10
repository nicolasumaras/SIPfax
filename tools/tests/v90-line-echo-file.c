#include <stdio.h>
#include <stdlib.h>
#include "v90lineecho.h"
int main(int argc,char **argv){
 if(argc!=5)return 2;
 unsigned block=(unsigned)atoi(argv[4]);if(!block||block>2048)return 2;
 FILE *rx=fopen(argv[1],"rb"),*tx=fopen(argv[2],"rb"),*out=fopen(argv[3],"wb");if(!rx||!tx||!out)return 3;
 V90LineEcho *s=calloc(1,sizeof(*s));if(!s||!v90_line_echo_init(s,1428))return 4;
 int16_t r[2048],t[2048],z[2048];size_t n;
 while((n=fread(r,2,block,rx))){
  if(fread(t,2,n,tx)!=n)return 5;
  for(size_t i=0;i<n;++i)z[i]=v90_line_echo_rx(s,r[i]);
  for(size_t i=0;i<n;++i)v90_line_echo_tx(s,t[i]);
  if(fwrite(z,2,n,out)!=n)return 6;
 }
 free(s);fclose(rx);fclose(tx);return fclose(out)!=0;
}
