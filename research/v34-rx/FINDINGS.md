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

## 7. Control experiment: slmodem reference capture (2026-08-20)

Captured slmodem's own receive audio on the same path minutes later (it connects at 33.6),
persisted as `test/fixtures/v34-captures/live-datamode-slmodem-ref-rx.s16` - 46.5 s with a
**36 s uninterrupted data segment** (vs linmodem's 8 s before the caller gives up).

Identical pipeline over both:

| | slmodem REF (works @33.6) | linmodem (fails) |
|---|---|---|
| path SNR (silence vs data) | 46.7 dB | 45.2 dB |
| symbol rate (cyclostationary line) | 3428.4 Hz | 3428.4 Hz |
| symbol-clock offset | +46.3 ppm | +44.5 ppm |
| **kurtosis** | **1.677** | **1.390** |
| data segment | 36 s, no dropout | 8 s, then 1200 Hz retry tone |

Conclusions:

- **The path is identical and excellent in both cases** (~46 dB), and the ~45 ppm symbol-clock
  offset is a property of the path/caller clock, not of linmodem - slmodem tracks it fine.
  So neither the line nor the clock explains linmodem's failure.
- **Kurtosis separates them exactly as the negotiated rates predict.** 1.677 is near-Gaussian,
  the signature of the large shell-shaped constellation used at 33.6k; 1.390 matches a small
  (~48-point) constellation. So when linmodem is the answer modem the caller really is
  sending 16800-class data, confirming the constellation model - the remaining ~30 dB gap
  between 46 dB path SNR and ~13 dB slicer SNR is still unexplained.

**Pipeline limitation found:** `front_end()` hard-codes 3429 baud / 1959 Hz carrier, so it
cannot lock *startup* in either capture - the cyclostationary line during Phase 2/3 reads
2000-2570 Hz, and 4-point EVM sits at the no-lock value (~44%) in every startup window of
both files. That blocks the obvious validation (lock the known 4-point TRN in real audio)
**and** it blocks TRN-trained equalization, which is what a real V.34 receiver does and what
section 6 already identified as the way off blind adaptation.

**Next: make the front end rate/carrier agnostic** (drive it from the negotiated S and carrier
rather than constants), then lock Phase-3 TRN in the slmodem reference. That single change
both validates the pipeline against a known constellation on real audio and unlocks
TRN-trained equalization for data mode.

## 8. Rate-agnostic front end + the control that explains everything (2026-08-20)

`v34_front.py` replaces the fixed-3429 front end. Every V.34 symbol rate has an exact
rational relation to 8 kHz, so each gets an integer samples-per-symbol at a small upsample
factor (`symbol_rate = 2400*a/c`, `carrier = symbol_rate*d/e` from linmodem's `S_tab`):

| rate | a/c | US | SPS | carrier low/high |
|---|---|---|---|---|
| 2400 | 1/1 | 3 | 10 | 1600 / 1800 |
| 2743 | 8/7 | 12 | 35 | 1646 / 1829 |
| 2800 | 7/6 | 7 | 20 | 1680 / 1867 |
| 3000 | 5/4 | 3 | 8 | 1800 / 2000 |
| 3200 | 4/3 | 2 | 5 | 1829 / 1920 |
| 3429 | 10/7 | 3 | 7 | 1959 (both) |

Validated **<0.61% EVM on clean synthetic at all 11 rate/carrier combinations**.

### The pipeline is sound - proven on real audio

Scanning startup across all rates, the linmodem capture **locks 4-point at 3.5-4.4% EVM
(~29 dB) at 2400 baud**, t=3.4-6.2 s. This is the validation that was impossible before:
a known constellation, in real audio, recovered cleanly. The old front end could never see
it because it hard-coded 3429.

### Why no data segment ever resolved

Scanning both data segments across all 11 rates x constellations L=4..128:

| segment | best margin (EVM / (dmin/2)) |
|---|---|
| linmodem data | 0.76 |
| **slmodem data (decodes at 33.6!)** | **0.85** |

**slmodem's data signal shows no lattice structure under this pipeline either** - and that
signal demonstrably decodes, since the call connects at 33.6 and carries data. So the
absence of structure is *expected* for a matched-filter + timing + slicer chain, and was
never evidence that linmodem's signal was degraded.

This retires the "~30 dB unexplained gap" from section 5. There is no anomaly: an
unequalized dispersive V.34 data signal simply does not sit on the lattice, and the ~13%
"EVM" was just the best fit of a lattice to an unequalized signal. Nothing about the line,
the constellation model, or the caller was wrong.

### What is actually required next

Data-mode demodulation needs **equalizer training on the known Phase-3 TRN**, then carrying
those taps into data mode - exactly what a real V.34 receiver does, and what blind CMA/DD
has failed at throughout this work. That is now reachable, because the front end can finally
lock TRN (3.5% EVM above). Sequence:

1. Locate the caller's Phase-3 TRN precisely and confirm its symbol rate (the 4-point lock
   above is at 2400 baud; the data segment's cyclostationary line reads 3428 Hz, so the
   Phase-3 -> data rate relationship must be pinned down before taps can be carried over).
2. Train the equalizer on TRN's known scrambled 4-point sequence (data-aided LMS/LS).
3. Carry the trained taps into data mode, add carrier/timing tracking and the 44.5 ppm clock
   correction, then slice against the 48-point constellation.
4. Only then judge the residual EVM - and only then is the trellis decoder meaningful.

## 9. TRN-trained equalization works - and the real blocker is now protocol, not DSP

`v34_rx.py` adds the trained equalizer (T/2 fractionally-spaced, CMA warm-up ->
decision-directed, multi-pass, with carrier PLL, AGC and the measured clock correction).

On the linmodem capture, against the *known* Phase-3/Phase-4 signals:

| region | 4-point EVM | 16-point EVM | clock |
|---|---|---|---|
| Phase-3 TRN (11.6-14.6 s) | **2.91%** | 22.46% | +45.2 ppm |
| Phase-4 MP (15.6-18.6 s) | **3.49%** | 22.36% | +42.6 ppm |

**~3% EVM is ~30 dB** - the receiver core works on real V.34 audio. The 16-point column
rules out a 16-point constellation: both are 4-point at 3429 baud.

### What this finally explains

Cross-referencing the call log timestamps, the linmodem capture decomposes as:

| capture time | content |
|---|---|
| 11 - 15 s | caller Phase 3 (S / PP / TRN / J) |
| 15 - 19 s | caller MP |
| 19.4 s + | 1200 Hz V.8 INFO carrier - the caller gives up |

**There is no data mode in the capture at all.** linmodem never completes Phase 4, so the
caller never enters data mode. Every earlier attempt to fit a 48-point data constellation
to "the data segment" was fitting a lattice to TRN and MP - which is exactly why no
constellation, rate, modulo, or warp ever fit, and why the "~30 dB gap" looked so strange.

### The blocker has moved

The DSP receiver core is built and validated end-to-end on real audio (~3% EVM). It is not
yet a *data-mode* decoder, but that is no longer the limiting factor:

- **To decode data we first need a capture that contains data**, which requires linmodem to
  complete Phase 4 (E / B1) so the caller actually enters data mode. That is protocol work
  in the C implementation, not DSP.
- The slmodem reference does reach data mode, but at 33.6 with precoding and a large
  shell-shaped constellation (kurtosis 1.68); decoding it additionally needs the precoder
  modulo and the full mapper.

Note: "descramble TRN to all ones" is an unreliable check here (~50% even on known-valid
TRN). Use EVM / SER to the nearest ideal point instead.
