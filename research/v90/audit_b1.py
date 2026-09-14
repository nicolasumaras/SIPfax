#!/usr/bin/env python3
"""Offline upstream B1 correlation, restricted to 4800 bit/s at 3200 baud.

ITU-T V.34 (02/98) 8.1, Table 7 and 10.1.3.1: B1 is 128 symbols
of scrambled ones, with reset encoders and the last-frame inversion.
This diagnostic does not gate data, establish negotiated parameters, or
prove that a call passed its complete startup procedure. Input is private
8 kHz signed little-endian PCM; output contains timing/score metadata only.
"""
import argparse
import json
import sys
from pathlib import Path

import numpy as np


def template():
    """Clockwise four-point symbol labels; GPA scrambler starts at zero."""
    bits = []
    for i in range(192):
        bits.append(1 ^ (bits[i-5] if i >= 5 else 0)
                    ^ (bits[i-23] if i >= 23 else 0))
    state = previous = 0
    labels = []
    for pair in range(64):
        info, q0, q1 = bits[3*pair:3*pair+3]
        a = (previous + q0 + 2*q1) & 3
        # J=7 pattern 01110111111110: last frame has an inversion
        # on its first pair, and none on its 33rd pair.
        b = (a + 2*info + ((state & 1) ^ (pair == 0))) & 3
        labels.extend((a, b))
        previous = a
        y1 = ((a & 1) & ((b & 1) ^ 1)) ^ (a >> 1) ^ (b >> 1)
        y2, u = a & 1, state & 1
        state = (state >> 1) ^ y1 ^ (y2 << 1) ^ ((y2 ^ u) << 2) ^ (u << 3)
    return np.exp(-0.5j*np.pi*np.array(labels))


def scores(symbols):
    """Normalized correlation invariant to constant carrier phase and gain."""
    symbols = np.asarray(symbols, dtype=complex)
    if symbols.ndim != 1 or not np.all(np.isfinite(symbols)):
        raise ValueError('Expected a finite one-dimensional symbol sequence')
    if len(symbols) < 128:
        return np.array([], dtype=float)
    dot = np.correlate(symbols, template(), 'valid')
    energy = np.convolve(abs(symbols)**2, np.ones(128), 'valid')
    return np.clip(abs(dot)/np.sqrt(np.maximum(128*energy, 1e-20)), 0, 1)


def audit(pcm, threshold=0.85):
    if not 0 < threshold <= 1:
        raise ValueError('Threshold must be in (0, 1]')
    sys.path.insert(0, str(Path(__file__).resolve().parents[1]/'v34-rx'))
    from v34_front import Rate, front_end
    if len(pcm) < 320:
        return []
    rate = Rate(4, True)
    baseband = front_end(np.asarray(pcm, dtype=float), rate, beta=.1)
    candidates = []
    for phase in range(rate.SPS):
        correlation = scores(baseband[phase::rate.SPS])
        for i in np.flatnonzero(correlation >= threshold):
            candidates.append({'seconds': (int(i)*5+phase)/16000,
                               'correlation': float(correlation[i]),
                               'timing_phase': phase})
    # One event per 40 ms B1, keeping the strongest timing lane.
    events = []
    for candidate in sorted(candidates, key=lambda p: -p['correlation']):
        if all(abs(candidate['seconds']-p['seconds']) >= .04 for p in events):
            events.append(candidate)
    return sorted(events, key=lambda p: p['seconds'])


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('pcm', type=Path)
    parser.add_argument('--threshold', type=float, default=.85)
    args = parser.parse_args()
    print(json.dumps(audit(np.fromfile(args.pcm, dtype='<i2'), args.threshold), indent=2))
