#!/usr/bin/env python3
"""Known-waveform carrier estimation with independent constellation/noise samples."""
import sys
import ctypes as C
import cmath
import math
from pathlib import Path
import random
import subprocess
import tempfile
symbols=120 if "--120" in sys.argv else 128
root=Path(__file__).resolve().parents[2]
quarters=[1+1j,-3+1j,1-3j,-3-3j,1+5j,5+1j,-3+5j,5-3j,5+5j,-7+1j,1-7j,-7-3j,-3-7j,-7+5j,5-7j,1+9j,9+1j,-3+9j,9-3j,-7-7j,5+9j,9+5j,-11+1j,1-11j]
points=[z*(-1j)**q for z in quarters for q in range(4)]
with tempfile.TemporaryDirectory() as td:
    d=Path(td);w=d/'w.c';so=d/'x.so'
    w.write_text('''#include "v90qam8.c"
void fit(const unsigned char *labels,const double *re,const double *im,
         unsigned position,double *result) {
 V90Qam8Stream s={0};s.b1.position=position;s.b1.length=128;
 for(unsigned n=0;n<128;++n) {
  s.b1.labels[n]=labels[n];unsigned i=(position+n)%128;
  s.b1.re[i]=re[n];s.b1.im[i]=im[n];
 }
 b1_carrier(&s,&result[0],&result[1],&result[2]);
}
''')
    w.write_text(w.read_text().replace('128',str(symbols)))
    subprocess.run(['gcc','-O2','-Wall','-Wextra','-Werror','-shared','-fPIC','-I'+str(root/'vendor/linmodem'),str(w),*[str(root/'vendor/linmodem'/f) for f in ['v90trellis.c','v90shell.c','v90mapping.c','v90equalizer.c']],'-lm','-o',str(so)],check=True)
    lib=C.CDLL(str(so));lib.fit.argtypes=[C.POINTER(C.c_uint8),C.POINTER(C.c_double),C.POINTER(C.c_double),C.c_uint,C.POINTER(C.c_double)]
    def fit(labels,samples,position):
        out=(C.c_double*3)();lib.fit((C.c_uint8*symbols)(*labels),(C.c_double*symbols)(*[z.real for z in samples]),(C.c_double*symbols)(*[z.imag for z in samples]),position,out);return list(out)
    rng=random.Random(19340);errors=[]
    for trial in range(48):
        labels=[rng.randrange(96) for _ in range(symbols)]
        frequency=[-.018,-.009,-.001,0,.001,.009,.018][trial%7]
        gain=[.25,1,7][trial%3];phase=-2.7+trial*.119
        signal=[gain*points[label]*cmath.exp(1j*(phase+frequency*n)) for n,label in enumerate(labels)]
        estimated=fit(labels,signal,[0,17,symbols-1][trial%3])
        assert abs(estimated[2]-frequency)<2e-7,estimated
        assert abs(estimated[0]/gain-1)<1e-6
        assert abs(cmath.phase(cmath.exp(1j*(estimated[1]-phase))))<2e-5
        noisy=[z+gain*complex(rng.gauss(0,.35),rng.gauss(0,.35)) for z in signal]
        estimated=fit(labels,noisy,[0,17,symbols-1][trial%3]);errors.append(estimated[2]-frequency)
        assert abs(estimated[0]/gain-1)<.03
        assert abs(cmath.phase(cmath.exp(1j*(estimated[1]-phase))))<.05
    rmse=math.sqrt(sum(x*x for x in errors)/len(errors))
    assert rmse<.00016,rmse
print(f'PASS: carrier slope/gain/phase, circular B1 history, signed offsets and noisy frequency RMSE {rmse:.8f} rad/symbol')
