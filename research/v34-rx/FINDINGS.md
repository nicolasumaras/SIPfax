# V.34 data-mode receive: what the live capture actually contains

Analysis of `test/fixtures/v34-captures/live-datamode-goodline-rx.s16`
(caller -> SIPfax receive direction, linmodem engine, MP negotiated `ca=16800 ac=9600`,
we advertise `h=0` i.e. no precoding). All measurements use the validated front end in
`rx_front.py` (0.58% EVM on clean synthetic 48-QAM).

## 1. The line is not the limit

| measurement | value |
|---|---|
| silence rms (idle gaps in the same capture) | 11 |
| data rms | 1743 |
| **path SNR** | **~44 dB** |
| symbol rate (cyclostationary envelope line) | 3428.4 Hz (confirms S=3429) |
| symbol-clock offset vs our 24000/7 grid | ~42 ppm (measured, correctable) |

44 dB easily supports 33.6k. Earlier claims in this project that the receive channel was
"~16-19 dB / additive-noise limited" were **wrong** - they were measuring our own broken
front end (see `rx_front.py` header for the two bugs).

## 2. The call fails at 19.4 s - the caller gives up

| window | kurtosis `E|s|^4/(E|s|^2)^2` | what it is |
|---|---|---|
| 11.5 - 19.2 s | 1.39 - 1.41 | wideband, aperiodic, QAM-like |
| after 19.4 s | **1.001** | **pure 1200 Hz tone** (14553x spectral peak) |

1200 Hz is the V.8/Phase-2 INFO carrier: the caller abandons the connection and returns to
retry. So the capture holds only **~8 s of real signal**, and every earlier measurement that
averaged over "12-38 s" was training equalizers on ~20 s of a single tone.

## 3. The 8 s of real signal has no recoverable constellation

Aperiodic (magnitude autocorrelation < 0.05 at every lag 20..2000, so it is *not* the caller
repeating MP frames). But no lattice model fits it:

- margin = EVM / (dmin/2), scale-free and comparable across constellation sizes.
  A correct model at 44 dB SNR should give margin << 0.1.
- Searched L = 16..256 on both the 4Z+1 grid (linmodem's mapper base) and the full odd
  lattice: **every model gives margin 0.85 - 1.02**.
- Adding a receiver-side modulo (derotate -> modulo -> slice, scanning scale and box size):
  **margin stays 0.786**.
- 0.786 is almost exactly the theoretical value (0.816) for points distributed *uniformly*
  with respect to the lattice.

## 4. The signature points at Tomlinson-Harashima precoding

| distribution | kurtosis |
|---|---|
| 48-point QAM, uniform over points | 1.337 |
| dense 960-point QAM | 1.333 |
| **uniform over a square** | **1.397** |
| **measured (real capture)** | **1.390** |

Uniform-over-a-square is the TH precoder signature: the precoder output `x(n) = y(n) - p(n)`
fills the modulo region rather than sitting on the constellation lattice.

**This also explains why every equalizer attempt floored at ~15%.** A precoded signal only
returns to the lattice *after* the channel is equalized, and the slicer must then apply the
modulo. Blind CMA / decision-directed adaptation against a plain lattice slicer cannot
converge on a precoded signal - which is exactly what we observed, repeatedly.

Open question: we advertise `h=0`, so the caller should *not* be precoding. Either our MP's
precoder-coefficient fields are being read as non-zero by the caller (a bit-layout bug in
`V34_send_MP`, worth auditing against Table 21 - note the h fields sit behind start bits),
or the caller precodes regardless.

## 5. Next steps

1. **Audit our transmitted MP h-field bit layout** against Table 21/V.34. If the caller reads
   garbage coefficients it will precode with them, which matches everything above.
2. **Train the equalizer on Phase-3 TRN** (known sequence, sent *before* precoding starts) and
   carry the trained taps into data mode, instead of blind CMA. This is what a real V.34
   receiver does and is the only way to converge on a precoded signal.
3. Then add the **modulo slicer** (V.34 9.6.2: `c(n)` components are integer multiples of
   `2w`, `w=1` for `b<56`) ahead of the trellis decoder.
4. Correct the ~42 ppm symbol-clock offset with a tracking loop (measured, currently
   uncompensated; harmless over 8 s, not over a long connection).
