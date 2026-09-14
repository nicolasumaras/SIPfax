#!/usr/bin/env python3
"""Validate current-interval V34 mapping through clean synthetic channels.

Tests generated symbols, known-phase precoding, and automatic frame grouping.
Even prefix drops retain 4D pairing. This does not qualify nonlinear encoding,
odd-symbol pairing recovery, analog timing, noise tolerance, or hardware PPP.
"""
import argparse
import os
from pathlib import Path
import subprocess
import tempfile
import numpy as np

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('binary', type=Path)
a = parser.parse_args()
binary = a.binary.resolve()
taps = ('3277,0,0,0,0,0', '4413,1769,-3741,423,2586,-446')
with tempfile.TemporaryDirectory() as tmp:
    sync = Path(tmp)/'sync'
    pattern = '0111011111111010'
    sync.write_text(''.join(f'0 0 0 {int(pattern[(n%480)//30]) if n%30 == 0 else 0}\n'
                            for n in range(16000)))
    def run(rate, shape=1, h=None, oracle=False, drop=0, require_output=True, **options):
        env = {k:v for k,v in os.environ.items() if not k.startswith('SIPFAX_')}
        env.update(SIPFAX_DATALOOP='1', SIPFAX_DATA_R=str(rate),
                   SIPFAX_DL_SHAPE=str(shape), SIPFAX_DL_DROP=str(drop), SIPFAX_FIG9='1')
        if h:
            env.update(SIPFAX_DL_H=h, SIPFAX_DL_CHAN=h, SIPFAX_RX_PRECODE='1')
        if oracle:
            env.update(SIPFAX_HALF='2', SIPFAX_V0ORACLE=str(sync), SIPFAX_V0ALIGN='0')
        env.update(options)
        r = subprocess.run([str(binary)], env=env, capture_output=True, timeout=30)
        assert r.returncode == 0, r.stderr.decode(errors='replace')
        tx = np.frombuffer(Path('/tmp/dl_tx.txt').read_bytes(),dtype=np.uint8).astype(np.int16)-48
        rx = np.frombuffer(Path('/tmp/dl_rx.txt').read_bytes(),dtype=np.uint8).astype(np.int16)-48
        assert np.all((tx==0)|(tx==1)) and np.all((rx==0)|(rx==1))
        if require_output: assert len(rx)>50000
        return tx,rx

    for rate in (12000,16800,33600):
        for shape in (0,1):
            for h in taps:
                tx,rx = run(rate,shape,h,oracle=True)
                n=min(len(tx),len(rx));assert np.array_equal(tx[:n],rx[:n]), (rate,shape,h)
    print('PASS: 12 exact precoded clean-channel cases with specification-derived sync')

    # The former extra-C0 constraint and absent inverse must fail, rather than
    # being silently ignored by a harness that reports success from bit counts.
    for opts in ({'SIPFAX_FC':'1'}, {'SIPFAX_RX_PRECODE':'0'}):
        tx,rx=run(12000,h=taps[1],oracle=True,require_output=False,**opts)
        n=min(len(tx),len(rx));assert n<50000 or np.count_nonzero(tx[:n]!=rx[:n])>1000,opts
    print('PASS: extra-C0 and missing-inverse negative controls fail decoding')

    checked=0
    for h in (None,taps[1]):
        for rate in (12000,16800,21600,33600):
            for drop in (0,2,6,10,18,38):
                tx,rx=run(rate,h=h,drop=drop)
                begin=40000; window=2048; maxlag=5000
                assert len(tx)>begin+window+maxlag and len(rx)>begin+window
                scores=np.correlate(2*tx[begin:begin+window+maxlag]-1,
                                    2*rx[begin:begin+window]-1,'valid')
                lag=int(np.argmax(scores))
                assert scores[lag]==window and np.count_nonzero(scores==window)==1
                if drop: assert lag>0, 'prefix-drop option was ignored'
                n=min(len(rx),len(tx)-lag)-begin
                assert n>50000 and np.array_equal(rx[begin:begin+n],tx[begin+lag:begin+lag+n]),(h,rate,drop,lag)
                checked+=n
    print(f'PASS: 48 automatic frame-alignment cases; {checked} settled bits, zero errors')
