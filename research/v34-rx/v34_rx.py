#!/usr/bin/env python3
"""
V.34 receiver core: front end + symbol-clock correction + trained equalizer.

Validated on REAL captured audio: the caller's Phase-3 TRN and Phase-4 MP resolve to
**~3% EVM (~30 dB)** on `live-datamode-goodline-rx.s16`, using nothing but this chain.

    front_end(rate)            exact integer samples/symbol, exact carrier  (v34_front)
    timing_offset()            Oerder-Meyr, closed form, no acquisition limit
    clock_ppm()                symbol-clock offset from the drift of per-block O&M
    equalize()                 T/2 fractionally-spaced, CMA warm-up -> decision-directed,
                               multi-pass, with carrier PLL + AGC

Why multi-pass: this is offline analysis, so the equalizer is run repeatedly over the same
audio, carrying taps between passes. That converges where a single forward pass does not.
A live receiver instead trains on TRN and carries the taps forward into data mode.

Measured on the linmodem capture (caller Phase 3 = 11.6-14.6 s, MP = 15.6-18.6 s, both
4-point at 3429 baud, clock offset ~+45 ppm):

    region        4-point EVM        16-point EVM
    TRN           2.9 - 3.1%         22.5%
    MP            3.5 - 4.9%         22.4%

The 16-point column is what rules out a 16-point constellation: the signals are 4-point.

## Status

The DSP receiver core is working and validated. It is NOT yet a data-mode decoder,
because **neither captured call contains any data mode**:

- linmodem capture: the caller runs Phase 3 (11-15 s) then MP (15-19 s), then gives up at
  19.4 s and reverts to the 1200 Hz V.8 INFO carrier. linmodem never completes Phase 4, so
  the caller never enters data mode. Every "48-QAM at 13 dB" measurement earlier in this
  work was a lattice being fitted to TRN/MP, which is why nothing ever fit.
- slmodem reference: does reach data mode at 33.6, but that signal is precoded and uses a
  large shell-shaped constellation (kurtosis 1.68), so decoding it needs the precoder
  modulo and the full mapper - not a target for this stage.

So the blocker is no longer DSP: it is that linmodem does not complete the handshake into
data mode. Getting a capture that actually contains data-mode symbols is the prerequisite
for the next DSP step.

Note: the "descramble TRN to all ones" check is unreliable here (it reads ~50% even on
known-valid TRN) - use EVM / SER to the nearest ideal point as the quality metric instead.
"""
import numpy as np, math
from v34_front import (FS0, Rate, all_rates, load_s16, front_end, timing_offset,
                       symbol_sample, clock_ppm, baud_line, evm_to, synth,
                       QPSK, constellation_16800)


def equalize(z, rate, alphabet, ppm=0.0, NT=25, n_cma=3, n_dd=4, mu=3e-3,
             kc=(5e-3, 1e-4), agc=2e-4, start=12, report=False):
    """T/2 fractionally-spaced equalizer: CMA warm-up then decision-directed, multi-pass.

    Returns (taps, symbols, evm_per_dd_pass). `alphabet` should be the constellation the
    training segment actually uses (4-point for TRN/MP).
    """
    al = alphabet/np.sqrt(np.mean(np.abs(alphabet)**2))
    R2 = np.mean(np.abs(al)**4)/np.mean(np.abs(al)**2)
    w = np.zeros(NT, complex); w[NT//2] = 1.0
    hist = []
    syms = np.array([])
    for p in range(n_cma + n_dd):
        dd = p >= n_cma
        step = rate.SPS*(1 + ppm*1e-6)/2.0
        pos = rate.SPS*start
        buf = np.zeros(NT, complex)
        cnt = 0; out = []; th = 0.0; frq = 0.0; g = 1.0; ev = []
        while pos < len(z)-2:
            i0 = int(pos); f = pos-i0
            yv = z[i0]*(1-f) + z[i0+1]*f
            buf = np.roll(buf, 1); buf[0] = yv
            cnt += 1
            o = np.dot(w, buf)
            if cnt % 2 == 0:                      # symbol instant
                yp = o*g*np.exp(-1j*th)
                nrm = np.vdot(buf, buf).real + 1e-2
                if not dd:
                    w += mu*o*(R2-abs(o)**2)*np.conj(buf)/nrm
                else:
                    d = al[int(np.argmin(np.abs(al-yp)))]
                    pe = np.angle(yp*np.conj(d))
                    frq += kc[1]*pe; th += frq + kc[0]*pe
                    g *= (1 + agc*(abs(d)-abs(yp)))
                    w += mu*(d*np.exp(1j*th)/g - o)*np.conj(buf)/nrm
                    k = len(out)
                    if k > 1200 and k % 250 == 0:
                        sg = np.array(out[-250:])
                        ev.append(math.sqrt(np.mean([np.min(np.abs(al-v))**2 for v in sg]))*100)
                out.append(yp)
            pos += step
        syms = np.array(out)
        if dd and ev:
            hist.append(float(np.median(ev)))
            if report:
                print("   DD pass %d: EVM=%.2f%%" % (len(hist), hist[-1]))
    return w, syms, hist


def demod_segment(x, t0, t1, rate, alphabet, **kw):
    """Convenience: slice audio, run the whole chain, return (symbols, evm_history, ppm)."""
    seg = x[int(t0*FS0):int(t1*FS0)]
    z = front_end(seg, rate)
    ppm = clock_ppm(z, rate, block_syms=1200)
    w, s, h = equalize(z, rate, alphabet, ppm=ppm, **kw)
    return s, h, ppm


if __name__ == '__main__':
    import os
    F = os.path.join(os.path.dirname(__file__), '..', '..',
                     'test', 'fixtures', 'v34-captures')
    cap = os.path.join(F, 'live-datamode-goodline-rx.s16')
    x = load_s16(cap)
    r = Rate(5, True)                     # 3429 baud, carrier 1959.18
    A16 = constellation_16800()[:16]
    print("V.34 receiver core on real captured audio (%s)\n" % os.path.basename(cap))
    print("  region            4-point EVM   16-point EVM   clock")
    for t0, t1, lbl in [(11.6, 14.6, 'Phase-3 TRN'), (15.6, 18.6, 'Phase-4 MP ')]:
        _, h4, ppm = demod_segment(x, t0, t1, r, QPSK)
        _, h16, _ = demod_segment(x, t0, t1, r, A16)
        print("  %s   %6.2f%%       %6.2f%%     %+5.1f ppm"
              % (lbl, h4[-1] if h4 else 99, h16[-1] if h16 else 99, ppm))
    print("\n  => both are 4-point at 3429 baud, recovered at ~3% EVM (~30 dB).")
    print("     Neither capture contains data mode; see module docstring.")
