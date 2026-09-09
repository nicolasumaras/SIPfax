#!/usr/bin/env python3
"""PPP frames replayed by multiple timing lanes must be delivered once."""
import ctypes as C
from pathlib import Path
import subprocess,tempfile
root=Path(__file__).resolve().parents[2]
def wire(value):
    data=bytes([0x21,value]);crc=0xffff
    for b in data:
        crc^=b
        for _ in range(8):crc=(crc>>1)^(0x8408 if crc&1 else 0)
    data+=(crc^0xffff).to_bytes(2,'little')
    return b'\x7e'+b''.join(bytes([0x7d,b^0x20]) if b in (0x7d,0x7e) else bytes([b]) for b in data)+b'\x7e'
with tempfile.TemporaryDirectory() as td:
    d=Path(td);w=d/'wrapper.c';so=d/'test.so'
    w.write_text('''#include <stdlib.h>
#include "v90upstream.c"
void *create(void){V90Upstream*s=malloc(sizeof(*s));v90_upstream_init(s);return s;}
void feed(V90Upstream*s,long sample,const unsigned char*p,unsigned n){
 V90UpLane lane={0};lane.crc=0xffff;s->samples=sample;
 for(unsigned i=0;i<n;++i)byte(s,&lane,p[i]);
}
unsigned count(V90Upstream*s){return s->frames;}
void destroy(void*s){free(s);}
''')
    subprocess.run(['gcc','-O2','-Wall','-Werror','-shared','-fPIC','-I'+str(root/'vendor/linmodem'),str(w),str(root/'vendor/linmodem/v90trellis.c'),'-lm','-o',str(so)],check=True)
    lib=C.CDLL(str(so));lib.create.restype=C.c_void_p
    lib.feed.argtypes=[C.c_void_p,C.c_long,C.c_char_p,C.c_uint];lib.count.argtypes=[C.c_void_p];lib.destroy.argtypes=[C.c_void_p]
    s=lib.create()
    def feed(t,data):lib.feed(s,t,data,len(data))
    burst=b''.join(wire(i) for i in range(123))
    feed(100,burst);assert lib.count(s)==123
    for phase in range(1,10):feed(100+phase,burst)
    assert lib.count(s)==123,'replayed frames duplicated across timing candidates'
    # Identical frames outside the five-millisecond suppression window are real repeats.
    feed(140,wire(0));assert lib.count(s)==124
    for i in range(300):feed(200+i*41,wire(i%256))
    assert lib.count(s)==424,'history wrapping suppressed valid repeated traffic'
    bad=bytearray(wire(50));bad[1]^=1;feed(20000,bytes(bad));assert lib.count(s)==424
    lib.destroy(s)
    print('PASS: 123-frame replay across ten lanes, genuine repeats, history wrapping and corrupt FCS')
