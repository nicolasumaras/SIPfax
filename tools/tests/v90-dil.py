#!/usr/bin/env python3
"""Validate the native Ja parser against the hardware frame and corruptions."""
import ctypes as C
from pathlib import Path
import subprocess,tempfile
root=Path(__file__).resolve().parents[2]
class Dil(C.Structure):
    _fields_=[('n',C.c_uint),('lsp',C.c_uint),('ltp',C.c_uint),('sp',C.c_uint8*128),('tp',C.c_uint8*128),('h',C.c_uint8*8),('reference',C.c_uint8*8),('ucodes',C.c_uint8*255)]
with tempfile.TemporaryDirectory() as temp:
    so=Path(temp)/'parser.so'
    subprocess.run(['gcc','-shared','-fPIC','-Wall','-Wextra','-Werror',str(root/'vendor/linmodem/v90dil.c'),'-o',str(so)],check=True)
    lib=C.CDLL(str(so));lib.v90_dil_parse.argtypes=[C.POINTER(Dil),C.POINTER(C.c_uint8),C.c_uint,C.POINTER(C.c_uint)]
    data=(root/'test/fixtures/v90-ja-5983.bits').read_bytes()
    bits=(C.c_uint8*len(data)).from_buffer_copy(data);out=Dil();used=C.c_uint()
    assert lib.v90_dil_parse(C.byref(out),bits,len(bits),C.byref(used))==1
    assert used.value==1736 and (out.n,out.lsp,out.ltp)==(147,126,126)
    assert list(out.h)==[20]*6+[11]*2 and list(out.reference)==[78]*8
    expected=[]
    for k in range(118):
        expected.append(k)
        if k%4==3:expected.append(78)
    assert list(out.ucodes)[:out.n]==expected
    for n in range(len(bits)):
        assert lib.v90_dil_parse(C.byref(out),bits,n,C.byref(used))==0,n
    for k in range(len(bits)):
        bits[k]^=1
        assert lib.v90_dil_parse(C.byref(out),bits,len(bits),C.byref(used))!=1,k
        bits[k]^=1
    print('PASS: hardware Ja parameters, all truncations, all 1736 single-bit corruptions rejected')
