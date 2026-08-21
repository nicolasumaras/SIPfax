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

## 10. Why linmodem never reaches E/B1 (2026-08-20)

### The observable

Over the whole call the caller sends **151 MP frames with ack=0 and CRC OK**, and **never a
single valid MP'** (all 59 ack=1 frames are CRC failures, i.e. misdecodes). In V.34 Phase 4 a
modem sets ack=1 only after it has correctly received the *other* side's MP. So:

> **The caller never successfully receives our MP.**

Without the caller's MP', linmodem never advances to E, never sends B1, and the caller times
out at 19.4 s and drops back to the 1200 Hz V.8 carrier. That is the whole failure.

### What we ruled out - our transmission is fine

Decoding **our own transmitted audio** (`live-linmodem-tx.s16`, extracted from the call's RTP)
with an MP decoder first validated on a synthetic MP (29/29 frames CRC OK):

- **653 of our MP frames decode with CRC OK.** They are well-formed, correctly scrambled with
  GPA, correctly differentially encoded and CW-rotated, CRC valid, and they show the expected
  progression `ack=0` early then `ack=1` (MP') from ~18 s.
- Our signal is clean: 1.7-2.9% EVM on our own 4-point transmission.
- Phase-4 pacing is adequate: our TX resumes at 14.24 s and the first MP frame is 2373 symbols
  later, comfortably above the S(128T)+Sbar(16T)+TRN(512T) = 656 symbol minimum.
- The caller demonstrably *can* demodulate us at 3429 baud - it completed Phase 3 against our
  signal and advanced to Phase 4.

So the modulation, framing, scrambling, CRC and timing of our MP are all correct.

### What is left: the MP *content* is fixed, not negotiated

`V34_send_MP` hard-codes its parameters:

| field | we send | the caller says |
|---|---|---|
| call-to-answer max rate | `12` = **28800** | ca = **16800** |
| answer-to-call max rate | `12` = **28800** | ac = **9600** |
| trellis | `0` = **16-state** | trel = **2** (64-state) |
| rate mask | `0x7ff8` (fixed) | - |
| precoder h(1..3) | 0 | - |

We advertise a fixed 28800/28800 with a 16-state trellis regardless of what Phase 2 and the
caller's own MP established. The caller is asking for 16800/9600 with a 64-state trellis. An MP
whose parameters are inconsistent with the negotiated capabilities is exactly the kind of frame
a peer will decline to acknowledge - and this matches the already-known open item in this
project ("our TX still sends fixed MP (28800 mask) not the negotiated rate").

### The fix to try

Populate the MP from the negotiated state instead of constants: mirror the caller's rates
(ca=16800 -> field 7, ac=9600 -> field 4), set trellis to match the caller's 64-state
selection, and make the rate mask consistent with the rates actually offered. This is a small,
contained change in `V34_send_MP`, and it is directly testable on a live call: success is the
caller emitting **MP' (ack=1) with CRC OK**, which is the gate to E / B1 / data mode.

Note the tooling now exists to verify our own transmission offline before spending a call:
extract the TX flow from the RTP pcap and decode it with the validated MP decoder.

## 11. MP-content hypothesis FALSIFIED (2026-08-20)

Section 10 proposed that the caller withheld its MP' because our MP advertised parameters
that contradicted its proposal. Two live tests settled it - the hypothesis is **wrong**.

**Test 1** (commit d65e1b1, send negotiated parameters). Result: still 152x `ack=0 crc=OK`,
no valid MP'. But decoding our own transmitted audio showed the fix had not actually taken
effect: we were emitting **ca=0, ac=0, empty mask** - *worse* than the original. Cause: the
MP decoder stores its results on `v34_rx` while `V34_send_MP` builds from `v34_tx`, and only
the p4_* *flags* were bridged in `V34_process`, never the values. (Same class of bug as the
old `J_received` split between the two structs - worth remembering as a pattern here.)

**Test 2** (commit 0880728, bridge the values + guard). Our transmission was then verified
correct from its own audio: **629 MP' frames carrying exactly `ca=16800 ac=9600 64-state
ack=1`**, CRC valid - precisely what the caller asked for. Result: **still 152x `ack=0
crc=OK` and not one valid MP'.**

So: we now send exactly the parameters the caller proposed, in valid frames, and it still
never acknowledges. **MP content is not the blocker.**

Both fixes are nonetheless correct and are kept: advertising negotiated parameters is right,
and the rx->tx bridging bug was real and would have corrupted any future use of those values.

### What is now known about the caller's silence

- Our MP frames are valid and correctly parameterised (verified by decoding our own audio).
- Our signal is clean (1.7-2.9% EVM) with ample Phase-4 preamble (2373 symbols vs 656 min).
- The caller *can* demodulate us - it completed Phase 3 against our signal and advanced.
- J signalling is self-consistent: we send `J4POINTS` and modulate MP at 4-point.

### Next step (offline, no call needed)

**Diff our Phase-4 transmission against slmodem's.** slmodem does get this caller to
acknowledge, and both TX directions are already captured:

- ours: the `192.168.1.31 ->` flow in the linmodem test pcaps
- slmodem's: the same flow in the slmodem reference pcap

Comparing the two Phase-4 streams - S / Sbar lengths, TRN duration, when MP starts relative
to the caller's, frame cadence, level and constellation - should show directly what we do
differently. That is a concrete, bounded comparison against a known-good reference, and it
needs no further live calls.

## 12. Offline pcap mining: what it settled and what it did not (2026-08-20)

With both directions of a working (slmodem) and a failing (linmodem) call persisted, the
Phase-4 choreography is directly comparable. Classifying each 0.3 s window by kurtosis
(`.`=silent, `4`=constant-modulus/4-point, `m`=16-point-ish, `D`=dense/data):

```
slmodem  (WORKS)   caller->us: mmmmmmmmDDDDDDDDDDDDDDDDDDDD...
                   us->caller: ...mmmmmmmDDDDDDDDDDDDDDDDDD...   both reach DATA ~15 s

linmodem (fails)   caller->us: mmmmmmmmmmmmmmmmmmmmmmmmmmD4444444444444
                   us->caller: ..........m44444444444444444444444444444   we stay 4-point
```

That looked like a clean answer - slmodem sends 16-point MP, we send 4-point - and it is
what motivated the `is_16states=1` test. **But the follow-up measurements do not support it,
and two of my own classifiers turned out to be invalid:**

- **Kurtosis is unreliable here.** It is only meaningful on a window containing a single
  signal type; the MP windows in these calls mix TRN, MP and (for slmodem) data, giving
  values from 1.39 to 7.99 that classify nothing.
- **The frame-period test is invalid.** MP frames repeat, so the symbol sequence should
  repeat every 94 symbols (4-point) or 47 (16-point). It does not - not even in our own
  transmission, which is known to repeat. The self-synchronising scrambler carries state
  across frames, so identical bits produce different symbols. No periodicity exists to find.

**The strongest evidence actually points the other way.** Our C MP decoder reads the
*caller's* MP at 2 bits/symbol (rotation dibits only) and gets CRC OK 152 times per call. A
16-point MP carries 4 bits/symbol, so a 2-bit decode could not produce a valid CRC. **The
caller's own MP is therefore 4-point** - which makes it unlikely that 4-point MP is what it
objects to.

### State

`mp_16point` now defaults to 0 (4-point, the configuration under which the caller reliably
sends us valid MP), with `SIPFAX_MP16=1` to try 16-point without a rebuild. The TRN/MP
decoupling is kept regardless: `is_16states` driving both `V34_send_TRN` and the MP/J
constellation was a real latent bug, and it is what made the 16-point test regress so badly
(our TRN went 16-point, the caller stopped training, and the MP exchange vanished entirely).

### Honest status after four live tests

The caller has never acknowledged our MP. Conclusively ruled out: MP frame content and
parameters, MP validity/CRC/scrambling (our own transmission decodes at 653 frames CRC OK),
signal quality (1.7-2.9% EVM), and Phase-4 preamble length (2373 symbols vs 656 minimum).
Not yet explained, and not resolvable from these captures alone: why a caller that sends us
valid 4-point MP will not acknowledge our valid 4-point MP.

The most promising remaining avenue is the part of Phase 4 not yet compared in detail - the
exact S / Sbar sequences we emit before TRN, and the E sequence - since those are what mark
the phase boundaries the caller's state machine keys on. Unlike constellation guesses, those
can be checked against the spec tables directly and verified offline from our own TX audio.

## 13. S / Sbar / E verified - also not the difference (2026-08-20)

Detecting S/Sbar directly in the transmitted audio (both are a constant +-90 deg per-symbol
alternation; the S->Sbar boundary is a 180 deg jump, which keeps the alternation intact, so
the pair shows up as one run of 128+16 = 144 symbols):

| | S+Sbar run | at |
|---|---|---|
| **ours** | **144 symbols (42.0 ms)** | start of our Phase-4 burst |
| **slmodem** | **145 symbols (42.3 ms)** | start of its Phase-4 burst |

Our Phase-4 preamble is structurally correct and matches the working reference. Source
agrees: `V34_send_S` emits 64x2 = 128 symbols alternating 0/-90 deg, `V34_send_Sinv` emits
8x2 = 16 symbols at 180/90 deg (the same alternation rotated 180 deg).

`V34_send_E` is never reached: the E state is entered only on `p4_mpp_rx || p4_e_rx`, and the
caller sends us neither. So E cannot be the cause - it is downstream of the gate we are
stuck at.

One structural difference is visible but not yet explained: **slmodem clears Phase 4 in about
2-3 seconds** (S/Sbar at ~12.2 s, in data by ~15 s), transmitting in several discrete bursts
with gaps, whereas we transmit one continuous burst from 15.1 s to the end of the call.

### Cumulative status - everything ruled out so far

| checked | verdict |
|---|---|
| MP frame content / negotiated parameters | correct (mirrors caller exactly); not the cause |
| MP validity, CRC, scrambling, differential coding | correct - our own TX decodes, 653 frames CRC OK |
| signal quality | clean, 1.7-2.9% EVM |
| Phase-4 preamble length | 2373 symbols vs 656 minimum |
| S / Sbar structure | 144 symbols, matches slmodem's 145 |
| MP constellation (4 vs 16 point) | caller's own MP is 4-point, so 4-point is acceptable |
| E sequence | unreachable - downstream of the gate |

The captures have now been mined for everything they can settle. What remains is protocol
work rather than signal analysis: walking V.34 section 11.4 (the Phase-4 state machine and
its timing rules) against linmodem's implementation, since the failure is that a caller which
sends us valid MP will not acknowledge ours - a state-machine or timing condition rather
than anything measurable in the waveform.

## 14. Walking V.34 section 11.4 against linmodem's Phase-4 state machine (2026-08-20)

Spec re-fetched from ITU (T-REC-V.34 02/98) and section 11.4 extracted. Note the text is
copyrighted, so it is not committed here - re-fetch from itu.int and extract pages 52-57.

### Structure: linmodem matches the spec

| spec (11.4.1.2, answer modem) | linmodem |
|---|---|
| transmit S for 128T | `V34_send_S` = 64x2 = 128 symbols |
| then S-bar for 16T | `V34_send_Sinv` = 8x2 = 16 symbols |
| then TRN, >=512T, <= 2000 ms + RTT | `V34_send_TRN` in 1024T chunks, 2-6 chunks = 0.6-1.8 s |
| then MP until the call modem's MP arrives | `STARTUP4_MP` -> on `p4_mp_rx` |
| then MP' until MP' or E received | `STARTUP4_MPP` -> on `p4_mpp_rx \|\| p4_e_rx` |
| then a single 20-bit E, then B1 | `V34_send_E` (20 bits), then `V34_DATA` |

The state sequence, the 128T/16T lengths and the 20-bit E are all correct.

### Deviation found: J' detection is stubbed

**11.4.1.2.1** requires the answer modem, after sending S, to *"condition its receiver to
detect sequence J' followed by signal TRN"*. And **11.4.2.2.1** makes it a timed condition:
if J' is not received within 100 ms plus a round trip delay of the S-to-S-bar transition, the
answer modem must fall back to INFOMARKSc / Tone B handling.

linmodem never detects J' at all:

```c
case V34_STARTUP4_WAIT_JP:   /* unused (J' handled by RX re-hunt) */
    s->state = V34_STARTUP4_SINV;
```

with `s->JP_received = 1` hard-coded in `V34_mod_init`. So we never anchor Phase 4 on the
caller's J', and instead infer the caller's TRN completion by a re-hunt heuristic. This is a
real spec deviation and it is the mechanism by which our MP could be mistimed relative to
what the caller expects, even though every individual MP frame is valid.

### A timing rule that turned out NOT to discriminate

**11.4.2.1.1**: if the call modem does not detect the S-to-S-bar transition within *600 ms
plus a round trip delay* from the start of sequence J, it transmits silence then
**INFOMARKSc** - which would neatly explain the 1200 Hz V.8 carrier we see the caller fall
back to. Measured from the captures:

| | caller's long burst starts | we answer | delay |
|---|---|---|---|
| linmodem (fails) | 11.05 s | 15.05 s | **4.00 s** |
| slmodem (works) | 9.90 s | 12.91 s | **3.01 s** |

Both exceed 600 ms, and slmodem still succeeds - so either this caller is lenient about the
deadline, or "start of sequence J" is not the burst boundary I measured against. We are ~1 s
slower than the working reference, which is suggestive but not conclusive. **Not a confirmed
root cause.**

### Where this leaves it

The best-supported remaining defect is the **stubbed J' detection**. It is a genuine
departure from 11.4.1.2.1, it is in exactly the part of Phase 4 that establishes the timing
relationship between the two modems, and it is consistent with the one thing every other
test has shown: our MP frames are individually perfect, yet the caller never acknowledges
them. Implementing real J' detection - and anchoring the TRN/MP transition on it rather than
on a re-hunt heuristic - is the next concrete piece of work.

## 15. J' detection works live - the acknowledgement is still withheld (2026-08-20)

`SIPFAX_JP=1` tested on a live call:

```
[srx] caller J detected at sym 10044 (phase 3, 29/32) -> J_received
[srx] caller J' detected at sym 10509 (phase 6)       -> Phase 4 anchored
[p4]  MP READ (CRC): ca=16800 ac=9600 trellis=64state ack=0
```

J' lands 465 symbols after J - matching the offline test exactly - so the detector behaves
identically on live audio and the section 11.4.1.2.1 deviation is genuinely closed.

**But the caller still never acknowledges:** 151x `ack=0 crc=OK`, no valid MP'.

### The complete picture after five live tests

Every element of the exchange is now verified correct on our side:

| element | status |
|---|---|
| Phase 2/3 startup, J detection | works (caller advances to Phase 4) |
| S / S-bar (128T / 16T) | correct, matches slmodem (144 vs 145 symbols) |
| J' detection and Phase-4 anchoring | **now implemented, works live** |
| Phase-4 TRN | 2373 symbols, well above the 512T minimum |
| our MP frames | valid - 653 decode with CRC OK from our own audio |
| our MP parameters | mirror the caller's proposal exactly (16800/9600/64-state) |
| our signal quality | 1.7-2.9% EVM |
| the caller's own MP | decodes for us, CRC OK, 151 frames per call |

So both modems complete the entire startup and exchange MP successfully in both directions.
The *only* missing step is the caller setting its acknowledge bit.

### What this narrows it to

Per 11.4.1.1.2 the call modem sets ack=1 "after receiving the answer modem's MP sequence".
Ours are demonstrably well-formed and on the wire for tens of seconds. Since content,
timing, framing, level and constellation have each been eliminated, what remains is
something about how the caller's *receiver* locks onto our MP stream - not about the frames
themselves. Candidates not yet tested:

1. **We may switch to MP' too early.** We send MP (ack=0) for only ~2 s before switching, on
   receiving the caller's MP - which is what 11.4.1.2.2 says to do, but if the caller has not
   yet synchronised to our MP stream it may never see an ack=0 frame at all. Holding MP
   (ack=0) for a fixed longer interval before honouring the switch is a cheap experiment.
2. **Scrambler polarity on our MP.** We scramble with GPA as the answer modem. If the caller
   expects the opposite assignment for MP specifically, every frame would descramble to
   noise for it while remaining self-consistent for us - which matches the symptom exactly
   (we can decode our own frames perfectly; the caller behaves as though it never saw them).

(2) is worth testing first: it is a one-line change, it is invisible to all of our own
verification (which uses the same polarity to encode and decode), and it would explain why
everything looks perfect from our side while the caller acts as if nothing arrived.

## 16. Six live tests, no acknowledgement - and what to do instead of a seventh guess

The MP-hold change worked exactly as designed. Decoding our own transmission:

```
ca=28800 ac=28800 16-state ack=0  x56   <- before the caller's MP is read (unavoidable)
ca=16800 ac=9600  64-state ack=0  x8    <- NEW: settled parameters, ack still 0
ca=16800 ac=9600  64-state ack=1  x644  <- MP'
```

The caller now receives eight MP frames carrying our real, negotiated parameters with the
acknowledge bit clear, exactly as intended - and still never acknowledges.

### Cumulative record

Six live tests. Every hypothesis tested has been eliminated, several of them by evidence
rather than by trying:

| hypothesis | outcome |
|---|---|
| MP parameters not negotiated | fixed; no change |
| rx->tx parameter bridging (real bug) | fixed; no change |
| 16-point MP like slmodem | regressed (TRN coupling), then unsupported |
| Phase-4 J' anchoring missing (real spec gap) | fixed, works live; no change |
| scrambler polarity | **disproved by inspection + cross-validation, no call spent** |
| MP parameters unstable across the ack flip | fixed; no change |

And these remain verified-correct: S/S-bar lengths, TRN duration, frame validity and CRC,
signal quality, and the caller's own MP decoding for us.

### Why I am stopping the guess-and-dial loop

Each test above changed one variable inferred from the spec or from indirect measurement.
That approach has now failed six times, which is itself information: the difference between
us and slmodem is not something I have been able to *infer*. It needs to be *observed*.

**The one high-information thing not yet done: decode slmodem's own MP frames and diff them
against ours, field by field.** My MP decoder finds no frames in slmodem's transmission,
which is unexplained and is itself the lead - either its MP is 16-point (4 bits/symbol,
which my decoder does not handle) or its MP sits somewhere I have not looked. Writing a
16-point MP decoder would settle the constellation question that section 12 left open *and*
expose any field-level difference, without a single further call.

That is a bounded piece of work against captures already in the repo, and unlike the last
six attempts it cannot come back "no change with no explanation" - either slmodem's frames
decode and can be compared, or the failure to decode localises the difference itself.

## 17. slmodem's MP decoded - the difference is observed, not inferred (2026-08-20)

`mp_decode.py` adds a 16-point MP decoder (4 bits/symbol: I1,I2 from the differential
rotation, Q1,Q2 from the base-point index), validated on synthetic MP at both sizes and
rotation-invariant across all four phases. Applied to the captured transmissions:

| field | **slmodem (acknowledged)** | **ours (never acknowledged)** |
|---|---|---|
| MP constellation | **16-point** | **4-point** |
| call-to-answer rate | 16800 | 16800 |
| answer-to-call rate | **16800** | **9600** |
| trellis | **0 = 16-state** | **2 = 64-state** |
| constellation shaping | **1 = expanded** | **0 = minimum** |
| rate mask | **0x3fff** | 0x3ffe |
| aux / asymmetric | 0 / 1 | 0 / 1 |

Both decode with valid CRC, so this is a direct field-by-field comparison of two real
transmissions - the observation the previous six live tests could not produce.

**The 16-point lead from section 12 was right after all.** It was dismissed because the
kurtosis classifier was unreliable and because the caller's *own* MP is 4-point (our decoder
reads it at 2 bits/symbol with valid CRC). Both of those remain true - but they do not imply
the *answer* modem may use 4-point. slmodem, which this caller acknowledges, sends 16-point.

A second difference is just as interesting: **slmodem advertises its own capabilities rather
than mirroring the caller's.** It answers the caller's `trel=2` request with `trel=0`, and
offers `ac=16800` where the caller asked for 9600. Our "mirror the caller exactly" behaviour
- added in section 10 and never independently justified - is not what the working peer does.

### Next

The 16-point MP path already exists and is decoupled from TRN (`mp_16point`, section 12), so
it can be enabled without the TRN regression that sank the first attempt. Given that six
single-variable tests produced nothing, the efficient move is to match the known-good peer
on all of these fields at once, then narrow down afterwards if it works.

## 18. The 16-point regression explained - and the blocker is finally well-defined (2026-08-20)

Testing `SIPFAX_MP_SLCOMPAT=1` live produced *no* MP exchange at all - the same signature as
the first 16-point attempt. Two measurements explain it completely.

**1. Our 16-point MP transmission is correct.** Decoding our own transmitted audio with the
new decoder: **86-97 frames per window, valid CRC**, carrying exactly slmodem's field values
(`ca=16800 ac=16800 trel=0 shape=1 mask=0x3fff`). So the 16-point modulation works and we
are now sending, field for field and constellation for constellation, the MP that this
caller acknowledges from slmodem.

**2. The caller follows our J.** Classifying the caller's Phase-4 signal:

| we signalled | caller's Phase-4 signal |
|---|---|
| J4POINTS | 4-point |
| **J16POINTS** | **16-point** (4pt-EVM 45% vs 16pt-EVM 9%) |

This is 10.1.3.3: *"The scrambled bits are mapped to a 4- or 16-point 2D constellation
depending on the signal J."* J governs the constellation for the link, so signalling
16-point makes the **caller's** TRN and MP 16-point too - and **our receiver only handles
4-point**. It therefore never detects the caller's TRN, never enters MP hunt, and never
reads its MP. Hence "TRN done (6 chunks)" (the timeout path) and zero MP frames.

### What this resolves

The regression was never evidence against 16-point MP. It was our own receiver being unable
to follow the caller into the constellation we had just asked it to use. Both 16-point
attempts failed for this reason, and the TRN/MP decoupling introduced earlier made it worse
by putting our J (16-point) at odds with our own TRN (4-point).

It also offers the first plausible explanation for the original symptom: slmodem, which this
caller acknowledges, signals 16-point. We signalled 4-point on every call where the caller
sent us valid MP - and it never acknowledged. The caller may simply not accept a 4-point MP
from an answer modem, even though it sends 4-point itself.

### The blocker, now precisely defined

To send the MP that gets acknowledged, the receiver has to follow. Required:

1. **16-point TRN detection** in the C receiver (currently 4-point only) - this is what
   breaks first and prevents MP hunt.
2. **16-point MP decoding** in the C receiver. The algorithm is already written and
   validated in `mp_decode.py`: 4 bits/symbol, I1/I2 from the differential rotation and
   Q1/Q2 from the base-point index, rotation-invariant.
3. Keep `is_16states` consistent for J, TRN and MP - they are one setting, not three. The
   decoupling added earlier should be reverted once the receiver can handle 16-point.

That is concrete, offline-verifiable work against captures already in the repo: this call's
capture contains a real 16-point caller TRN and MP to develop against.

## 19. TRN uses ABSOLUTE rotation, not differential - and that retires an old false lead

Starting the 16-point receiver turned up a convention error that has been distorting this
work for a long time. **10.1.3.6**: the TRN signal rotates its constellation point
*clockwise by In*90 degrees*, In = 2*I2n + I1n - an **absolute** rotation. **10.1.3.3**: J
and MP instead **accumulate**, Zn = In + Zn-1.

Decoding TRN with the differential convention gives ~0.50 ones and looks exactly like "no
lock". That is why the descramble-to-all-ones check was recorded as unreliable, "~50% even
on known-valid TRN", and abandoned in favour of EVM. It was not unreliable - it was being
applied with the wrong rotation convention.

With the absolute convention, on a real captured caller TRN:

| | ones-fraction |
|---|---|
| differential (what we were doing) | 0.32 - 0.51 (looks like noise) |
| **absolute (10.1.3.6) + GPC** | **0.998** |

So `mp_decode.trn_bits()` / `trn_score()` now give a reliable, prior-free TRN detector: a
correct decode returns ~1.0 ones and nothing else does. That is a much stronger validation
signal than EVM, and it will be the check for the C receiver's 16-point TRN work.

### Still open

The caller's 16-point region does not decode as TRN under either constellation (best ~0.51
ones, EVM 54%). Two candidates: our 16-point signal set may be wrong - we take the first
four points of linmodem's energy-sorted data constellation, whereas 10.1.3.6 specifies
"points 0-3 of the quarter-superconstellation of Figure 5", which has not been verified
against the figure - or that region is the caller's 16-point *MP* (differential) rather than
TRN. Resolving this is the next step, and the 0.998 control gives a trustworthy test for it.

## 20. Quarter-superconstellation settled; the caller's whole Phase 3-4 now decodes (2026-08-20)

**The constellation question is closed.** Page 19 defines the labelling: points are numbered
by increasing magnitude, ties broken by *greatest imaginary component first*, and the full
superconstellation is the union of four 90-degree rotations of the quarter. That makes the
quarter exactly the 4Z+1 grid (rotating it lands on the other three cosets), so:

| magnitude | point |
|---|---|
| 2 | (1,1) -> **0** |
| 10 | (-3,1) -> **1** (imag 1 > -3) |
| 10 | (1,-3) -> **2** |
| 18 | (-3,-3) -> **3** |

which is exactly what `mp_decode.BASE` already used, and what linmodem builds. **Our
16-point signal set was correct all along** - the earlier failures were sampling windows
that straddled signal transitions.

### The Python receiver now decodes the caller's entire startup

Scanning the 16-point call with the validated absolute-rotation TRN detector:

| time | content | score |
|---|---|---|
| 13-14 s | **4-point TRN** (Phase 3) | 1.00 |
| 16 s | **16-point TRN** (Phase 4) | 0.99, EVM 7% |
| 17.5-18.2 s | **16-point MP** | 54-77 CRC-valid frames |

The caller's Phase-4 MP reads `ca=16800 ac=9600 trel=2 shape=1 mask=0x3ffe` - **and ack=0**.

### What this means

Two separate facts, and it is worth keeping them apart:

1. **Our receiver is the reason the 16-point call stalls early.** The caller does reach
   Phase-4 TRN and MP and transmits perfectly valid 16-point frames; our C receiver is
   4-point only, so it sees none of it, never enters MP hunt, and never gets the chance to
   acknowledge. That is a definite, fixable defect with a validated reference implementation
   now sitting in `mp_decode.py`.
2. **The caller still does not acknowledge us**, even when we send the 16-point MP that
   matches slmodem field for field. So 16-point is necessary for our receiver to follow the
   conversation, but it is not on its own sufficient to earn the acknowledgement.

The honest reading is that these may be the same problem: we have never once been in a state
where we could *see* the caller's MP and acknowledge it while also sending an MP it accepts.
Porting 16-point TRN and MP to the C receiver is what makes that state reachable for the
first time, and it is the necessary next step regardless.

## 21. The receiver gate is open - and the problem is now provably elsewhere (2026-08-20)

Two live calls with the block receiver produced the first forward motion on this gate.

**Call 7** - `[p4blk] LIVE MP READ: 68 frames 16pt ca=16800 ac=9600 trel=2 ack=0`, followed
by `TX: caller MP in -> holding MP(ack=0)`. **The transmit state machine advanced for the
first time in seven calls.** But the block receiver read only once, and the caller sets its
acknowledge bit only *after* receiving our MP - so its MP' necessarily arrives later than
the first MP we decode. Reading once could never see it.

**Call 8** - after making the receiver re-read on a sliding window: **three MP reads across
the call** (68, 72, 62 frames, all 16-point), every one `ack=0`. So we now demonstrably
receive the caller's MP *continuously*, and it still never acknowledges.

### Everything measurable now matches the peer that this caller does acknowledge

Decoding our own transmission from the same call:

| | ours (never acknowledged) | slmodem (acknowledged) |
|---|---|---|
| MP frames decoded from our audio | **113-134 per window, CRC valid** | - |
| constellation | 16-point | 16-point |
| ca / ac | 16800 / 16800 | 16800 / 16800 |
| trellis / shaping | 0 / 1 | 0 / 1 |
| ack progression | 0 -> 1 (MP -> MP') | 0 -> 1 |
| transmit rms / peak | 2325 / 6908, no clipping | 2184 / 7932, no clipping |

Our MP is well-formed, carries slmodem's exact field values, progresses its acknowledge bit
correctly, and is transmitted at a comparable level. The receiver reads the caller
throughout. And the acknowledgement still does not come.

### What this eliminates, and what is left

This closes out the receiver as a suspect: the earlier symptom - "we never see the caller's
MP" - is fixed and demonstrably so. It also closes out MP frame content, constellation,
field values, acknowledge sequencing and transmit level, each by direct measurement against
a working reference rather than by inference.

What has never been examined is **Phase 2**. Our MP now advertises `ca=16800 ac=16800`,
16-state trellis and expanded shaping, but our INFO sequences - sent long before, and never
decoded or verified in this work - advertise whatever linmodem hard-codes there. A peer that
cross-checks the MP against the capabilities established during INFO would reject a
mismatch, and that is exactly the shape of the symptom: valid frames, correct parameters,
politely ignored. The INFO exchange is the last part of the startup never inspected, and the
captures needed to inspect it are already in the repo.

## 22. INFO compared - identical to slmodem. The startup is now fully eliminated. (2026-08-20)

`info_decode.py` decodes Phase-2 INFO: binary DPSK at 600 bit/s (10.1.2.3.1), answer modem
on 2400 Hz, call modem on 1200 Hz. 8000/600 = 40/3, so upsampling by 3 gives exactly 40
samples per symbol and 2400 Hz is exactly 1/10 cycle per sample at 24 kHz - no fractional
resampling. CRC verified (covered set bits 12:29 for INFO0, 12:50 for INFO1a).

**INFO0a** (Table 14), CRC-valid in both captures:

| | value |
|---|---|
| slmodem (acknowledged) | `rates[2743,2800,3429,3000lohi,3200lohi] allow3429=1 1664pt=1 lowpwr=1 maxdiff=0 cme=0 clk=0 ack=0` |
| linmodem (never acked) | **identical** |

**INFO1a** (Table 16 - the selected symbol rates and probing results), CRC-valid in both:

| | value |
|---|---|
| slmodem | `pwr=0+0 mdlen=0 hicarr=0 preemph=6 projrate=9 (21600) SR a->c=3429 SR c->a=3429` |
| linmodem | **identical** |

So the INFO hypothesis is dead, and with it the last unexamined stage of the startup.

### The full elimination list

Every stage of the startup has now been compared against a peer that this exact caller does
acknowledge, by direct measurement rather than inference:

| stage | verdict |
|---|---|
| INFO0a / INFO1a (Phase 2) | **identical to slmodem, CRC-valid** |
| S / S-bar (Phase 4 entry) | 144 symbols vs slmodem's 145 |
| J / J-prime | detected, Phase 4 anchored, spec-compliant |
| TRN (4-point and 16-point) | decoded, scores 0.99 |
| MP frame content and fields | identical to slmodem (16-point, 16800/16800, trellis 0, shaping 1) |
| MP validity | 113-134 CRC-valid frames per window from our own audio |
| acknowledge sequencing | 0 -> 1 progression correct |
| transmit level | rms 2325 vs slmodem 2184, no clipping |
| our receiver | reads the caller's MP continuously, three reads per call |

Everything we transmit matches the working peer everywhere it can be measured, and we now
receive the caller throughout. The acknowledgement still never comes.

### What is genuinely left

Nothing in the *content* of the exchange distinguishes us from slmodem any more, so the
remaining candidates are things content comparison cannot see:

1. **Timing and pacing** - when our MP starts relative to the caller's, how long each stage
   is held, the gaps between them. slmodem clears Phase 4 in 2-3 seconds; we hold MP for
   tens of seconds. Nothing measured so far captures the *dynamics*, only the values.
2. **Something outside the V.34 signal entirely** - the caller may be reacting to a property
   of the SIP/RTP path (jitter, packet timing, a codec detail) rather than to the modem
   signal. slmodem and linmodem run through the same path, but they drive it differently.

Both are dynamic rather than static properties, which is consistent with everything static
having now been eliminated.

## 23. Timing comparison: found the J/MP asymmetry, matched it, still no acknowledgement

Measuring the *dynamics* of the working call rather than its contents produced the sharpest
structural finding of this work:

**In the slmodem call the caller acknowledges 0.50 s after slmodem's MP begins** - and the
two directions use *different* constellations:

| | slmodem call | our call (before the fix) |
|---|---|---|
| answer modem's own MP | **16-point** | 16-point |
| caller's MP | **4-point** | **16-point** |

So slmodem sends **J4POINTS** - J tells the *caller* which constellation to use for **its**
transmissions - while independently choosing 16-point for its **own** MP. The two are
separate decisions. We had J following `mp_16point`, so requesting 16-point MP also pushed
the caller onto 16-point: the opposite of slmodem, and it forced the caller onto a
constellation the streaming receiver cannot read. (The block receiver was built to work
around a problem we had created.)

Fixed: J now follows `is_16states` (J4POINTS, caller stays 4-point) while `mp_16point`
governs only our own MP. Live call 9 confirms the prediction exactly - the caller switched
back to 4-point (`LIVE MP READ: 38 / 65 frames 4pt`) - and decoding our own transmission
confirms we sent 16-point MP throughout with slmodem's exact fields
(`ca=16800 ac=16800 trel=0 shape=1`, ack progressing 0 -> 1).

**The caller still never acknowledges.**

### Where this leaves the investigation

We now match the peer this caller acknowledges on every dimension that has been measurable:
INFO0a and INFO1a byte-identical, J4POINTS, 16-point own MP with identical field values,
correct acknowledge sequencing, comparable transmit level, and a receiver that reads the
caller's MP continuously throughout the call. The acknowledgement still does not come, and
in the working call it arrives within half a second.

That is a meaningful boundary: everything observable in the exchange has been matched
against a working reference, so whatever remains is not visible in the frames, the
constellations, the parameters, the levels or the sequence of stages. The most likely
remaining candidates are fine-grained timing within Phase 4 - our stages run several times
longer than slmodem's - or a property of how the audio reaches the caller that the two
engines drive differently. Both would require instrumenting the *process* rather than the
*content*, which is a different kind of investigation from everything done so far.

## 24. Phase-4 timeline: the caller is not ignoring us, it is TIMING OUT and restarting

`timeline.py` labels every 0.5 s window of both directions using the decoders built here
(`.` silence, `I` INFO, `S` S/S-bar, `4`/`6` TRN 4/16-point, `m` MP, `a` MP with ack=1,
`D` data). Comparing the two calls:

```
WORKING (slmodem)
   caller: ...DD?44?444aDDDDDDD          TRN -> MP ALREADY ack=1 -> DATA
   us    : 44??....D?6?IDDIDIDD

FAILING (linmodem, call 9)
   caller: ....DD4444?4?44?mmmm?mSSSSSSSSSSSSSSSSSS
   us    : ?II?........44?4m??a??aa??aa??a??a??aa??
                            ^^ we DO send MP' (ack=1)
```

Two things this makes visible that no content comparison could:

1. **We do send MP'** - the `a` windows confirm the acknowledge bit is set on the wire, so
   our side of the protocol is behaving.
2. **The caller does not ignore us - it TIMES OUT and restarts.** After roughly 3 s of MP
   with `ack=0` it switches to `SSSSSSSS`, i.e. S/S-bar, which is the start of Phase 4
   again. That is the recovery in **11.4.2.1.2**: *"If, after sending the J' sequence, the
   modem has not received the E sequence for the following timeout period, it shall
   initiate the retrain procedure... 2500 ms plus two round trip delays"*.

So the failure is a **deadlock against a deadline**. The caller is waiting for our **E**.
We only send E after seeing its MP' - and it only sets MP' after accepting our MP. Whatever
prevents it accepting our MP, the consequence is now precisely characterised: it waits
~2.5-3 s, never gets E, and restarts. Every call has been ending this way.

In the working call the caller's very first decoded MP already carries `ack=1`, meaning
slmodem's MP reached it *before* it began transmitting MP at all - well inside the deadline.
Our MP starts at essentially the same moment as the caller's, leaving only the remainder of
that ~3 s window.

This reframes the remaining question usefully. It is not "why is our MP rejected" in the
abstract - it is "why does the caller not accept an MP that arrives at that point in the
exchange", with a hard deadline attached. Getting our MP out substantially earlier - during
the caller's TRN rather than after it - is the obvious thing to try, and is a pacing change
rather than another content hypothesis.

## 25. Pacing fixed - our MP now precedes the caller's, and it still is not accepted

Call 10, after shortening Phase-4 TRN to one 1024T chunk and letting the block receiver
decode at ~1.5 s:

```
caller: I.....DD444444?444mmmmmmSSSSSSSSSSSS
us    : ??4II?........??m??aa??aa??I??aa?aI?
                        ^ our MP at t=16.0, inside the caller's TRN
                          the caller's own MP starts at t=17.0
```

The pacing change did exactly what it was meant to. Our MP is now on the wire **before the
caller starts transmitting MP at all** - the same ordering slmodem has, and the ordering
that in the working call results in the caller's very first MP already carrying ack=1.

The caller still sends `mmmmmm` with ack=0 for ~2.5 s and then restarts with `SSSS`.

### So ordering and timing are eliminated too

The elimination list is now complete across every dimension these captures can express:

| dimension | matched to slmodem? |
|---|---|
| INFO0a / INFO1a | identical, CRC-valid |
| J signalling | J4POINTS, caller responds 4-point as in the working call |
| our MP constellation | 16-point, as slmodem |
| our MP field values | identical (16800/16800, trellis 0, shaping 1) |
| our MP validity | 113-134 CRC-valid frames decoded from our own audio |
| acknowledge sequencing | 0 -> 1, MP -> MP' |
| transmit level | comparable, no clipping |
| **ordering** | **our MP now precedes the caller's, as slmodem's does** |
| our receiver | reads the caller's MP continuously |

A note on what "matched" can and cannot prove: our MP decodes with the same decoder that
decodes slmodem's MP, which does establish that our modulation conventions agree with
slmodem's rather than merely being self-consistent. That was the obvious blind spot and it
is closed.

### Honest conclusion

Ten calls, and every property that can be observed in these captures has been matched
against a peer the same caller acknowledges within half a second. The acknowledgement does
not come, and the caller's behaviour is a clean 2.5-3 s timeout followed by a Phase-4
restart (11.4.2.1.2).

What remains is by definition something these captures do not express. Candidates worth
considering, none of them testable with the current tooling:

- a property of the RTP delivery itself - packet pacing, jitter, or timestamp behaviour -
  rather than of the decoded audio, since both engines are compared only after the audio has
  been reassembled;
- an aspect of the caller's own implementation that keys off something outside the V.34
  signal;
- a detail of the 16-point MP that survives round-tripping through a decoder built from the
  same reading of the spec, and would only be exposed by a third implementation.

The productive next step is not another content or timing hypothesis. It would be to compare
the RTP streams themselves - packet timing, sizes, timestamps and jitter - between the two
engines, which is a layer nothing in this investigation has yet examined.

## 26. The RTP layer: we were starving our own transmitter

Every comparison so far had been made on **decoded audio** - the pcap payloads concatenated
back into a sample stream. That step throws away delivery timing, and delivery timing turned
out to be where the two engines differ.

Comparing the RTP stream we send against the working slmodem call:

| | slmodem (works) | linmodem (fails) |
|---|---|---|
| packets | 2329 over 45.8 s | 1678 over 33.5 s |
| payload | 160 B, all | 160 B, all |
| sequence gaps | 0 | 0 |
| RTP timestamp step | 160, always | 160, always |
| median inter-packet | 20.01 ms | 19.99 ms |
| **max inter-packet** | **27.7 ms** | **110.7 ms** |
| gaps > 40 ms | **0** | **17** |
| gaps > 100 ms | **0** | **5** |

Nothing is lost: sequence numbers and RTP timestamps are perfectly contiguous, which is
exactly why every content-level comparison kept coming back clean. The audio is all there.
What fails is *delivery* - the sender stalls and then emits a burst.

### The stalls are mine, and they are periodic

```
t= 16.79s   69.8 ms      t= 22.83s  110.7 ms
t= 17.81s   93.5 ms      t= 23.82s   97.6 ms
t= 18.83s  106.2 ms      ...        ~1.00 s apart, all the way out
```

They start at t=16.79 s - the instant our MP begins - and repeat at 1.00 s intervals, which
is precisely the block receiver's decode cadence (`p4since >= 8000`). And the linmodem calls
captured *before* the block receiver was wired live show max 23.8 ms and zero stalls.

So our own MP carrier had a ~100 ms hole punched in it, five packet-times wide, every
second, for its entire duration. A peer has no reason to acknowledge that.

The cause: the decode ran wholly inside the audio callback - front end, plus two
constellations x sixteen equaliser passes over 20000 samples. About 100 ms of arithmetic
against a 20 ms frame period. Wiring it live and then, in section 25, making it run *more
often* made it steadily worse.

### Fix: amortise

At most one bounded unit of work per call - the front end, or a single equaliser pass, or
the MP decode - so a full decode spans ~18 frames rather than blocking one. The taps were
already `static`; `p4_equalize` only needed its reset placed under caller control. 4-point
is now tried first, since that is what the caller actually uses.

Measured by `V34_p4step_test`, which drives the receiver in 160-sample frames exactly as the
audio callback does:

```
16-point caller audio: 2216 steps,  5 MP reads, worst 10.18 ms, mean 1.81 ms, 0 over 20 ms
4-point  caller audio: 2217 steps, 14 MP reads, worst 10.22 ms, mean 1.87 ms, 0 over 20 ms
```

Both still decode the caller correctly (ca=16800 ac=9600 trel=2), and *more often* than the
old once-a-second full decode - so the acknowledgement should be seen sooner, not later.
Worst-case step latency is now instrumented in the live path so this cannot regress quietly.

### What this does and does not explain

It does not by itself explain the missing acknowledgement: calls from before the block
receiver existed also failed, and they had clean RTP. So this is not the original root cause.

It is, however, a real defect that would independently prevent the handshake, and it had to
be cleared before any further diagnosis could mean anything.

The transferable lesson is the one that cost nine calls: **comparing decoded content silently
assumes delivery is equivalent.** `pcap_util.audio()` concatenates payloads in arrival order
and never looks at sequence numbers or timestamps, so a stalling sender and a clean one
produce byte-identical audio. When two implementations differ in behaviour but not in
content, check the layer the comparison discarded.

## 27. TRN length matched to slmodem - still no acknowledgement

Measured from the working capture: slmodem's Phase-4 carrier comes up at 11.6 s and its MP
is first decodable at 13.4 s, so the caller gets ~1.6-1.8 s of S/Sbar+TRN to train on -
and having trained, its very first MP frame is already ack=1. Our pacing change had cut our
TRN to 0.3 s. That produced a coherent failure matrix:

| call | TRN | MP promptness | delivery | result |
|---|---|---|---|---|
| slmodem | ~1.6 s | immediate | clean | ack=1 instantly |
| ours pre-pacing | 1.79 s ok | held ~3 s | clean | fail |
| call 10 | 0.3 s | immediate | stalls | fail |
| call 12 | 0.3 s | immediate | clean | fail |
| **call 13** | **1.5 s** | **immediate** | **clean (max 49.6 ms)** | **fail** |

Call 13 (`SIPFAX_P4_TRN_CHUNKS=5`) was the first call matching slmodem on every one of
those dimensions simultaneously. The caller still sent ack=0 for ~3 s and restarted:

```
caller: I.....DD444444?444mmmmmmSSSSSSSSSSSSSSSD
us    : ??4II?........?44m?aa??aa?Ia?Ia??aa??aa?
```

So TRN length joins the elimination list. Two observations survive as leads:

1. **Periodic `I` windows inside our MP run** (`aa?Ia?Ia?`) - INFO-carrier classification in
   the middle of what should be continuous MP. slmodem's MP region never shows this.
2. slmodem's Phase-4 TX has a very different texture under the same classifier (`D?6?I`),
   and only ONE MP frame decodes from its whole MP region with 0.6 s windows, while ours
   yields ~86 per window. Either its MP region is simply short, or its Phase-4 sequence
   contains structure (PP?) that ours lacks.

Both are now under systematic investigation (5-lens workflow: signal structure incl. PP
detection, V.34 11.4 spec audit of linmodem's TX path, caller MP field forensics, bit-level
MP cross-validation against slmodem, and the I-window anomaly).

Operational notes for this and the two preceding attempts: two calls were burned on arming
mistakes (an invented bridge path, then a slmodem env missing `SIPFAX_MODEM_ARGS=-P` - the
engine spawns but never enters pipe mode and the line answers with silence). The linmodem
env must be restored wholesale from `sipfax.env.linmodem-dev.1785688709`, and the engine
smoke-tested (`head -c 48000 /dev/zero | linmodem-lm -P` must print "pipe engine up" and
emit ~48k bytes) before asking for a call.

## 28. ROOT CAUSE: the caller's J commands a 16-point Phase 4, and linmodem discarded the bit

A 13-agent forensics workflow over the working and failing captures (5 analysis lenses,
every finding adversarially re-measured by an independent agent; 8 confirmed, 0 refuted)
found what 13 calls of single-hypothesis testing missed.

**The caller sends J = 0x0D91 (J16POINTS) in every call - working and failing alike.**
Per V.34 10.1.3.3 / Table 18, J "is used to request the constellation size to be used by
the remote modem for transmitting sequences TRN, MP, MP', and E during Phase 4". The
variant bit was measured directly: 77 consecutive J frames per call carry it, terminated
by the canonical 0xF991.

| | Phase-4 TRN | Phase-4 MP | caller's verdict |
|---|---|---|---|
| slmodem (working) | **16-point** (descramble-ones 0.999) | 16-point | ack=1 in 0.5 s |
| linmodem (failing) | **4-point** (0.997; trn16 at chance) | 16-point (env override) | never acks, restarts |

linmodem hard-coded `is_16states = 0` and its J detector correlated only against 0x0991
with a 28/32 threshold - the live log's `caller J detected (29/32)` is *precisely the
signature of a 0x0D91 signal scored against the 0x0991 pattern*. The variant bit was
silently discarded, and the caller - whose receiver its own J had configured for 16-point
from us - could never train on our 4-point TRN. It never read our MP at all (which is why
every content fix changed nothing), timed out per 11.4.2.1.2, and restarted.

The direction of J was pinned by **double dissociation**: we command J4POINTS and the
caller duly transmits its Phase 4 as 4-point in both captures; the caller commands J16 and
slmodem goes 16-point while linmodem stayed 4-point. An old code comment justified the
4-point TRN by "slmodem sends J4POINTS while transmitting its own MP as 16-point" - true,
and exactly the point: J commands the *other* side. Asymmetric constellations per
direction are normal and present in the working call.

Also confirmed, secondary: our Phase-4 burst opened with ~94 symbols (28 ms) of stale
queued J draining when the TX unmuted; slmodem opens with S within 1 ms.

Ruled out along the way (each by direct measurement): PP in Phase 4 (neither modem sends
it - it is Phase-3 only, and both send it there identically), S/Sbar structure, TRN
length/timing, TX power, envelope gaps, spectral defects, MP field/CRC/cadence/scrambler
differences (bit-identical to slmodem's), RTP delivery, and the "I windows" inside our MP
(equalizer-convergence variance in the classifier, not a signal property - they do not
reproduce across calls).

### The fix (linmodem 892112e)

1. **J variant vote**: one 32-bit snapshot at fire time measured a dead tie (29/29) on
   real audio, so the detector now votes - 96 further symbols predicted against both
   patterns, decision by totals. Both real caller captures: **J4=180, J16=192/192** - a
   perfect score with exactly the 12 variant-position misses a J16 signal must show
   against the J4 pattern.
2. **One constellation flag**: rx_j16 bridges to TX and sets is_16states + mp_16point
   together - TRN, MP, MP' and E all follow the caller's command. `SIPFAX_J16_OBEY=0`
   reverts.
3. **Our J decoupled** from is_16states (it commands the caller, which must stay 4-point
   for our receiver); srx_rx16() now keys on what our J commanded, fixing a latent
   wrong-radius bug in the streaming tracker.
4. **Stale-J flush**: the TX mute extends by exactly the queued residue so S is the first
   thing on the wire.

Validated offline before any call: 16-point TRN encodes at 0.998 descramble-ones (raw
decode of our own synthetic TX; the earlier 0.671 was my equalizer failing to converge on
900 symbols of 16-point - the exact artifact the verifiers warned about), 172 CRC-valid
16-point MP frames; 4-point regression clean (0.994 / 90 frames); block receiver
regression clean (worst step 10.7 ms, zero over 20 ms).

## 29. Calls 14-17: the J16 chain works end to end - and the caller still never acks

Implementing the J16 obedience took four calls, each exposing one more defect in my own
plumbing before the chain finally ran whole:

| call | defect found | fix |
|---|---|---|
| 14 | stale-J flush re-latched every block, muting the WHOLE Phase-4 TX (caller heard silence from S onward) | one-shot guard |
| 15 | flush latched after V34_mod had queued S into the same buffer - the mute swallowed S+Sbar entirely; with no Sbar anchor (11.4.2.1.2) the caller re-sent J' and gave up. Wire forensics of this call confirmed the rest: J16 vote fired on the real J' run, TRN went out 16-point, MP followed - all unanchored | latch at the bridge |
| 16 | (a) block receiver disabled by my own srx_rx16() re-keying (its enable gate conflated "block path on" with "caller is 16-pt"); (b) TRN16 cut to 0.9 s by the call-10 pacing shortcut | gate unconditional; chunks honored strictly |
| 17 | timed flush still shaved 12 ms off S's head | zero tx_buf in place (also the filter history) - S goes out whole |

Also fixed en route: p4_mp_decode reported the FIRST CRC-valid frame of a 2.5 s sliding
window, so a mid-run ack flip (exactly slmodem's own behaviour: 24 ack=0 frames then 4
ack=1) would surface up to 2.5 s late - longer than the caller waits for E. Positively
tested with a synthetic ack-flip stream (flip reported 0.4 s after the wire, vs ~2.5 s
before). Retro-decode of call 17's caller shows all ~160 of its MP frames genuinely
ack=0, so this was latent, not the cause.

### Call 17's burst measures equal to slmodem's on every axis

- head: `...SSSS` then TRN (whole S/Sbar, spec-shaped; caller at full level during S in
  BOTH calls, so J'-overlap is not a discriminator)
- TRN16: 1.79 s vs slmodem 1.72 s; 16-point from the first block in both (slmodem has NO
  4-point bootstrap - measured, cv 0.31 from block one)
- signal quality: raw-chain EVM floor 25.2% (ours, synthetic) vs 25.6% (slmodem, wire) -
  identical; k-means clusters sit on the spec constellation, ring ratio 2.998 vs 3.000
- continuity: one continuous DD-tracking pass shows our burst SMOOTHER than slmodem's,
  no jumps at any of the six 1024T chunk boundaries
- MP: constant SLCOMPAT fields on the wire (ca=ac=16800 trel=0 shape=1 mask=0x3fff),
  bit-identical to slmodem's acked frames

The caller anchors, runs its own Phase 4, exchanges MP with us for ~2.6 s - ack=0 in
every frame - and quits. Same caller acks slmodem 0.24 s after its MP starts.

### The anomaly hiding in plain sight

Every LIVE MP READ of every call has shown it: **the caller proposes ca=16800 ac=9600
trel=2 to us, but proposed ca=16800 ac=16800 trel=0 to slmodem.** ac is the answer->call
direction - OUR transmit direction - and the caller assesses it from OUR Phase-2 line
probing (L1/L2) and Phase-3 training signals. It rates our direction at barely a third of
slmodem's before Phase 4 even begins. Phase 2 and Phase 3 have never been compared
between the engines (linmodem's Phase 3 is also structurally nonstandard: a hand-rolled
2.3s-TX/2.0s-silent yield cycle produces THREE bursts where slmodem sends one continuous
2.8 s block). A second five-lens forensics workflow is now on it: Phase-2 probing
comparison, Phase-3 structure, spec ack-conditions (may a strict caller lawfully withhold
ack from an MP whose rates exceed its own proposal?), a free wire diff, and the
provenance of ac=9600 across all captures.

## 30. Phase 2 now completes - and the caller restarts it anyway

The reactive-ranging work (section 29 follow-on) moved the failure four stages earlier and
uncovered a chain of defects, each hidden behind the last:

| # | defect | evidence | fix |
|---|---|---|---|
| 1 | Phase-3 output muted whenever the caller transmitted first | caller reached Phase 3 at p3n=40 ms once ranging was correct; our S/PP/TRN/J went out silent; deadlock, line quiet 47 s | 11.3 Phase 3 is DUPLEX - yield only after our block is sent |
| 2 | Tone A raised at our L2 end, before the caller's probe | Tone A is its probe-TERMINATE signal; it aborted its probe at 240 ms and no white-noise phase happened at all | silence after L2; Tone A only once its probe is >=450 ms in |
| 3 | B-reversal detector latch-storm | 32 false latches in 180 ms at cos=-1.00 (DPSK blocks slipping past the INFOC gate) | 200 ms refractory + reference rebuild |
| 4 | classify() could not see the caller's INFO at all | INFO is 600 bps DPSK on 1200 Hz and **the carrier is suppressed**: mag@1200 measured 3..578 against rms 1750, far under the rms*0.4 gate, so INFO fell through to OTHR/WIDE | band test on 900+1050+1350+1500 (measured 1644..2283 vs threshold ~880) |
| 5 | we never waited for the caller's INFO1c | a fixed 1.5 s wait for a 2nd B-reversal pushed our INFO1a 1.4 s past its INFO1c; it re-sent INFO1c for 16 s | R_PRX waits for INFO1c + 100 ms quiet; R_INFO1A then does 0.5 s silence, 0.3 s Tone A, one INFO1a, 70 ms gap |
| 6 | the mute also swallowed our **J** | V34_send_J queues symbols and the state machine reaches WAIT_J in the same call, so the mute zeroed the very J the caller waits for (J at 10.712, WAIT_J 10.732, then silence) | p3go mute removed entirely |

Phase 2 now runs to completion live: ranging reactive, probe intact, `caller INFO1c
arriving` / `caller INFO1c done -> our INFO1a`, INFO1a sent, Phase 3 transmitting
continuously.

### The current wall

**The caller restarts Phase 2 ~180 ms after our INFO1a**, sending INFO0c (CRC-valid,
byte-identical to the INFO0c from the start of the call) every ~80 ms for the rest of the
call, while we sit in Phase 3 transmitting S/Sbar/PP/TRN/J. Neither side advances.

And the decisive measurement: **our INFO1a is BIT-IDENTICAL to slmodem's** -

```
1111011100100000000000000001101001101101000000000011010011011100001111
```

decoded from both wires, 70 bits, no differences. So content is not the defect.

Timing is comparable too (each stream in its own clock; in the working capture the caller
flow leads the answer flow by 0.759 s):

| | caller INFO1c | our/slmodem INFO1a | gap | caller's next act |
|---|---|---|---|---|
| working | 7.22 | 8.22 | 1.00 s | silent 1.7 s, then its Phase 3 |
| failing | 7.73 | 8.84 | 1.11 s | **INFO0c at 9.14, forever** |

The caller's INFO1c itself differs between the calls in exactly **2 bits** (frame indices
13 and 14: working 1,0 - failing 0,1), everything else identical.

Note a measurement trap found here: zero-padding one stream to align clocks destroys
`info_bits()`' DPSK phase reference, which is why an earlier scan "could not find"
slmodem's INFO1a at all. Work in each stream's own clock and convert times.

A five-lens forensics workflow is now on the question - INFO1a delivery properties vs
slmodem's, the full Phase-2 exit ladder, INFO1c field decode and the meaning of the 2-bit
delta, the caller's exact decision instant (is it reacting to our INFO1a or to the first
milliseconds of our Phase-3 S?), and fix design including a recovery path so a caller
INFO0c during Phase 3 can no longer deadlock us.

## 31. CONNECTED — the V.34 handshake completes on linmodem

Call 28, 2026-08-21 18:18. The full ITU-T V.34 startup ran to completion against the real
modem, with linmodem as the answer modem and no slmodem anywhere in the path:

```
[v34p2] L2 done (caller Tone B) -> Tone A         reactive L2 termination
[v34p2] Tone A reversal #3 (11.2.1.2.6)           contiguous with our probe
[v34p2] modem probe RECEIVED (580ms) -> symrate 5
[v34p2] caller INFO1c arriving
[v34p2] caller INFO1c done -> INFO1a NOW          40 ms, was 930 ms
[v34p3] Phase 3: S/Sbar/PP/TRN -> J
[v34p3] CALLER TRANSMITTING in Phase 3
[srx]   caller J detected at sym 3519 (28/32)
[srx]   J variant vote: J4=192 J16=180 -> J4POINTS
[p4]    caller commanded 4-point -> TRN/MP/E all 4-point
[p4]    caller TRN 512T done -> MP
[p4]    TX: TRN done (6 chunks) -> MP
[p4]    MP READ (consensus): ca=16800 ac=9600 trellis=64state ack=1   <-- THE ACK
[p4]    MP-prime READ (consensus)
[p4]    TX: E sent -> DATA (B1)
[p4]    E received at sym 12424                                       <-- both directions
```

Data mode held for ~64 s until the caller hung up. The acknowledgement that 27 calls
chased arrived 20 ms after our MP' went out.

### What actually unlocked it

The blocker was never Phase 4. It was **INFO1a latency**: V.34 11.2.2.1.6 budgets 700 ms
between the end of the caller's INFO1c and the start of the answer modem's INFO1a
(the round trip cancels in a tap measurement). slmodem answers in 22-27 ms. We answered
in 929-934 ms, because we went silent and waited instead of doing what 11.2.1.2.8-9
requires - hold Tone A while receiving INFO1c and reply immediately. The caller lawfully
abandoned Phase 2 and restarted with INFO0c, every single call, and everything we saw in
Phases 3 and 4 was that restart playing out downstream.

The second unlock followed within one call: once Phase 2 completed properly the caller's
J changed from J16POINTS to **J4POINTS** - it had been demanding 16-point only because it
had never seen a working INFO exchange. Our bridge handled only the J16 branch, so
`mp_16point` stayed pinned by SIPFAX_MP16/SLCOMPAT and we answered J4POINTS with a
16-point MP. Making the caller's J the authority for both flags produced the ack.

### Negotiated parameters

`ca=16800 ac=9600 trellis=64-state`, 4-point Phase 4, S=3429 carrier 1959 Hz. Note the
caller still rates our direction 9600 (section 30's ac anomaly) - worth revisiting now
that Phase 2 is correct, since its INFO1c asks us for 4 dB of power reduction which we do
not currently apply.

### Next wall: data-mode demodulation

Data mode is entered but no bytes are produced: no PPP traffic reached the bridge. The
V.34 data-mode receiver (48-point QAM at 16800, 4D trellis decoding, shell demapping,
precoder) is the remaining work - the Viterbi table and mapper groundwork from sections
1-9 feed directly into it.

Fixture saved: `test/fixtures/v34-captures/live-linmodem-connect.pcap` - the first capture
of a complete linmodem V.34 connection.

## 32. Data mode, session 1: the codec is done; the receive chain is the work

Starting the data-mode demodulator. The first thing to establish was how much of it
already exists — and the answer is: most of the hard part.

### The codec round-trips

`SIPFAX_DATALOOP` drives linmodem's own shell mapper, 4D trellis encoder/decoder,
precoder and scramblers TX→RX with no channel. At the parameters we actually negotiated
(R=16800, S=3429, 64-state) it returns **100.0% bit match**, likewise at 19200 and 9600.
So the shaping/coding/framing stack is not the work; the receive chain and its wiring is.

Two harness bugs had to be fixed before that number meant anything:

- **Symbol scale.** `put_sym` carries `coordinate × 128` in data mode — verified on the
  wire, lattice coordinates (1,5) arrive as (128,640) — which is exactly the contract
  `tcm_decision()` assumes (it reads a coordinate back as `(sample>>8)*2+1`). The harness
  was multiplying by 128 a second time.
- **Noise model.** It assumed raw lattice units, making the injected noise 128× too weak.
  Every "100% at 0 dB SNR" reading was noise rounding away to nothing.

A zero-input control (`SIPFAX_DL_ZERO`) pins the failure floor at 50.4%, which is what
separates "the decoder is working" from "the harness is measuring itself".

### The number that matters: ≥24 dB

With both fixed, the waterfall at R=16800 (deterministic LCG noise, so runs compare):

| SNR | 40 | 30 | 26 | 24 | 22 | 20 | 18 | 16 | 14 |
|---|---|---|---|---|---|---|---|---|---|
| bit match | 100% | 100% | 100% | 100% | 99.6% | 95.1% | 80.3% | 61.4% | 50.1% |

**The receiver must deliver ≥24 dB.** For reference our CMA/DD chain measured 2.91% EVM
(30.7 dB) on the caller's real Phase-3 TRN, so the margin exists in principle.

### The data-mode constellation is NOT the TRN/MP one

At R=16800 the encoder emits L=48, K=28, M=12 — 48 points on the **odd-integer (2Z+1)**
lattice, coordinates ±1,±3,±5,±7. `constellation_16800()` in `v34_front.py` builds the
**4Z+1 quarter-superconstellation** (coordinates 1,5,9,-3,-7…), which is correct for TRN
and MP and wrong for data mode. Fitting data symbols against it silently inflates EVM;
that mistake cost a full measurement pass. `data_constellation(n)` now provides the right
set (it matches linmodem's encoder except for a 2-point tie-break at energy 58, where
four equal-energy points exist and V.34 picks a specific pair).

### The captured data-mode signal does not fit a clean lattice

`live-linmodem-connect.pcap` holds 48 s of the caller transmitting in data mode. Against
the correct 48-point set our receiver plateaus at **14.8% EVM (16.6 dB)** — below the
24 dB required — and the figure is flat to ±0.05% across the whole 48 s, independent of
pass count (1→6 CMA, 1→14 DD) and step size.

The control proves the metric is sound: a clean synthetic 48-point signal through the
same chain fits its own set at **1.92%** and gets monotonically *worse* with larger sets
(11.7% at 64 points), the signature of a correct fit. The real signal shows no such
minimum — it improves monotonically out to ~192 points (12.0%) with no clean lattice
anywhere.

So the received symbols genuinely are not on the 48-point lattice. The leading explanation
is **precoding** (V.34 §9.6): if the caller precodes, its transmitted symbols are
deliberately spread within Voronoi cells and only collapse onto the lattice after the
receiver applies the matched channel response — which our receiver does not implement.
The caller's MP did carry non-zero h coefficients (noted as "always wild, never repeating"
back in section 30's cross-call survey), consistent with precoding being active.

### Next steps, in order

1. Determine whether the caller is precoding — decode the h coefficients from its MP in
   `live-linmodem-connect.pcap` and check whether applying that response collapses the
   received cloud onto the 48-point set.
2. Wire `V34_demod_cma` → `baseband_decode_impl` at data-mode entry (`p4_e_rx`), feeding
   symbols at coordinate×128 with the equalizer taps carried over from Phase-4 TRN rather
   than re-acquired — CMA cannot converge on a shaped 48-point set (our own control shows
   it degrading with more passes).
3. Verify against the loopback waterfall: the live chain must show ≥24 dB before PPP can
   run.

## 33. Precoding: refuted. Shaping: a real bug, found and fixed.

Checking the section-32 precoding hypothesis directly.

### What the MPs actually say

Decoded from `live-linmodem-connect.pcap`, both CRC-valid and stable across every frame:

| | ca | ac | trellis | nonlin | shape | h coefficients |
|---|---|---|---|---|---|---|
| caller's MP | 16800 | 9600 | 64-state | **1** | 1 | **all 6 non-zero**, identical in all 18 frames |
| our MP | 16800 | 16800 | 0 | 0 | **1** | all zero |

Per V.34 §9.6 the MP's precoder coefficients are computed by the *receiver* for the far
*transmitter*, so the caller's non-zero h and nonlin=1 instruct **our** transmitter, while
our h=0 tells the caller not to precode.

### The direct test refutes it

Filtering the received data-mode symbols by the caller's own
`H(z) = 1 + h1 z^-1 + h2 z^-2 + h3 z^-3`, and by its inverse, at every plausible
fixed-point interpretation (2^-12 … 2^-16, giving |h1| from 0.46 to 7.3), leaves the
symbols exactly as unstructured as before (0.576-0.580 against a 0.577 structureless
reference; the IIR inverse diverges for the larger scalings). **Precoding does not explain
the received signal**, consistent with our own h=0 instructing the caller not to use it.

### A real bug found on the way: expanded shaping

`expanded_shape` scales the constellation by 1.25 — `M = rint(1.25·2^(K/8))` instead of
`ceil(2^(K/8))` — so at R=16800:

```
expanded_shape=0  ->  K=28 q=0 M=12 L=48
expanded_shape=1  ->  K=28 q=0 M=14 L=56     (loopback: 100% bit match either way)
```

Our MP advertises **shape=1** and the caller obliges, but `V34_init()` hard-coded
`expanded_shape = 0`, so our constellation builder produced **L=48 while the caller
transmits L=56**. Now defaulted to 1 to match what we advertise (`SIPFAX_SHAPE`
overrides); the codec round-trips at 100% with shaping on, and Phase-2/3/4 regression is
unchanged (14 MP reads, worst step 11.1 ms).

This is a genuine defect, but on its own it does not account for the measurements: L=56
does not collapse the captured symbols either.

### A caution about the offline metrics

Several constellation-fitting results in section 32 and here proved unreliable, and the
control that exposed it is worth recording: the caller's **Phase-4 TRN** — which
demonstrably decodes at 8.7% EVM as 4-point — scores 0.574 on the same lattice metric that
scored data mode 0.577. The metric only means anything when the assumed constellation
matches the signal, so it cannot be used as a general "is there structure here" detector,
and the "no lattice fit at any size" framing of section 32 overstates what was shown.

What survives from those measurements is narrower and still useful: the data-mode signal
has kurtosis 1.71 against 1.335 for an unshaped 48-point QAM and 2.02 for Gaussian noise,
so it is a real modulated signal with a Gaussian-ised (shaped) amplitude distribution —
which is exactly what `shape=1` should produce.

### Conclusion and next step

Offline constellation fitting has reached its useful limit here: it re-acquires from
scratch in the middle of data mode, where V.34 provides no training signal, and its
verdicts depend on constellation assumptions that have been wrong twice. The reliable
oracle is the decoder itself, which already knows L, K, M, shaping and the trellis and
round-trips at 100%.

So the next step is unchanged but now better justified: wire `V34_demod_cma` →
`baseband_decode_impl` at data-mode entry, carrying the equalizer taps, timing and carrier
phase continuously from Phase-4 TRN rather than re-acquiring, feed symbols at
coordinate×128, and read the answer off the decoder's own bit output against the ≥24 dB
waterfall from section 32.
