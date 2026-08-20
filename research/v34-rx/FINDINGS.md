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

## 4. Tomlinson-Harashima precoding: HYPOTHESISED, THEN RULED OUT

| distribution | kurtosis |
|---|---|
| 48-point QAM, uniform over points | 1.337 |
| **uniform over a square** (TH precoder signature) | 1.397 |
| **measured (real capture)** | **1.390** |
| **synthetic 48-QAM degraded to ~13 dB** | **1.391** |

The measured kurtosis first looked like the TH precoder signature (uniform-over-a-square).
**That inference was wrong.** A plain 48-QAM degraded to ~13 dB reproduces *both* the
kurtosis (1.391 vs 1.390) *and* the EVM (15.1% vs 16.2%). The kurtosis coincidence was
noise inflation, not a precoder.

Independently, the MP transmit path was audited and is **correct**, which removes the
mechanism entirely:

- Encoder bit offsets tally to exactly 188 bits and match our CRC-validated decoder on
  every field: `type@18, rate_ca@20, rate_ac@24, trellis@29, ack@33, CRC@171`.
  The CRC landing at 171 pins every preceding field width - if an h field were mis-sized
  the CRC would land elsewhere and real modem frames would not verify (they do).
- h-field offsets: `h1r@52 h1i@69 h2r@86 h2i@103 h3r@120 h3i@137`.
- `s->h` is only ever `memset` to 0 and never assigned, so we genuinely transmit h=0.

**Conclusion: we do not request precoding and the caller is not precoding.**

## 5. Where it actually stands

Best converged result on the real 8 s, with everything fixed (validated front end,
Oerder-Meyr timing, 44.5 ppm drift correction, multi-pass equalizer run to convergence):

| configuration | EVM |
|---|---|
| plain 48-QAM slicer | 13.15% (converged, stable over 6 passes) |
| + compressive radial warp `u(1+a|u|^2)`, a=-0.16 | 9.54% |

So the signal behaves like a 48-QAM at only ~13 dB effective SNR **on a 44 dB line** - a
~30 dB gap that is NOT explained by timing, carrier, equalizer convergence, constellation
size, or precoding. The compressive warp helping (13.2% -> 9.5%, monotonic, and still
improving at the fold limit of the cubic model) suggests a genuine *saturating*
nonlinearity somewhere in the path, but that is not yet pinned down.

Two prior-free probes disagree about what the symbols are, so neither is trusted yet:
matched-filter-only gives kurtosis 1.39 (QAM-like, ~48 points), while a CMA-only
equalizer gives kurtosis 1.002 and 4 clusters (constant modulus) - the latter is
most likely CMA collapsing after repeated passes.

## 6. Next steps

1. **Capture slmodem's data-mode receive audio as a control.** slmodem decodes this exact
   path at 33.6, so running the same pipeline over its receive signal answers the question
   this analysis keeps tripping over: if the pipeline shows clean constellation structure
   there, the pipeline is sound and the linmodem call's signal is genuinely degraded; if it
   does not, the pipeline still has a defect. This is the missing control experiment.
2. Identify the compressive nonlinearity (level/clipping in the ATA or a companding
   mismatch) - inverting it is worth ~3.5 dB already.
3. Train the equalizer on Phase-3 TRN rather than blind CMA (blind adaptation on a dense
   constellation has been unreliable throughout this work).
4. Correct the measured 44.5 ppm symbol-clock offset with a tracking loop.
