#!/usr/bin/env python3
"""Passive echo recovery: real PPP framing, timeout, peer/lifecycle guards."""
import ctypes as C
from pathlib import Path
import subprocess,tempfile
root=Path(__file__).resolve().parents[2]
def packet(code=9,ident=1,magic=0x12345678,ac=True,payload=b'',size=None):
    lcp=bytes([code,ident])+((8+len(payload)) if size is None else size).to_bytes(2,'big')+magic.to_bytes(4,'big')+payload
    data=(b'\xff\x03' if ac else b'')+b'\xc0\x21'+lcp
    # Polynomial division over the transmitted bit order, complemented FCS.
    r=0xffff
    for v in data:
        for k in range(8):
            carry=(r&1)^((v>>k)&1);r>>=1
            if carry:r^=0x8408
    return data+(r^0xffff).to_bytes(2,'little')
def wire(p):
    return b'\x7e'+b''.join(bytes([0x7d,b^32]) if b<32 or b in (125,126) else bytes([b]) for b in p)+b'\x7e'
with tempfile.TemporaryDirectory() as tmp:
    w=Path(tmp)/'test.c';so=Path(tmp)/'test.so'
    w.write_text('''#include <stdlib.h>
#include "v90echo.h"
void *create(void){V90Echo *s=malloc(sizeof(*s));v90_echo_init(s);return s;}
void destroy(void *s){free(s);}
void tx(V90Echo *s,const unsigned char *p,unsigned n,long long now){for(unsigned i=0;i<n;++i)v90_echo_tx(s,p[i],now);}
int armed(V90Echo *s){return s->armed;}
''')
    subprocess.run(['gcc','-shared','-fPIC','-O2','-Wall','-Werror','-I'+str(root/'vendor/linmodem'),str(w),str(root/'vendor/linmodem/v90echo.c'),'-o',str(so)],check=True)
    lib=C.CDLL(str(so));lib.create.restype=C.c_void_p
    lib.destroy.argtypes=[C.c_void_p];lib.armed.argtypes=[C.c_void_p]
    lib.tx.argtypes=lib.v90_echo_rx.argtypes=[C.c_void_p,C.c_char_p,C.c_uint,C.c_longlong]
    lib.v90_echo_due.argtypes=[C.c_void_p,C.c_longlong];lib.v90_echo_pause.argtypes=[C.c_void_p]
    def tx(s,t,**kw):
        p=wire(packet(**kw));lib.tx(s,p,len(p),round(t*8000))
    def rx(s,t,**kw):
        p=packet(code=10,magic=0x87654321,**kw);lib.v90_echo_rx(s,p,len(p),round(t*8000))
    def due(s,t):return lib.v90_echo_due(s,round(t*8000))
    def fresh():
        s=lib.create();tx(s,0);rx(s,1);assert lib.armed(s);return s
    # First learn support; no recovery for a peer that never answered echoes.
    s=lib.create();tx(s,0);tx(s,30);assert not due(s,80);lib.destroy(s)
    # Boundary and single-shot, then a valid new reply rearms it.
    for ac in (False,True):
        s=fresh();tx(s,10,ident=255,ac=ac);tx(s,40,ident=0,ac=ac)
        assert not due(s,49.999875);assert due(s,50);assert not due(s,90)
        rx(s,51,ident=0,ac=ac);tx(s,60,ident=2);tx(s,90,ident=3);assert due(s,100)
        lib.destroy(s)
    # Single/burst requests, even many, cannot imitate spaced missing echoes.
    s=fresh()
    for t in range(10):tx(s,10+t*.01)
    assert not due(s,100);lib.destroy(s)
    # Later requests must not postpone the established recovery deadline.
    s=fresh()
    for t in [10,20,30,40,50]:tx(s,t)
    assert due(s,50.01),'frequent requests postponed recovery'
    lib.destroy(s)
    # Same-ID retransmissions are allowed and still establish repeated loss.
    s=fresh();tx(s,10);tx(s,40);assert due(s,50);lib.destroy(s)
    # Correct replies stop detection; unsolicited, looped or stale replies don't.
    for kind in ['correct','wrong-id','loop','stale','crc','length','request']:
        s=fresh();tx(s,10,ident=2);tx(s,40,ident=3)
        if kind=='correct':rx(s,41,ident=3)
        elif kind=='wrong-id':rx(s,41,ident=7)
        else:
            p=packet(code=9 if kind=='request' else 10,ident=3,magic=0x12345678 if kind=='loop' else 0x87654321,size=7 if kind=='length' else None)
            if kind=='crc':p=p[:-1]+bytes([p[-1]^1])
            lib.v90_echo_rx(s,p,len(p),round((131 if kind=='stale' else 41)*8000))
        assert bool(due(s,150 if kind=='stale' else 50))==(kind!='correct'),kind
        lib.destroy(s)
    # Malformed/transient TX cannot count as a request, including frame overflow.
    for bad in [packet()[:-1],packet(size=99),packet(code=10),packet(payload=b'x'*4100)]:
        s=fresh();b=wire(bad);lib.tx(s,b,len(b),80000);lib.tx(s,b,len(b),320000)
        assert not due(s,100);lib.destroy(s)
    # Aborted escape, noise before opening flag, and chunked escaped payload.
    s=fresh();b=b'junk'+wire(packet(ident=4,payload=b'\x7e\x7d\x00'))
    for i in range(0,len(b),3):lib.tx(s,b[i:i+3],len(b[i:i+3]),80000)
    rx(s,11,ident=4,payload=b'\x7e\x7d\x00');assert not due(s,100)
    tx(s,120);lib.v90_echo_pause(s);tx(s,150);assert not due(s,200)
    tx(s,180);assert due(s,200);lib.v90_echo_pause(s)
    tx(s,220);tx(s,250);assert not due(s,300) # no retrain loop until valid reply
    rx(s,301);tx(s,310);tx(s,340);assert due(s,350);lib.destroy(s)
    print('PASS: echo framing/FCS, learned support, 40s deadline, identifiers/wrap, malformed/loop/stale guards, training pause and one-shot rearm')
