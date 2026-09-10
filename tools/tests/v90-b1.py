#!/usr/bin/env python3
"""B1 reset/scrambler checks and noisy, rotated acquisition controls."""
import importlib.util
from pathlib import Path
import numpy as np

root = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location('audit_b1', root/'research/v90/audit_b1.py')
m = importlib.util.module_from_spec(spec)
spec.loader.exec_module(m)
reference = m.template()
assert len(reference) == 128
labels = (-np.rint(np.angle(reference)/(np.pi/2)).astype(int)) % 4
bits = []
previous = 0
for a, b in labels.reshape(-1, 2):
    q = (int(a)-previous) & 3
    bits.extend((((int(b)-int(a)) & 3) >> 1, q & 1, q >> 1))
    previous = int(a)
# Independent receiver's GPA shift-register form, including first pair.
register = 0
for bit in bits:
    assert ((register >> 22) ^ bit) & 1 == 1
    register = (register << 1) & 0x7fffff
    if bit:
        register ^= 1 | (1 << 18)

rng = np.random.default_rng(9034)
noise = lambda n: rng.normal(size=n) + 1j*rng.normal(size=n)
signal = noise(1500)
signal[523:651] = 2.7*np.exp(1.13j)*reference + .08*noise(128)
score = m.scores(signal)
assert int(np.argmax(score)) == 523 and score[523] > .99
assert max(m.scores(noise(5000))) < .5
assert not np.any(m.scores(np.zeros(500)))
assert len(m.scores(reference[:-1])) == 0
assert m.scores(reference)[0] > .999999
assert m.scores(reference[::-1])[0] < .5
for bad in [[float('nan')]*128, [float('inf')]*128, [[1]*128]]:
    try:
        m.scores(bad)
    except ValueError:
        pass
    else:
        raise AssertionError('Invalid symbols accepted')
assert m.audit(np.zeros(1000)) == []
print('PASS: B1 frame/reset/GPA, phase/gain/noise correlation and negative controls')

# Exercise the production C detector against the independent Python template.
import ctypes as C
import subprocess
import tempfile
with tempfile.TemporaryDirectory() as td:
    d = Path(td)
    (d/'wrap.c').write_text('''#include <stdlib.h>
#include "v90upstream.c"
void *create(void){V90Upstream*s=malloc(sizeof(*s));v90_upstream_init(s);return s;}
void destroy(void*s){free(s);}
int feed(void*p,double re,double im){V90Upstream*s=p;b1_symbol(s,0,re,im);return s->b1_seen;}
int pcm(void*p,const int16_t*x,unsigned n){V90Upstream*s=p;for(unsigned i=0;i<n;i++)v90_upstream_receive(s,x[i]);return s->b1_seen;}
long detected(void*p){return ((V90Upstream*)p)->b1_sample;}
''')
    subprocess.run(['gcc','-O2','-Wall','-Werror','-shared','-fPIC',
                    '-I'+str(root/'vendor/linmodem'),str(d/'wrap.c'),
                    str(root/'vendor/linmodem/v90trellis.c'),str(root/'vendor/linmodem/v90qam8.c'),str(root/'vendor/linmodem/v90shell.c'),'-lm','-o',str(d/'b1.so')],check=True)
    lib=C.CDLL(str(d/'b1.so'));lib.create.restype=C.c_void_p
    lib.destroy.argtypes=[C.c_void_p]
    lib.feed.argtypes=[C.c_void_p,C.c_double,C.c_double]
    def feed(values):
        state=lib.create()
        try:
            for i,v in enumerate(values):
                if lib.feed(state,float(v.real),float(v.imag)):return i
            return None
        finally:lib.destroy(state)
    assert feed(signal) == 650
    assert feed(noise(5000)) is None
    assert feed(np.zeros(500)) is None
    assert feed(reference[:-1]) is None
    assert feed(np.r_[reference[:100],complex(float('nan'),0),reference]) == 228
    assert feed(reference[::-1]) is None
print('PASS: native bounded B1 detector, noisy acquisition and reset/rejection controls')
