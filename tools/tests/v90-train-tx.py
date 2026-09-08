#!/usr/bin/env python3
"""Check digital training wire levels, scrambler and Jd CRC independently."""
import ctypes as C
from pathlib import Path
import subprocess,tempfile
root=Path(__file__).resolve().parents[2]
class Tx(C.Structure):
    _fields_=[('sample',C.c_uint),('scrambler',C.c_uint),('sign',C.c_uint),('alaw',C.c_int),('uinfo',C.c_int),('jd',C.c_uint8*72)]
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
    print('PASS: both PCM laws, Sd/Sbar lengths, TRN1d GPC, Jd differential encoding/CRC')
