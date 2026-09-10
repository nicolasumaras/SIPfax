#!/usr/bin/env python3
"""Exhaustive shell ordering oracle, independent of C rank arithmetic."""
import ctypes as C
from collections import Counter
import itertools
from pathlib import Path
import random
import subprocess
import tempfile
root=Path(__file__).resolve().parents[2]

# V.34 9.4: energy, left-half energy, right subtree before left subtree.
# Enumerate tuples directly; do not call the production encoder to define truth.
def ordering(r):
    return (sum(r),sum(r[:4]),sum(r[4:6]),r[6],r[4],sum(r[:2]),r[2],r[0])

with tempfile.TemporaryDirectory() as directory:
    d=Path(directory);w=d/'wrapper.c';so=d/'shell.so'
    legacy=(root/'vendor/linmodem/v34.c').read_text(errors='replace')
    legacy=legacy[legacy.index('static void index_to_rings('):legacy.index('/* return the K bit index') ]
    w.write_text('''#include <stdlib.h>
#include "v90shell.h"
void *create(unsigned m,unsigned k){
 V90Shell*s=malloc(sizeof(*s));if(!s)return NULL;
 if(!v90_shell_init(s,m,k)){free(s);return NULL;}return s;
}
void destroy(void*s){free(s);}
typedef struct {int M,g2_tab[137],g4_tab[137],z8_tab[138];} V34DSPState;
''' + legacy + '''
void legacy_encode(V34DSPState*s,int index,int*rings){index_to_rings(s,(int(*)[2])rings,index);}
''')
    subprocess.run(['gcc','-shared','-fPIC','-O2','-Wall','-Wextra','-Werror',
        '-I'+str(root/'vendor/linmodem'),str(w),
        str(root/'vendor/linmodem/v90shell.c'),'-o',str(so)],check=True)
    lib=C.CDLL(str(so));lib.create.argtypes=[C.c_uint,C.c_uint];lib.create.restype=C.c_void_p
    lib.destroy.argtypes=[C.c_void_p]
    lib.v90_shell_encode.argtypes=[C.c_void_p,C.c_uint32,C.POINTER(C.c_uint8)]
    lib.v90_shell_decode.argtypes=[C.c_void_p,C.POINTER(C.c_uint8),C.POINTER(C.c_uint32)]
    class Legacy(C.Structure):
        _fields_=[('M',C.c_int),('g2_tab',C.c_int*137),('g4_tab',C.c_int*137),('z8_tab',C.c_int*138)]
    lib.legacy_encode.argtypes=[C.POINTER(Legacy),C.c_int,C.POINTER(C.c_int)]
    for m,k in [(1,0),(2,6),(2,8),(3,12)]:
        s=lib.create(m,k);assert s
        oracle=sorted(itertools.product(range(m),repeat=8),key=ordering)
        legacy=Legacy();legacy.M=m
        for width,field in [(2,'g2_tab'),(4,'g4_tab')]:
            counts=Counter(map(sum,itertools.product(range(m),repeat=width)))
            for total,count in counts.items():getattr(legacy,field)[total]=count
        counts=Counter(map(sum,oracle))
        for total in range(8*(m-1)+1):legacy.z8_tab[total+1]=legacy.z8_tab[total]+counts[total]
        for index,rings in enumerate(oracle):
            value=C.c_uint32(0xdeadbeef);wire=(C.c_uint8*8)(*rings)
            accepted=lib.v90_shell_decode(s,wire,C.byref(value))
            assert bool(accepted)==(index<1<<k),(m,k,index,rings,accepted)
            if accepted:
                assert value.value==index,(m,k,index,value.value)
                out=(C.c_uint8*8)()
                assert lib.v90_shell_encode(s,index,out)
                assert tuple(out)==rings,(m,k,index,tuple(out),rings)
                old=(C.c_int*8)();lib.legacy_encode(C.byref(legacy),index,old)
                assert tuple(old)==rings,('legacy',m,k,index,tuple(old),rings)
            else:assert value.value==0xdeadbeef
        out=(C.c_uint8*8)(*([255]*8))
        assert not lib.v90_shell_encode(s,1<<k,out)
        assert list(out)==[255]*8
        invalid=(C.c_uint8*8)(m,0,0,0,0,0,0,0)
        assert not lib.v90_shell_decode(s,invalid,C.byref(C.c_uint32()))
        lib.destroy(s)
    for m,k in [(0,0),(19,0),(2,32),(1,1),(2,9)]:
        assert not lib.create(m,k),(m,k)
    # Expanded shell tables exceed 32-bit counts. Exercise the top index bit.
    s=lib.create(18,31);assert s
    rng=random.Random(9034)
    for index in [0,1,2**31-1,2**30]+[rng.randrange(2**31) for _ in range(2000)]:
        rings=(C.c_uint8*8)();value=C.c_uint32()
        assert lib.v90_shell_encode(s,index,rings)
        assert all(x<18 for x in rings)
        assert lib.v90_shell_decode(s,rings,C.byref(value)) and value.value==index
    lib.destroy(s)
print('PASS: exhaustive M=1/2/3 shell ordering and legacy transmitter equivalence, unused shells, bounds, expanded 31-bit indices')
