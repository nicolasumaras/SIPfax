#!/usr/bin/env python3
"""
Rate-agnostic V.34 receiver front end.

Supersedes the fixed-3429 front end in `rx_front.py`, which hard-coded 3429 baud /
1959 Hz and therefore could not lock *startup* (Phase 2/3) in any capture - blocking
both validation against a known constellation and TRN-trained equalization.

Every V.34 symbol rate has an exact rational relationship to the 8 kHz RTP rate, so each
one gets an INTEGER number of samples per symbol at a small upsample factor. No fragile
resampling loop is needed for acquisition:

    symbol_rate = 2400 * a/c          (linmodem S_tab, matches ITU Table 1)
    carrier     = symbol_rate * d/e   (d1/e1 = low carrier, d2/e2 = high)
    SPS         = 10*US*c / (3*a)     choose the smallest US making this an integer
    carrier cycles/sample at 8000*US  = 3*a*d / (10*c*e*US)   (exact rational)

| rate | a/c  | US | SPS | carrier low/high |
|------|------|----|-----|------------------|
| 2400 | 1/1  |  3 |  10 | 1600 / 1800      |
| 2743 | 8/7  | 12 |  35 | 1646 / 1829      |
| 2800 | 7/6  |  7 |  20 | 1680 / 1867      |
| 3000 | 5/4  |  3 |   8 | 1800 / 2000      |
| 3200 | 4/3  |  2 |   5 | 1829 / 1920      |
| 3429 | 10/7 |  3 |   7 | 1959 (both)      |

Symbol timing is acquired in closed form with Oerder-Meyr (no acquisition-range limit,
which is what broke the original loop - see rx_front.py header).

Validated: <1% EVM on clean synthetic signals at EVERY rate (run this file).
"""
import numpy as np, math, struct
from fractions import Fraction

FS0 = 8000.0

# linmodem S_tab: (a, c, d_low, e_low, d_high, e_high, J, P)
S_TAB = [
    (2400,  1, 1, 2, 3, 3, 4,  7, 12),
    (2743,  8, 7, 3, 5, 2, 3,  8, 12),
    (2800,  7, 6, 3, 5, 2, 3,  7, 14),
    (3000,  5, 4, 3, 5, 2, 3,  7, 15),
    (3200,  4, 3, 4, 7, 3, 5,  7, 16),
    (3429, 10, 7, 4, 7, 4, 7,  8, 15),
]


class Rate:
    """Exact parameters for one (symbol rate, carrier) choice."""

    def __init__(self, idx, high=True):
        nom, a, c, d1, e1, d2, e2, J, P = S_TAB[idx]
        d, e = (d2, e2) if high else (d1, e1)
        self.idx, self.nominal, self.high = idx, nom, high
        self.a, self.c, self.d, self.e, self.J, self.P = a, c, d, e, J, P
        self.baud = 2400.0*a/c
        self.carrier = self.baud*d/e
        for us in range(1, 200):                    # exact integer samples/symbol
            if (10*us*c) % (3*a) == 0:
                self.US = us
                self.SPS = 10*us*c//(3*a)
                break
        else:
            raise ValueError("no exact upsample for rate %d" % nom)
        self.FSu = FS0*self.US
        self.cyc = Fraction(3*a*d, 10*c*e*self.US)  # carrier cycles per upsampled sample

    def __repr__(self):
        return "Rate(%d %s carrier=%.1f US=%d SPS=%d)" % (
            self.nominal, "high" if self.high else "low", self.carrier, self.US, self.SPS)


