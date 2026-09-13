#!/usr/bin/env python3
"""Replay real upstream PCM through the native streaming Ja receiver."""
import ctypes as C
from pathlib import Path
import subprocess,sys,tempfile
import numpy as np
root=Path(__file__).resolve().parents[2]
with tempfile.TemporaryDirectory() as tmp:
    wrapper=Path(tmp)/'wrapper.c';so=Path(tmp)/'test.so'
    wrapper.write_text('''#include <stdlib.h>
#include "v90training.h"
void *create(void) { V90Training *s=malloc(sizeof(*s));v90_training_init(s);return s; }
unsigned segments(V90Training *s) { return s->dil.n; }
''')
    subprocess.run(['gcc','-shared','-fPIC','-O2','-Wall','-Werror','-I'+str(root/'vendor/linmodem'),str(wrapper),str(root/'vendor/linmodem/v90training.c'),str(root/'vendor/linmodem/v90dil.c'),str(root/'vendor/linmodem/v90cp.c'),'-lm','-o',str(so)],check=True)
    lib=C.CDLL(str(so));lib.create.restype=C.c_void_p;lib.segments.argtypes=[C.c_void_p]
    lib.v90_training_receive.argtypes=[C.c_void_p,np.ctypeslib.ndpointer(dtype=np.int16,flags='C_CONTIGUOUS'),C.c_int]
    pcm=np.fromfile(sys.argv[1],dtype='<i2');state=lib.create();found=0
    for n in range(0,len(pcm),160):
        frame=pcm[n:n+160];found=lib.v90_training_receive(state,frame,len(frame))
        if found:break
    assert found,'No CRC-valid Ja in recording'
    assert lib.segments(state)==147
    print('PASS: streaming C receiver decodes actual 147-segment hardware Ja')
