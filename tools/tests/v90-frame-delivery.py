#!/usr/bin/env python3
"""PPP frames replayed by multiple timing lanes must be delivered once."""
import ctypes as C
from pathlib import Path
import subprocess,tempfile
root=Path(__file__).resolve().parents[2]
def encoded(data):
    crc=0xffff
    for b in data:
        crc^=b
        for _ in range(8):crc=(crc>>1)^(0x8408 if crc&1 else 0)
    data+=(crc^0xffff).to_bytes(2,'little')
    return b'\x7e'+b''.join(bytes([0x7d,b^0x20]) if b in (0x7d,0x7e) else bytes([b]) for b in data)+b'\x7e'
def wire(value):return encoded(bytes([0x21,value]))
with tempfile.TemporaryDirectory() as td:
    d=Path(td);w=d/'wrapper.c';so=d/'test.so'
    w.write_text('''#include <stdlib.h>
#include "v90upstream.c"
void *create(void){V90Upstream*s=malloc(sizeof(*s));v90_upstream_init(s);return s;}
void feed(V90Upstream*s,long sample,long source,const unsigned char*p,unsigned n){
 V90UpLane lane={0};lane.crc=0xffff;lane.source_sample=source;s->samples=sample;
 for(unsigned i=0;i<n;++i)byte(s,&lane,p[i]);
}
void gate(V90Upstream*s,int seen){s->require_b1=1;s->b1_seen=seen;}
unsigned count(V90Upstream*s){return s->frames;}
unsigned startup(V90Upstream*s){return s->lcp_seen;}
void destroy(void*s){free(s);}
''')
    subprocess.run(['gcc','-O2','-Wall','-Werror','-shared','-fPIC','-I'+str(root/'vendor/linmodem'),str(w),str(root/'vendor/linmodem/v90trellis.c'),str(root/'vendor/linmodem/v90qam8.c'),str(root/'vendor/linmodem/v90equalizer.c'),str(root/'vendor/linmodem/v90shell.c'),str(root/'vendor/linmodem/v90mapping.c'),str(root/'vendor/linmodem/v42detect.c'),str(root/'vendor/linmodem/v90odp.c'),'-lm','-o',str(so)],check=True)
    lib=C.CDLL(str(so));lib.create.restype=C.c_void_p
    lib.feed.argtypes=[C.c_void_p,C.c_long,C.c_long,C.c_char_p,C.c_uint];lib.count.argtypes=[C.c_void_p];lib.startup.argtypes=[C.c_void_p];lib.destroy.argtypes=[C.c_void_p]
    s=lib.create()
    lib.gate.argtypes=[C.c_void_p,C.c_int]
    def feed(t,data,source=None):lib.feed(s,t,t if source is None else source,data,len(data))
    lib.gate(s,0);feed(0,wire(1));assert lib.count(s)==0,'data passed before B1'
    lib.gate(s,1);feed(0,wire(1));assert lib.count(s)==1,'B1 did not release gate'
    lib.destroy(s);s=lib.create()
    burst=b''.join(wire(i) for i in range(123))
    feed(100,burst);assert lib.count(s)==123
    for phase in range(1,10):feed(100+phase,burst)
    assert lib.count(s)==123,'replayed frames duplicated across timing candidates'
    # Later candidate acquisition replays the same original audio interval.
    feed(10000,burst,source=100);assert lib.count(s)==123
    # Identical frames outside the five-millisecond suppression window are real repeats.
    feed(140,wire(0));assert lib.count(s)==124
    for i in range(300):feed(200+i*41,wire(i%256))
    assert lib.count(s)==424,'history wrapping suppressed valid repeated traffic'
    bad=bytearray(wire(50));bad[1]^=1;feed(20000,bytes(bad));assert lib.count(s)==424
    lib.destroy(s)
    # FCS alone is not evidence of initial PPP: do not suppress recovery.
    cases=[(b'\x5e\x08\x48',False), (b'\x21\x45',False),
           (b'\xc0\x21\x01\x01\x00\x04',False)]
    for code in range(1,5):
        header=b'\xff\x03\xc0\x21'+bytes([code,1])
        cases += [(header+b'\x00\x04',True),
                  (header+b'\x00\x04padding',True),
                  (header+b'\x00\x08\x01\x04\x05\xdc',True),
                  (header+b'\x00\x03',False),
                  (header+b'\x00\x08\x01\x04',False),
                  (header+b'\x00\x06\x01\x00',False),
                  (header+b'\x00\x05\x01',False)]
    for payload,expected in cases:
        s=lib.create();data=encoded(payload);feed(0,data)
        assert lib.count(s)==1,'FCS-valid frame must remain available to pppd'
        assert bool(lib.startup(s))==expected,(payload.hex(),expected)
        lib.destroy(s)
    print('PASS: 31 startup LCP recognition cases; malformed and compressed frames retain recovery')
    print('PASS: 123-frame replay across ten lanes, genuine repeats, history wrapping and corrupt FCS')
