#!/usr/bin/env python3
"""
V.34 Phase-2 INFO decoder (10.1.2.3.1, Table 14).

INFO is binary DPSK at 600 bit/s: the point rotates 180 degrees for a 1 and 0 degrees for a
0. The answer modem transmits on a 2400 Hz carrier, the call modem on 1200 Hz. 8000/600 =
40/3, so upsampling by 3 gives exactly 40 samples per symbol and 2400 Hz is exactly 1/10
cycle per sample at 24 kHz - no fractional resampling anywhere.

INFO0 layout (Table 14, bit 0 first in time):
    0:3   fill 1111          4:11  frame sync 01110010
    12    2743 supported     13    2800 supported      14   3429 supported
    15/16 3000 low/high      17/18 3200 low/high       19   0 => 3429 disallowed
    20    can reduce power   21:23 max symbol-rate difference
    24    CME modem          25    supports 1664-point constellations
    26:27 clock source       28    INFO0 ack
    29:44 CRC               45:48  fill 1111
"""
import numpy as np, math

SYNC = [0, 1, 1, 1, 0, 0, 1, 0]     # bits 4:11, left-most first in time


def info_bits(x, carrier_hz, fs=8000.0):
    """8 kHz real audio -> DPSK bit stream at 600 bit/s."""
    n = len(x)
    X = np.fft.rfft(x); Xu = np.zeros(n*3//2+1, complex); Xu[:len(X)] = X
    up = np.fft.irfft(Xu, n*3)*3
    k = np.arange(len(up))
    z = up*np.exp(-2j*math.pi*(carrier_hz/24000.0)*k)
    w = 40                                    # samples per symbol at 24 kHz
    m = (len(z)//w)*w
    sym = z[:m].reshape(-1, w).sum(axis=1)    # integrate and dump
    d = sym[1:]*np.conj(sym[:-1])
    return (d.real < 0).astype(int)           # 180 deg rotation => 1


def find_info(bits, want=49):
    """Locate frame syncs and return the candidate frames."""
    out = []
    for i in range(len(bits)-want):
        if list(bits[i+4:i+12]) == SYNC and list(bits[i:i+4]) == [1, 1, 1, 1]:
            out.append((i, bits[i:i+want]))
    return out


def parse_info0(f):
    b = lambda i: int(f[i])
    return dict(
        r2743=b(12), r2800=b(13), r3429=b(14),
        r3000lo=b(15), r3000hi=b(16), r3200lo=b(17), r3200hi=b(18),
        allow3429=b(19), lowpower=b(20),
        maxdiff=b(21) | (b(22) << 1) | (b(23) << 2),
        cme=b(24), c1664=b(25),
        clock=b(26) | (b(27) << 1), ack=b(28))


def describe(d):
    rates = []
    if d['r2743']: rates.append("2743")
    if d['r2800']: rates.append("2800")
    if d['r3429']: rates.append("3429")
    if d['r3000lo'] or d['r3000hi']: rates.append("3000" + ("lo" if d['r3000lo'] else "") + ("hi" if d['r3000hi'] else ""))
    if d['r3200lo'] or d['r3200hi']: rates.append("3200" + ("lo" if d['r3200lo'] else "") + ("hi" if d['r3200hi'] else ""))
    return ("rates[%s] allow3429=%d 1664pt=%d lowpwr=%d maxdiff=%d cme=%d clk=%d ack=%d"
            % (",".join(rates) if rates else "-", d['allow3429'], d['c1664'],
               d['lowpower'], d['maxdiff'], d['cme'], d['clock'], d['ack']))


def scan(x, carrier_hz, t0, t1, fs=8000.0, label=""):
    """Scan a window for INFO frames; returns list of (time, parsed)."""
    found = []
    step = 0.25
    t = t0
    while t < t1:
        seg = x[int(t*fs):int((t+0.8)*fs)]
        if len(seg) > 2000 and np.sqrt(np.mean(seg**2)) > 200:
            for _, f in find_info(info_bits(seg, carrier_hz, fs)):
                found.append((t, parse_info0(f)))
        t += step
    return found
