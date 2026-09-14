#!/usr/bin/env python3
"""V.42 detection and E/NUL decline bits against independent wire patterns."""
from pathlib import Path
import ctypes as C,subprocess,tempfile
root=Path(__file__).resolve().parents[2]
class State(C.Structure):
    _fields_=[(k,C.c_uint) for k in ['uart_bits','byte','marks','gap','previous','run','seen']]
def word(value):return [0]+[(value>>i)&1 for i in range(8)]+[1]
def odp(gap1=8,gap2=8,reverse=False):
    values=[0x91,0x11]*2 if reverse else [0x11,0x91]*2
    return sum((word(v)+[1]*(gap1 if i%2==0 else gap2) for i,v in enumerate(values)),[])
with tempfile.TemporaryDirectory() as td:
    so=Path(td)/'test.so'
    subprocess.run(['gcc','-O2','-Wall','-Wextra','-Werror','-shared','-fPIC',str(root/'vendor/linmodem/v42detect.c'),'-o',str(so)],check=True)
    lib=C.CDLL(str(so));lib.v42_detect_bit.argtypes=[C.POINTER(State),C.c_uint];lib.v42_decline_bit.argtypes=[C.c_uint]
    def events(bits):
        s=State();return sum(lib.v42_detect_bit(C.byref(s),b) for b in bits)
    cases=0
    for a in range(8,17):
        for b in range(8,17):
            for reverse in [False,True]:
                assert events([1]*40+odp(a,b,reverse)*3)==1
                cases+=1
    for gap in [0,1,7,17,32]:
        assert events(odp(gap,gap)*4)==0;cases+=1
    assert events(sum((word(0x11)+[1]*8 for _ in range(20)),[]))==0;cases+=1
    valid=odp()
    # Corrupt each character bit of an isolated minimum-length detection sequence.
    for k in range(4):
        for j in range(10):
            bad=valid.copy();bad[k*18+j]^=1
            assert events(bad)==0,(k,j);cases+=1
    assert events(valid[:3*18])==0;cases+=1
    reference=[int(b) for b in '0101000101'+'1'*8+'0000000001'+'1'*8]*10
    assert [lib.v42_decline_bit(i) for i in range(360)]==reference
    assert lib.v42_decline_bit(360)==-1 and lib.v42_decline_bit(2**32-1)==-1
    s=State();before=bytes(s);assert lib.v42_detect_bit(C.byref(s),2)==0 and bytes(s)==before
    print('PASS:',cases,'ODP framing/parity/gap cases; exact 360-bit E/NUL reply; bounds')
