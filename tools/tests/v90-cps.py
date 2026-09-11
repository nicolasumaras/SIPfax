#!/usr/bin/env python3
"""CPs acknowledgement, aligned silence/Rt, and resumed MP/data guards."""
import ctypes as C
from pathlib import Path
import subprocess
import tempfile
root=Path(__file__).resolve().parents[2]
with tempfile.TemporaryDirectory() as tmp:
    wrapper=Path(tmp)/'test.c'
    wrapper.write_text(r'''
#include <stdlib.h>
#include "v90phase4.c"
static unsigned consumed;
static int dte(void *unused){++consumed;return 1;}
#define CHECK(x) do {if(!(x))return __LINE__;} while(0)
int run(int law,unsigned offset,unsigned sr,unsigned ld){
 V90Phase4 s;v90_phase4_init(&s,law,78);
 s.cpt.drn=9;s.cpt.sr=sr;s.cpt.lookahead=ld;s.cpt.alaw=law;
 s.cpt.count=1;s.cpt.filter[0]=63;
 for(unsigned u=1;u<100;++u)s.cpt.mask[0][0][u]=1;
 s.preceding_cp=s.cpt;s.preceding_cp.type=1;
 s.cp=s.preceding_cp;s.cp.silence=1;
 CHECK(!v90_pcm_renegotiate(&s.encoder,&s.preceding_cp,&s.cpt,training_bit,&s));
 s.get_data_bit=dte;consumed=0;s.have_cpt=1;s.renegotiations=1;
 s.stage=2;s.have_cp=1;s.trn_frames=0;s.mp_length=102;
 s.samples=s.trn_start=600+offset;s.data_start=offset;
 s.rx.e_seen=1;mp_build(&s);
 /* E cannot replace CPs-prime. MP-prime must continue without silence. */
 for(unsigned i=0;i<1200;++i)v90_phase4_next(&s,0);
 CHECK(s.stage==2 && !s.ed_frame && !consumed);
 s.rx.cp=s.cp;s.rx.cp.ack=1;receive_cp(&s);
 for(unsigned i=0;i<1200 && s.stage!=7;++i)v90_phase4_next(&s,0);
 CHECK(s.stage==7 && !consumed && !s.encoder.queued);
 CHECK(s.generated==(s.ed_frame+2)*(s.encoder.k+s.encoder.s));
 V90Pcm paused=s.encoder;
 CHECK((s.samples-1-s.data_start)%6==0);
 for(unsigned i=0;i<800;++i)CHECK(v90_phase4_next(&s,0)==(law?8:0));
 /* Repeated CPs-prime must leave the modem in silence. */
 receive_cp(&s);CHECK(s.stage==7);
 s.rx.cp=s.preceding_cp;s.rx.cp.ack=0;receive_cp(&s);
 CHECK(s.stage==8 && !s.mp_ack && !s.rx.e_seen);
 unsigned tones=0;
 while(s.stage==8){
  unsigned before=s.samples;
  int out=v90_phase4_next(&s,0);
  if(s.rt_start){
   unsigned n=before-s.rt_start;
   int expected=v90_pcm_level(law,s.encoder.map[n%6][0]);
   if((n%6>=3) != (n>=384))expected=-expected;
   CHECK(out==expected);++tones;
  }else CHECK(out==(law?8:0));
  CHECK(tones<=408);
 }
 CHECK(tones==408 && s.stage==2 && !s.trn_frames && !consumed);
 CHECK(!memcmp(&paused,&s.encoder,sizeof(paused)));
 CHECK((s.rt_start-s.data_start)%6==0);
 for(unsigned i=0;i<1200;++i)v90_phase4_next(&s,0);
 CHECK(s.stage==2 && s.mp_ack && !s.ed_frame && !consumed);
 s.rx.cp.ack=1;receive_cp(&s);
 for(unsigned i=0;i<1200 && s.stage!=4;++i)v90_phase4_next(&s,0);
 CHECK(s.stage==4 && !consumed);
 for(unsigned i=0;i<400;++i)v90_phase4_next(&s,0);
 CHECK(consumed>0);
 return 0;
}
''')
    so=Path(tmp)/'test.so'
    sources=['v90training.c','v90pcm.c','v90cp.c','v90dil.c','v90upstream.c','v90trellis.c','v90qam8.c','v90equalizer.c','v90shell.c','v90mapping.c','v42detect.c']
    subprocess.run(['gcc','-shared','-fPIC','-O2','-Wall','-Werror','-I'+str(root/'vendor/linmodem'),str(wrapper),*[str(root/'vendor/linmodem'/s) for s in sources],'-lm','-o',str(so)],check=True)
    lib=C.CDLL(str(so));lib.run.argtypes=[C.c_int,C.c_uint,C.c_uint,C.c_uint]
    for law in [0,1]:
        for offset in range(6):
            for sr in range(4):
                for ld in range(4):
                    result=lib.run(law,offset,sr,ld)
                    assert result==0,(law,offset,sr,ld,'C assertion line',result)
    print('PASS: CPs-prime, aligned silence/Rt, coding-state continuity, resumed MP/data across 192 law/phase/shaping/lookahead combinations')
