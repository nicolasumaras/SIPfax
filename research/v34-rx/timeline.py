#!/usr/bin/env python3
"""
Phase-4 event timeline: classify every window of both directions of a call.

Content comparison eliminated everything measurable in the frames, so what is left is the
DYNAMICS - when each stage starts, how long it is held, and how the two directions
interleave. This labels each window using the decoders built during this work:

    .    silence
    I    Phase-2 INFO (binary DPSK 600 bit/s)
    S    S / S-bar (constant +-90 deg per-symbol alternation)
    4    4-point TRN      6  16-point TRN
    m    MP frames, CRC valid (suffix a = acknowledge bit set)
    D    dense / data-mode constellation
    ?    signal present, unclassified
"""
import numpy as np, math, sys
from v34_front import (Rate, front_end, timing_offset, symbol_sample, clock_ppm, FS0)
from v34_rx import equalize
from mp_decode import trn_score, decode, mp_points, GPA, GPC
from info_decode import info_bits, find_info

R = Rate(5, True)


def classify(seg, poly, carrier_info):
    """Label one window of audio."""
    if len(seg) < 3000 or np.sqrt(np.mean(seg**2)) < 220:
        return '.'
    # INFO first: it is a different carrier and baud entirely
    try:
        if find_info(info_bits(seg, carrier_info)):
            return 'I'
    except Exception:
        pass
    z = front_end(seg, R)
    s = symbol_sample(z, R, timing_offset(z, R), start=6)
    if len(s) < 250:
        return '?'
    # S / S-bar: a constant +-90 deg rotation every symbol
    d = s[1:]*np.conj(s[:-1])
    frac90 = np.mean(np.abs(np.abs(np.degrees(np.angle(d))) - 90) < 20)
    if frac90 > 0.80:
        return 'S'
    kurt = np.mean(np.abs(s)**4)/np.mean(np.abs(s)**2)**2
    for six, ch in ((False, '4'), (True, '6')):
        try:
            _, se, _ = equalize(z, R, mp_points(six)[0], n_cma=2, n_dd=3)
        except Exception:
            continue
        se = se[250:]
        if len(se) < 250:
            continue
        if trn_score(se, six, poly)[0] > 0.90:
            return ch
        fr, _ = decode(se, poly, sixteen=six)
        if fr:
            return 'ma' if any(f['ack'] for f in fr) else 'm'
    return 'D' if kurt > 1.55 else '?'


def timeline(x, poly, carrier_info, t0, t1, w=0.5):
    out = []
    t = t0
    while t < t1:
        out.append(classify(x[int(t*FS0):int((t+w)*FS0)], poly, carrier_info))
        t += w
    return out


def render(rx, tx, t0, w, width=48):
    """Print the two directions aligned under a time ruler."""
    for i in range(0, max(len(rx), len(tx)), width):
        a = "".join(('a' if c == 'ma' else c[0]) for c in rx[i:i+width])
        b = "".join(('a' if c == 'ma' else c[0]) for c in tx[i:i+width])
        print("   t=%5.1fs  caller: %s" % (t0+i*w, a))
        print("             us    : %s" % b)
