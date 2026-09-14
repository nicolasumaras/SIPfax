#include <assert.h>
#include <stdio.h>
#include <stdint.h>
void *v34_phase2_new(void);
void v34_phase2_free(void *);
int v34_phase2_run(void *,int16_t *,int16_t *,int);
int v34_phase2_md_ms(void *);
int main(int argc,char **argv)
{
    void *s=v34_phase2_new();assert(s);
    assert(v34_phase2_md_ms(s)==-1);
    if(argc==2) {
        FILE*f=fopen(argv[1],"rb");assert(f);
        int16_t in[160],out[160];size_t n;int done=0;
        while((n=fread(in,sizeof(*in),160,f))==160) {
            int result=v34_phase2_run(s,out,in,(int)n);
            if(result==1){done=1;break;}
            assert(result!=-1);
        }
        fclose(f);assert(done);assert(v34_phase2_md_ms(s)==700);
        puts("PASS: Phase 2 passes CRC-validated 700 ms MD from captured audio");
    }
    v34_phase2_free(s);return 0;
}
