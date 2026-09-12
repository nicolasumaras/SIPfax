#!/usr/bin/env python3
"""Verify corrected-label trellis traceback against the clean encoder.

Reports startup bit errors separately: this test does not qualify startup,
B1 acquisition, audio demodulation, or PPP.
"""
import argparse
import itertools
import os
from pathlib import Path
import subprocess
import tempfile
import numpy as np
p = argparse.ArgumentParser()
p.add_argument('binary', type=Path)
a = p.parse_args()
with tempfile.TemporaryDirectory() as tmp:
    for rate, trellis in itertools.product((7200, 16800, 33600),(16,32,64)):
        enc, dec = Path(tmp)/'encoder.txt', Path(tmp)/'survivor.txt'
        positions = Path(tmp)/'positions.txt'
        env = {k: v for k, v in os.environ.items() if not k.startswith('SIPFAX_')}
        env.update(SIPFAX_DATALOOP='1', SIPFAX_DATA_R=str(rate), SIPFAX_FIG9='1',
                   SIPFAX_DL_TRELLIS=str(trellis), SIPFAX_RX_BIT_POSITIONS=str(positions), SIPFAX_DL_SHAPE='1', SIPFAX_ENCDUMP=str(enc), SIPFAX_SURVDUMP=str(dec))
        subprocess.run([str(a.binary.resolve())], env=env, capture_output=True,
                       text=True, check=True, timeout=30)
        expected = np.loadtxt(enc, dtype=int)[:,6]
        actual = np.loadtxt(dec, dtype=int)[:,1]
        count = min(len(expected), len(actual)-29)
        assert count > 1000
        assert np.array_equal(expected[500:count], actual[529:count+29])
        tx = np.frombuffer(Path('/tmp/dl_tx.txt').read_bytes(), dtype=np.uint8)
        rx = np.frombuffer(Path('/tmp/dl_rx.txt').read_bytes(), dtype=np.uint8)
        at = np.loadtxt(positions, dtype=np.int64, ndmin=1)
        # The data-loop sink caps its saved bit file; the position trace continues.
        assert len(at) >= len(rx)
        at = at[:len(rx)]
        assert np.all(np.diff(at) > 0)
        assert at[-1] < len(tx)
        length = int(at[-1])+1
        bad = at[tx[at] != rx]
        # Decoder startup/superframe acquisition remains a separate open issue.
        settle_bits = rate * 56 // 100  # two 280 ms superframes
        # Suppression may erase invalid startup decisions, never settled data.
        stable = at[at >= settle_bits]
        assert np.array_equal(stable, np.arange(settle_bits, length)), 'settled bits were erased'
        assert length > 2*settle_bits and not np.any(bad >= settle_bits), (rate, bad[-10:])
        print(f'PASS R={rate} trellis={trellis}: exact traceback states; {len(bad)} startup bit errors; '
              f'{length-len(rx)} startup bits erased; zero errors over final {length-settle_bits} checked bits')
