#!/usr/bin/env python3
"""Initial V.42 decline must preserve DTE data and established-call behavior."""
from pathlib import Path
import subprocess,tempfile
root=Path(__file__).resolve().parents[2]
with tempfile.TemporaryDirectory() as td:
    d=Path(td);w=d/'check.c'
    w.write_text('''#include <assert.h>
#include <stdlib.h>
#include "v90phase4.c"
#include "v90startup.c"
static unsigned consumed;
static int dte(void*p){(void)p;++consumed;return 0;}
static V90Phase4 *prepare(void){
 V90Phase4*s=calloc(1,sizeof(*s));assert(v90_phase4_init_profile(s,0,78,28800,3200,1));
 s->stage=4;s->encoder.k=32;s->encoder.s=5;s->data_bits=48*37;
 s->get_data_bit=dte;s->rx_e_logged=1;s->upstream.b1_seen=1;
 s->upstream.b1_sample=320;s->upstream.samples=500;consumed=0;return s;
}
int main(void){
 unsetenv("SIPFAX_V90_V42_DECLINE");V90Phase4*s=prepare();assert(!s->v42_decline_enabled && data_bit(s)==0 && consumed==1);free(s);
 setenv("SIPFAX_V90_V42_DECLINE","true",1);s=prepare();assert(!s->v42_decline_enabled);free(s);
 setenv("SIPFAX_V90_V42_DECLINE","1",1);s=prepare();assert(s->v42_decline_enabled);
 s->data_bits=0;for(unsigned i=0;i<48*37;++i)assert(data_bit(s)==1);assert(!consumed);
 s->upstream.samples=6319;assert(data_bit(s)==1 && !consumed && !s->v42_complete);
 s->upstream.samples=6320;assert(data_bit(s)==0 && consumed==1 && s->v42_complete);
 s->upstream.odp_seen=1;assert(data_bit(s)==0 && !s->v42_reply_started);free(s);
 s=prepare();s->upstream.lcp_seen=1;assert(data_bit(s)==0 && s->v42_complete && consumed==1);free(s);
 s=prepare();s->upstream.odp_seen=1;
 for(unsigned i=0;i<360;++i){
  unsigned at=i%36,byte=at<18?0x45:0;at%=18;
  int expected=at==0?0:at<=8?(int)((byte>>(at-1))&1):1;
  if(i==90)s->upstream.lcp_seen=1; /* Do not cut ten complete ADPs short. */
  assert(data_bit(s)==expected && !consumed);
 }
 assert(s->v42_complete && s->v42_reply_bits==360);assert(data_bit(s)==0 && consumed==1);free(s);
 s=prepare();s->upstream.odp_seen=1;assert(data_bit(s)==0);
 s->stage=5;unsigned before=s->v42_reply_bits;assert(data_bit(s)==1 && s->v42_reply_bits==before && !consumed);
 s->renegotiations=1;s->stage=4;assert(data_bit(s)==0 && s->v42_complete && consumed==1);free(s);
 /* Actual startup-to-Phase4 path must preserve established PPP across retrain. */
 V90Startup*t=calloc(1,sizeof(*t));v90_startup_init(t,0);
 t->samples=20000;t->ranging_state=9;t->training_active=1;t->training.found=1;t->uinfo=78;
 t->have_upstream_data=1;int16_t in=0,out=0;v90_startup_process(t,&out,&in,1);
 t->training_tx.stage=2;v90_startup_process(t,&out,&in,1);
 assert(t->phase4_active && t->phase4.v42_complete);free(t);
 puts("PASS: opt-in, B1, T400 boundaries, DTE preservation, full ADP reply, early LCP and established-call retrain");
}
''')
    files=['v90training','v90dil','v90cp','v90pcm','v90upstream','v90trellis','v90qam8','v90equalizer','v90shell','v90mapping','v90train_tx','v42detect','v90odp']
    exe=d/'check';subprocess.run(['gcc','-O2','-Wall','-Wextra','-Werror','-I'+str(root/'vendor/linmodem'),str(w),*[str(root/'vendor/linmodem'/f'{f}.c') for f in files],'-lm','-o',str(exe)],check=True)
    subprocess.run([str(exe)],check=True)
