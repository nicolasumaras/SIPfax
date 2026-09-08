#!/usr/bin/env python3
"""
Phase-1 open V.34 data-mode receiver core (prototype).

Purpose: build an open receiver that matches slmodem's data-mode quality, to
replace the closed slmodem blob. This is the DSP prototype; it will be ported to
linmodem C once the algorithm floor is understood.

Chain:  downconvert (carrier 1959.184 Hz) -> optional FFT upsample (kills the
        linear-interpolation loss at 2.33 samples/symbol) -> RRC matched filter
        -> fractionally-spaced FSE (T/2), CMA warmup then decision-directed LMS
        -> Gardner symbol-timing recovery (PI loop) -> 2nd-order DD carrier PLL + AGC.

Status (2026-08-03):
  - On live linmodem receive captures (ca=16800, h=0):
      * fixed-timing crude receiver:        ~16% EVM (16 dB)
      * + Gardner timing:                   ~13-14%
      * + 2x FFT upsample (interp fix):     ~11.6% EVM (18.7 dB), converged floor
  - The ~11.6% floor is consistent across good- and bad-line calls => it's a
    decision-directed convergence floor (blind CMA->DD can't reach the MMSE
    optimum on the dense 48-pt constellation), NOT the channel.
  - NEXT: trained equalizer (seed from the known startup TRN symbols) to break the
    DD floor and reveal the true channel SNR; then wire demap+trellis+descramble;
    then precoding (send real h in MP) for rates above 16800.

Captures: test/fixtures/v34-captures/live-datamode-goodline-rx.s16 (good-line 16800),
          live-phase4-complete-rx.s16 (startup->data), live-datamode-caller-rx.s16.
slmodem reference (33.6, precoded, dense ~900-pt Gaussian blob): captured separately.
"""
import numpy as np, struct, math

FS0 = 8000.0                      # RTP/G.711 sample rate
FC  = 24000.0/49.0*4.0            # 1959.184 Hz  (V.34 S=3429 high carrier)
BAUD = 24000.0/7.0               # 3428.571 symbols/s


def load_s16(path):
    d = open(path, 'rb').read()
    return np.array(struct.unpack('<%dh' % (len(d)//2), d), dtype=float)


def rrc(beta, sps, span):
    N = int(2*span*sps)+1
    t = (np.arange(N) - (N-1)/2)/sps
    h = np.zeros(N)
    for i, ti in enumerate(t):
        if abs(ti) < 1e-8:
            h[i] = 1 - beta + 4*beta/math.pi
        elif beta > 0 and abs(abs(ti) - 1/(4*beta)) < 1e-6:
            h[i] = (beta/math.sqrt(2))*((1+2/math.pi)*math.sin(math.pi/(4*beta))
                    + (1-2/math.pi)*math.cos(math.pi/(4*beta)))
        else:
            h[i] = (math.sin(math.pi*ti*(1-beta)) + 4*beta*ti*math.cos(math.pi*ti*(1+beta))) \
                   / (math.pi*ti*(1-(4*beta*ti)**2))
    return h/np.sqrt(np.sum(h**2))


def fft_upsample(x, us):
    """Exact sinc interpolation (band-limited) by integer factor `us`."""
    if us == 1:
        return x.copy()
    N = len(x); X = np.fft.rfft(x); Nu = N*us
    Xu = np.zeros(Nu//2+1, complex); Xu[:len(X)] = X
    return np.fft.irfft(Xu, Nu)*us


def constellation_16800():
    """48-pt V.34 data constellation on the (4a+1,4b+1) grid, 48 lowest-energy."""
    pts = []
    for a in range(-6, 7):
        for b in range(-6, 7):
            x = 4*a+1; y = 4*b+1
            pts.append((x*x+y*y, x, y))
    pts.sort()
    return np.array([[p[1], p[2]] for p in pts[:48]], dtype=float)


def receive(x, con, upsample=2, beta=0.15, NT=41, ncma=2000,
            mu_cma=2e-3, mu_dd=2e-3, kt=(4e-3, 2e-5), kc=(8e-3, 2e-4)):
    """Return (symbols, evm_trajectory%). con = complex/real Nx2 constellation."""
    if con.ndim == 2:
        con = con[:, 0] + 1j*con[:, 1]
    a = con/np.sqrt(np.mean(np.abs(con)**2))
    R2 = np.mean(np.abs(a)**4)/np.mean(np.abs(a)**2)

    x = fft_upsample(x, upsample); FS = FS0*upsample; SPS = FS/BAUD
    n = np.arange(len(x)); bb = x*np.exp(-2j*math.pi*FC*n/FS)
    bb = np.convolve(bb, rrc(beta, SPS, 10), 'same'); bb = bb/np.sqrt(np.mean(np.abs(bb)**2))

    w = np.zeros(NT, complex); w[NT//2] = 1.0; buf = np.zeros(NT, complex)
    tstep = SPS/2.0; tpos = NT*1.0; tau = 0.0; dtau = 0.0
    th = 0.0; fr = 0.0; g = 1.0
    out = []; half = []; cnt = 0; syms = []; evm_t = []

    def slice_to(v):
        d = np.abs(a - v); i = int(np.argmin(d)); return a[i], d[i]

    while tpos < len(bb) - tstep - 5:
        p = tpos + tau; i0 = int(p)
        if i0 < 0 or i0+1 >= len(bb):
            tpos += tstep; continue
        frac = p - i0; y = bb[i0]*(1-frac) + bb[i0+1]*frac
        buf = np.roll(buf, 1); buf[0] = y; cnt += 1
        o = np.dot(w, buf)
        if cnt % 2 == 0:                       # symbol instant
            k = len(syms); yp = o*g*np.exp(-1j*th)
            if k < ncma:
                w += mu_cma*o*(R2-abs(o)**2)*np.conj(buf)/(np.vdot(buf, buf).real+1e-2)
            else:
                d, _ = slice_to(yp)
                pe = np.angle(yp*np.conj(d)); fr += kc[1]*pe; th += fr + kc[0]*pe
                g *= (1 + 3e-4*(abs(d)-abs(yp)))
                er = (d*np.exp(1j*th)/g - o); w += mu_dd*er*np.conj(buf)/(np.vdot(buf, buf).real+1e-2)
            syms.append(yp); out.append(o)
            if len(out) >= 2 and len(half) >= 1:
                ted = ((out[-1]-out[-2])*np.conj(half[-1])).real
                dtau += kt[1]*ted; tau += dtau + kt[0]*ted; tau = max(-2, min(2, tau))
            if k >= ncma and k % 400 == 0 and k > ncma+400:
                seg = np.array(syms[-400:])
                e = np.mean([slice_to(v)[1]**2 for v in seg]); evm_t.append(math.sqrt(e)*100)
        else:
            half.append(o)
        tpos += tstep
    return np.array(syms), np.array(evm_t)


if __name__ == '__main__':
    import sys
    path = sys.argv[1] if len(sys.argv) > 1 else 'live-datamode-goodline-rx.s16'
    t0 = float(sys.argv[2]) if len(sys.argv) > 2 else 11.0
    t1 = float(sys.argv[3]) if len(sys.argv) > 3 else 59.0
    x = load_s16(path)[int(t0*FS0):int(t1*FS0)]
    s, evm = receive(x, constellation_16800())
    c = np.mean(evm[-8:]) if len(evm) >= 8 else float('nan')
    print("symbols=%d  converged DD-EVM=%.1f%%  (~%.1f dB)" % (len(s), c, -20*math.log10(c/100)))
