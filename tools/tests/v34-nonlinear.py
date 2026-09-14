#!/usr/bin/env python3
"""Compare actual B1 encoder output to ITU-T V.34 (02/98) 9.7.

Uses supplied, independently measured reference energy and Decimal arithmetic.
Tests symbol projection and unchanged precoder history, not the live power
estimator, pulse shaping, receiver inverse, or hardware interoperability.
"""
import argparse
from decimal import Decimal, localcontext, ROUND_HALF_EVEN
import os
from pathlib import Path
import subprocess
import tempfile

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('binary', type=Path)
a = p.parse_args()
binary = a.binary.resolve()
checked = 0
with tempfile.TemporaryDirectory() as tmp:
    out = Path(tmp)/'symbols'
    def generate(options, norm=None):
        env = {k:v for k,v in os.environ.items() if not k.startswith('SIPFAX_')}
        env.update(SIPFAX_B1_REFERENCE=str(out), **options)
        if norm is not None:
            env['SIPFAX_B1_NL_MEAN'] = str(norm)
        subprocess.run([str(binary)], env=env, capture_output=True, check=True, timeout=10)
        pairs = [tuple(map(int,line.split())) for line in out.read_text().splitlines()]
        assert len(pairs) == 120
        return pairs
    for rate in (4800,12000,16800,33600):
        for shape in (0,1):
            for taps in ('0,0,0,0,0,0','4413,1769,-3741,423,2586,-446'):
                opts = dict(SIPFAX_B1_RATE=str(rate), SIPFAX_SHAPE=str(shape),
                            SIPFAX_B1_TRELLIS='64', SIPFAX_B1_H=taps)
                linear = generate(opts)
                # Q7 coordinates: energy in lattice units is divided by 128^2.
                # This is a test reference, not an estimate of live data power.
                energy = sum(x*x+y*y for x,y in linear)/len(linear)/16384
                for scale in (1,2):
                    mean = float(energy*scale)
                    actual = generate(opts,mean)
                    expected = []
                    with localcontext() as context:
                        context.prec = 50
                        denom = Decimal(str(mean))*16384
                        for x,y in linear:
                            z = Decimal(5)*(x*x+y*y)/(16*denom)
                            gain = 1+z/6+z*z/120
                            expected.append(tuple(int((gain*v).to_integral_value(rounding=ROUND_HALF_EVEN)) for v in (x,y)))
                    assert actual == expected, (rate,shape,taps,mean,next((n for n,(x,y) in enumerate(zip(actual,expected)) if x != y),None))
                    assert actual != linear, 'nonlinear option ignored'
                    # Former constant 5.3% gain must not satisfy the test.
                    flat = [(round(x*(1+.3125/6+.3125**2/120)),round(y*(1+.3125/6+.3125**2/120))) for x,y in linear]
                    if len({x*x+y*y for x,y in linear}) > 1:
                        assert actual != flat, 'constant-gain control passed'
                    checked += len(actual)
    for invalid in ('0','-1','nan','inf','1junk',''):
        env = {k:v for k,v in os.environ.items() if not k.startswith('SIPFAX_')}
        env.update(SIPFAX_B1_REFERENCE=str(out),SIPFAX_B1_NL_MEAN=invalid)
        assert subprocess.run([str(binary)],env=env,capture_output=True,timeout=10).returncode != 0
print(f'PASS: V34 9.7 projection for {checked} emitted B1 symbols; linear and constant-gain controls differ')
