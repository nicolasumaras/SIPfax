#!/usr/bin/env python3
"""Exercise E after CP without CP-prime, including guards and MP boundary."""
import ctypes as C
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]

def message(kind=1, silence=0, corrupt=False):
    b = bytearray((root/'test/fixtures/v90-cpt-6417.bits').read_bytes())
    b[19], b[30], b[33] = kind, silence, 0
    register = 0xffff
    for j in range(18, 409):
        if j % 17 == 0:
            continue
        top = (register >> 15) ^ b[j]
        register = (register << 1) & 0xffff
        if top:
            register ^= 0x1021
    b[409:425] = bytes((register >> (15-j)) & 1 for j in range(16))
    if corrupt:
        b[52] ^= 1
    return b

def scramble(bits):
    # Independent GPA transmitter recurrence: y[n]=x[n]^y[n-5]^y[n-23].
    y = []
    for n, b in enumerate(bits):
        y.append(b ^ (y[n-5] if n >= 5 else 0) ^ (y[n-23] if n >= 23 else 0))
    return bytes(y)

with tempfile.TemporaryDirectory() as tmp:
    wrapper = Path(tmp)/'test.c'
    wrapper.write_text('''#include <stdlib.h>
#include "v90training.c"
#include "v90phase4.c"
int detect(const unsigned char *bits,unsigned n,int other_lane){
 V90Training s;v90_training_init(&s);s.cp_mode=1;
 for(unsigned i=0;i<n;++i)bit(&s,&s.lanes[other_lane && i>=428?1:0],bits[i]);
 return s.e_seen;
}
static void ignored_frame(void*p,const uint8_t*b,unsigned n){(void)p;(void)b;(void)n;}
int early_e(const unsigned char *cp,unsigned n){
 V90Phase4 s;v90_phase4_init(&s,0,78);
 if(v90_cp_parse(&s.cp,cp,n,0)!=1)return -1;
 if(v90_pcm_init(&s.encoder,&s.cp,training_bit,&s))return -2;
 s.stage=2;s.ed_frame=1;s.trn_start=0;s.mp_length=102;
 s.rx.e_seen=1;s.upstream.samples=100;s.upstream.frames=99;
 s.upstream.receive_frame=ignored_frame;s.upstream.opaque=&s;
 for(unsigned i=0;i<30;++i){
  v90_phase4_next(&s,0);
  if(s.upstream.samples!=i+1)return -3;
 }
 return s.stage==4 && s.upstream.require_b1 && !s.upstream.frames &&
        s.upstream.receive_frame==ignored_frame && s.upstream.opaque==&s;
}
int finish(const unsigned char *cp,unsigned n,int have_cp,int have_e){
 V90Phase4 s;v90_phase4_init(&s,0,78);
 if(v90_cp_parse(&s.cpt,cp,n,0)!=1)return -1;
 if(v90_pcm_init(&s.encoder,&s.cpt,training_bit,&s))return -2;
 s.have_cp=have_cp;s.rx.e_seen=have_e;s.mp_length=102;
 s.generated=340*(s.encoder.k+s.encoder.s);mp_build(&s);
 /* First boundary starts MP-prime; it may not skip that message for E. */
 for(unsigned i=0;i<102;++i){training_bit(&s);if(s.ed_frame)return -3;}
 training_bit(&s);
 return s.ed_frame!=0;
}
''')
    so = Path(tmp)/'test.so'
    sources = ['v90pcm.c', 'v90cp.c', 'v90dil.c', 'v90upstream.c','v90trellis.c','v90qam8.c','v90equalizer.c','v90shell.c','v90mapping.c']
    subprocess.run(['gcc', '-shared', '-fPIC', '-O2', '-Wall', '-Werror',
        '-I'+str(root/'vendor/linmodem'), str(wrapper),
        *[str(root/'vendor/linmodem'/n) for n in sources], '-lm', '-o', str(so)], check=True)
    lib = C.CDLL(str(so))
    lib.detect.argtypes = [C.c_char_p, C.c_uint, C.c_int]
    lib.finish.argtypes = [C.c_char_p, C.c_uint, C.c_int, C.c_int]
    lib.early_e.argtypes = [C.c_char_p, C.c_uint]
    fixture=(root/'test/fixtures/v90-cpt-6417.bits').read_bytes()
    assert lib.early_e(fixture,len(fixture)) == 1, 'early E receiver reset or stopped at Ed'

    for kind, silence, corrupt in [(1,0,False), (0,0,False), (1,1,False), (1,0,True)]:
        cp = message(kind, silence, corrupt)
        for ones in [19,20,30]:
            wire = scramble(cp + bytes([1]*ones))
            expected = kind == 1 and silence == 0 and not corrupt and ones >= 20
            assert bool(lib.detect(wire,len(wire),0)) == expected
    # A second timing hypothesis cannot inherit a valid CP from the first.
    wire = scramble(message()) + scramble([1]*100)
    assert not lib.detect(wire,len(wire),1)
    wire = scramble([1]*100)
    assert not lib.detect(wire,len(wire),0)
    cp = bytes(message(kind=0))
    for have_cp in [0,1]:
        for have_e in [0,1]:
            assert lib.finish(cp,len(cp),have_cp,have_e) == (have_cp and have_e)
    print('PASS: E without CP-prime, 20-bit threshold, CP CRC/type/lane guards, complete MP-prime before Ed')