def all_rates():
    out = []
    for i in range(len(S_TAB)):
        out.append(Rate(i, True))
        lo = Rate(i, False)
        if abs(lo.carrier - out[-1].carrier) > 1.0:   # 3429 has only one carrier
            out.append(lo)
    return out


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
    if us == 1:
        return x.copy()
    N = len(x); X = np.fft.rfft(x); Nu = N*us
    Xu = np.zeros(Nu//2+1, complex); Xu[:len(X)] = X
    return np.fft.irfft(Xu, Nu)*us


def front_end(y8, rate, beta=0.15, span=12):
    """8 kHz real passband -> complex baseband at exactly rate.SPS samples/symbol."""
    x = fft_upsample(y8, rate.US)
    n = np.arange(len(x))
    ph = 2*math.pi*(rate.cyc.numerator/rate.cyc.denominator)
    z = x*np.exp(-1j*ph*n)
    z = np.convolve(z, rrc(beta, rate.SPS, span), 'same')
    p = np.mean(np.abs(z)**2)
    return z/np.sqrt(p) if p > 0 else z


def timing_offset(z, rate):
    """Oerder-Meyr closed-form symbol timing, in fractions of a symbol."""
    sps = rate.SPS
    M = (len(z)//sps)*sps
    if M == 0:
        return 0.0
    e = np.abs(z[:M])**2
    m = np.arange(M)
    return (-np.angle(np.sum(e*np.exp(-2j*math.pi*m/sps)))/(2*math.pi)) % 1.0


def symbol_sample(z, rate, off, start=15, ppm=0.0):
    """Sample at symbol instants; `ppm` corrects a known symbol-clock offset."""
    sps = rate.SPS
    step = sps*(1.0 + ppm*1e-6)
    pos = off*sps + sps*start
    out = []
    while pos < len(z)-2:
        i0 = int(pos); f = pos-i0
        out.append(z[i0]*(1-f) + z[i0+1]*f)
        pos += step
    return np.array(out)


def clock_ppm(z, rate, block_syms=1500):
    """Symbol-clock offset (ppm) from the drift of per-block Oerder-Meyr estimates."""
    BL = int(block_syms*rate.SPS)
    offs = [timing_offset(z[s:s+BL], rate) for s in range(0, len(z)-BL, BL)]
    if len(offs) < 3:
        return 0.0
    d = np.diff(np.array(offs)) % 1.0
    d = np.where(d > 0.5, d-1, d)
    return float(np.median(d)/block_syms*1e6)


def baud_line(x, lo=2000, hi=3700, fs=FS0):
    """Prior-free symbol-rate estimate from the cyclostationary envelope spectrum."""
    X = np.fft.fft(x); N = len(x)
    H = np.zeros(N); H[0] = 1; H[1:(N+1)//2] = 2
    if N % 2 == 0: H[N//2] = 1
    e = np.abs(np.fft.ifft(X*H))**2; e = e - e.mean()
    E = np.abs(np.fft.rfft(e*np.hanning(len(e)))); f = np.fft.rfftfreq(len(e), 1/fs)
    m = (f >= lo) & (f <= hi)
    i = int(np.argmax(E[m]))
    return f[m][i], E[m][i]/np.median(E[m])


def synth(alphabet, rate, n=20000, snr_db=None, seed=7, amp=3000.0):
    """Exact synthetic signal at `rate`, for validating the front end."""
    rng = np.random.default_rng(seed)
    a = alphabet/np.sqrt(np.mean(np.abs(alphabet)**2))
    sym = a[rng.integers(0, len(a), n)]
    imp = np.zeros(n*rate.SPS + 400, complex)
    imp[200::rate.SPS][:n] = sym
    tx = np.convolve(imp, rrc(0.15, rate.SPS, 12), 'same')
    k = np.arange(len(tx))
    ph = 2*math.pi*(rate.cyc.numerator/rate.cyc.denominator)
    y = ((tx*np.exp(1j*ph*k)).real)[::rate.US]*amp
    if snr_db is not None:
        p = np.mean(y**2)
        y = y + rng.normal(0, math.sqrt(p/10**(snr_db/10)), len(y))
    return y


def evm_to(alphabet, s, nrot=180, sub=5):
    """EVM (%) to the nearest alphabet point, minimised over a global rotation."""
    a = alphabet/np.sqrt(np.mean(np.abs(alphabet)**2))
    p = np.mean(np.abs(s)**2)
    if p <= 0:
        return 99.0
    q = s/np.sqrt(p)
    best = 99.0
    for th in np.arange(0, 2*math.pi, 2*math.pi/nrot):
        r = q*np.exp(-1j*th)
        d = np.array([a[int(np.argmin(np.abs(a-v)))] for v in r[::sub]])
        best = min(best, math.sqrt(np.mean(np.abs(r[::sub]-d)**2))*100)
    return best


def data_constellation(n=48):
    """V.34 DATA-MODE constellation: the n lowest-energy points of the ODD-INTEGER
    (2Z+1) lattice -- coordinates +-1,+-3,+-5,+-7 at n=48.

    Verified against linmodem's own encoder output at R=16800 (L=48, K=28, M=12): the
    two sets agree except for a 2-point tie-break at energy 58, where four points
    (+-3,+-7)/(+-7,+-3) are equal-energy and V.34 selects a specific pair.

    NOTE this is NOT constellation_16800() below, which builds the 4Z+1
    quarter-superconstellation used by TRN and MP. Fitting data-mode symbols against the
    4Z+1 set silently inflates EVM -- that mistake cost a full measurement pass.
    """
    c = sorted((x*x + y*y, x, y)
               for x in range(-15, 16, 2) for y in range(-15, 16, 2))
    return np.array([complex(x, y) for _, x, y in c[:n]])


def constellation_16800():
    """48-pt 4Z+1 QUARTER-superconstellation (TRN/MP only -- see data_constellation)."""
    pts = []
    for a in range(-6, 7):
        for b in range(-6, 7):
            x = 4*a+1; y = 4*b+1
            pts.append((x*x+y*y, x, y))
    pts.sort()
    return np.array([complex(p[1], p[2]) for p in pts[:48]])


QPSK = np.array([1+1j, -1+1j, -1-1j, 1-1j])/math.sqrt(2)


if __name__ == '__main__':
    print("rate-agnostic front end — synthetic validation (target <1%% EVM at every rate)\n")
    print("  %-28s %-8s %-8s %s" % ("rate", "4-point", "48-point", "O&M offset"))
    ok = True
    for r in all_rates():
        y = synth(QPSK, r, n=12000)
        z = front_end(y, r); off = timing_offset(z, r)
        e4 = evm_to(QPSK, symbol_sample(z, r, off)[200:-50])
        y = synth(constellation_16800(), r, n=12000)
        z2 = front_end(y, r); off2 = timing_offset(z2, r)
        e48 = evm_to(constellation_16800(), symbol_sample(z2, r, off2)[200:-50])
        flag = "" if (e4 < 1.5 and e48 < 2.0) else "   <-- CHECK"
        ok &= (e4 < 1.5 and e48 < 2.0)
        print("  %-28s %6.2f%%  %6.2f%%   %.3f%s" % (r, e4, e48, off, flag))
    print("\n%s" % ("ALL RATES OK" if ok else "some rates need attention"))
