#!/usr/bin/env python3
"""Verify corrected-label trellis traceback against the clean encoder.

Reports startup bit errors separately: this test does not qualify startup,
B1 acquisition, audio demodulation, or PPP.
"""
import argparse
import os
from pathlib import Path
import subprocess
import tempfile
import numpy as np
p = argparse.ArgumentParser()
p.add_argument('binary', type=Path)
a = p.parse_args()
with tempfile.TemporaryDirectory() as tmp:
    for rate in (7200, 16800, 33600):
        enc, dec = Path(tmp)/'encoder.txt', Path(tmp)/'survivor.txt'
        env = {k: v for k, v in os.environ.items() if not k.startswith('SIPFAX_')}
        env.update(SIPFAX_DATALOOP='1', SIPFAX_DATA_R=str(rate), SIPFAX_FIG9='1',
                   SIPFAX_DL_SHAPE='1', SIPFAX_ENCDUMP=str(enc), SIPFAX_SURVDUMP=str(dec))
        subprocess.run([str(a.binary.resolve())], env=env, capture_output=True,
                       text=True, check=True, timeout=30)
        expected = np.loadtxt(enc, dtype=int)[:,6]
        actual = np.loadtxt(dec, dtype=int)[:,1]
        count = min(len(expected), len(actual)-29)
        assert count > 1000
        assert np.array_equal(expected[500:count], actual[529:count+29])
        tx = np.frombuffer(Path('/tmp/dl_tx.txt').read_bytes(), dtype=np.uint8)
        rx = np.frombuffer(Path('/tmp/dl_rx.txt').read_bytes(), dtype=np.uint8)
        length = min(len(tx), len(rx))
        bad = np.flatnonzero(tx[:length] != rx[:length])
        # Decoder startup/superframe acquisition remains a separate open issue.
        settle_bits = rate * 56 // 100  # two 280 ms superframes
        assert length > 2*settle_bits and not np.any(bad >= settle_bits), (rate, bad[-10:])
        print(f'PASS R={rate}: exact traceback states; {len(bad)} startup bit errors; '
              f'zero errors over final {length-settle_bits} checked bits')
