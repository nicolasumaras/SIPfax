#!/usr/bin/env python3
"""Check digital training wire levels, scrambler and Jd CRC independently."""
import ctypes as C
from pathlib import Path
import subprocess,tempfile
root=Path(__file__).resolve().parents[2]
class Dil(C.Structure):
    _fields_=[('n',C.c_uint),('lsp',C.c_uint),('ltp',C.c_uint),('sp',C.c_uint8*128),('tp',C.c_uint8*128),('h',C.c_uint8*8),('reference',C.c_uint8*8),('ucodes',C.c_uint8*255)]
class Tx(C.Structure):
    _fields_=[('sample',C.c_uint),('scrambler',C.c_uint),('sign',C.c_uint),('alaw',C.c_int),('uinfo',C.c_int),('jd',C.c_uint8*72),('jd_end',C.c_uint),('dil_segment',C.c_uint),('dil_position',C.c_uint),('stage',C.c_uint),('stop_dil',C.c_uint),('dil',Dil)]
with tempfile.TemporaryDirectory() as tmp:
    so=Path(tmp)/'tx.so'
    subprocess.run(['gcc','-shared','-fPIC','-Wall','-Werror',str(root/'vendor/linmodem/v90train_tx.c'),'-o',str(so)],check=True)
    lib=C.CDLL(str(so));lib.v90_train_tx_init.argtypes=[C.POINTER(Tx),C.c_int,C.c_int]
    lib.v90_train_tx_next.argtypes=[C.POINTER(Tx)];lib.v90_train_tx_next.restype=C.c_int16
    for law,W,U,zero in [(0,7676,3772,0),(1,7808,3904,8)]:
        state=Tx();lib.v90_train_tx_init(C.byref(state),law,78)
        x=[lib.v90_train_tx_next(C.byref(state)) for _ in range(2472+144)]
        cycle=[W,zero,W,-W,-zero,-W]
        assert x[:384]==cycle*64
        assert x[384:432]==[-v for v in cycle]*8
        assert all(abs(v)==U for v in x[432:])
        signs=[int(v>0) for v in x[432:]]
        scrambled=signs[:2040]+[signs[j]^signs[j-1] for j in range(2040,len(signs))]
        decoded=[]
        for j,b in enumerate(scrambled):
            decoded.append(b^(scrambled[j-18] if j>=18 else 0)^(scrambled[j-23] if j>=23 else 0))
        assert decoded[:2040]==[1]*2040
        jd=decoded[2040:2112];assert decoded[2112:]==jd
        assert jd[:17]==[1]*17 and all(jd[j]==0 for j in [17,34,47,48,51,68,69,70,71])
        crc=0xffff
        for j in [*range(18,34),*range(35,51)]:
            top=(crc>>15)^jd[j];crc=(crc<<1)&0xffff
            if top:crc^=0x1021
        assert jd[52:68]==[(crc>>(15-j))&1 for j in range(16)]
    lib.v90_train_tx_end_jd.argtypes=[C.POINTER(Tx)]
    state=Tx();lib.v90_train_tx_init(C.byref(state),0,78)
    state.dil.n=2;state.dil.lsp=3;state.dil.ltp=2
    state.dil.sp[:3]=[1,0,1];state.dil.tp[:2]=[0,1]
    state.dil.ucodes[:2]=[0,78];state.dil.h[0]=0;state.dil.h[4]=1
    state.dil.reference[0]=78;state.dil.reference[4]=0
    x=[lib.v90_train_tx_next(C.byref(state)) for _ in range(2550)]
    lib.v90_train_tx_end_jd(C.byref(state));assert state.jd_end==2616
    x += [lib.v90_train_tx_next(C.byref(state)) for _ in range(2628-len(x))]
    sign=[int(v>0) for v in x[432:]]
    bits=sign[:2040]+[sign[j]^sign[j-1] for j in range(2040,len(sign))]
    decoded=[b^(bits[j-18] if j>=18 else 0)^(bits[j-23] if j>=23 else 0) for j,b in enumerate(bits)]
    assert decoded[-12:]==[0]*12
    cycle=[3772,0,3772,0,-3772,0]+[0,-3772,0,3772,0,3772]*2
    x=[lib.v90_train_tx_next(C.byref(state)) for _ in range(36)]
    assert x==cycle*2,(x,cycle)
    lib.v90_train_tx_next(C.byref(state));state.stop_dil=1
    for _ in range(5):lib.v90_train_tx_next(C.byref(state))
    assert state.stage==2 and lib.v90_train_tx_next(C.byref(state))==0
    print('PASS: both PCM laws, Sd/Sbar lengths, TRN1d GPC, Jd differential encoding/CRC')
