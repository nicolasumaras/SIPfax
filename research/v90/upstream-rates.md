# Extending the V.90 upstream receiver

## Current qualification and next implementation gate (2026-09-11)

CT105 is running `dcb450f`, upstream 28800 bit/s with automatic initial echo acquisition. Its verified binary SHA256 is `a885a92e8db167b78dad585ce8c6b67ee8c35bc1f229bec687f2a4c981bad761`. The short hardware call passed 18 transfer hashes; the sustained call passed 129 targets without request retries over 621 seconds, with downstream CP remaining 49.333 kbit/s. Residual modem errors occurred. See the dated entries in [live status](../v90-live-status.md) for captures and qualification limits. Later sections below are historical development records, not the current deployment inventory.

The development branch includes 31200 bit/s and consistent per-call INFO1d/MP rate selection, but two hardware trials at 31200 failed before PPP. A 28800 control with the corrected negotiation passed. Offline changes to B1 training span, feedback delay, acquisition threshold, timing-loop terms, and frozen equalizer/carrier combinations have not recovered usable PPP from those failed recordings. Freezing equalizer adaptation preserves constellation energy but still recovers no CRC-valid frames, so energy collapse alone is not the root-cause explanation. These experiments are not deployed.

### Required symbol-rate coverage

V.90 clause 5.2 requires the **digital modem** to receive both 3000 and 3200 upstream symbols/s. Clause 6.2 allows the **analogue modem** to omit 3000. The server implements the digital role: the analogue-role exception does not remove its 3000-symbol/s requirement. Support for 3429 symbols/s is optional; 31.2/33.6 kbit/s upstream are also optional under clause 6.1. This distinction changes implementation priority, without discarding the existing higher-rate investigation or broader project scope.

Primary specifications: [V.90 (09/98), clauses 5.2 and 6.1–6.2](https://www.itu.int/rec/dologin_pub.asp?id=T-REC-V.90-199809-I!!PDF-E&lang=e&type=items), [V.34 (02/98), clause 9 and Tables 7–10](https://www.itu.int/rec/dologin_pub.asp?id=T-REC-V.34-199802-I!!PDF-E&lang=e&type=items).

The 3000-symbol/s implementation sequence below is now complete through Phase 4; startup negotiation and hardware qualification remain:

- Represent symbol rate, carrier and framing separately from bit rate. The current receiver fixes a 1920 Hz carrier and ten quarter-sample units per symbol; 3000 needs its specified carrier and fractional sampling interval.
- Implement the 3000-symbol/s mapping schedule, including high/low bit allocation and shell parameters. V.34 Table 7 uses J=7 and P=15, versus P=16 at 3200. A 3000 data frame/B1 contains 120 symbols; the current B1 length and inversion-cycle assumptions are 128 symbols and 448 pairs.
- Carry the peer-selected symbol rate and carrier from startup into Phase 4 and receive state, with consistent INFO0d/INFO1d and MP negotiation. Do not advertise unimplemented receive modes.
- Verify independent bit-exact framing, B1 acquisition, carrier/timing offsets, delayed-E replay, bounds and lower-rate regressions, then run a reversible hardware negotiation and bidirectional PPP transfer test where the peer supports this mode.

Broader reliability, fallback, echo tracking and future concurrent calls remain separate unfinished work. The successful single-call deployment does not prove full V.90 conformance.

### 3000-symbol/s mapping foundation

`v90mapping.c/h` now provides validated minimum-shaping profiles for all 4800–28800 bit/s rates at 3000 symbols/s, alongside the existing 4800–31200 profiles at 3200. Profiles carry carrier frequencies (including the exact fractional 3200 low carrier), P/J, b, K/M/q and high-frame count. Stateless frame indexing implements the standard switching schedule without accumulating drift. Inverse mapping removes the inserted highest shell bit on low frames, rejects a nonzero inserted bit, and leaves output buffers unchanged on invalid input. Callers must advance their frame index after erasures.

At this foundation stage the new module was linked into the native build; streaming integration followed below. Live negotiation does not yet advertise it. Independent tests use published Tables 8/10 and the legacy shell encoder to verify 7497 mapping frames across 23 profiles, including all 15/16-frame schedules, superframe transitions, large frame indices, exact bit counts, low-frame insertion and buffer/label rejection. CT105 ASan/UBSan checks pass; the clean native build passes with the CI compiler flags. The local sanitizer runtime libraries were missing, so sanitizer validation was performed in CT105's isolated temporary directory. Production was not changed.

The profile-based B1 generator now emits 120 labels at 3000 symbols/s and 128 at 3200. It maintains GPA scrambling across low/high frames, omits inserted bits from the scrambler input count, resets differential and 16-state trellis encoders, and applies inversion as the last J=7 data frame. A profile-based inversion helper covers subsequent data and counter wraparound. Independent tests compare every B1 label against a separate GPA recurrence, legacy shell mapper and geometric subset-conversion table for all 23 profiles. Bounds/rejection checks pass with ASan/UBSan on CT105; the native build passes. The PCM frontend still initializes only the 3200-symbol/s profile.

Equalizer training now accepts explicit 120/128-symbol B1 lengths for all three supported filter sizes (7/15 baud-spaced taps and 29 half-symbol taps). It still fits 80 interior symbols and validates the remaining interior observations. The existing entry points delegate to length 128 without changing their algorithm. Unsupported lengths/strides reject before input-array access. Tests verify independent distorted-channel recovery at both lengths, wrapper equivalence at 128, held-out rejection, and exact-length input allocations under CT105 ASan/UBSan. The 28.8 kbit/s seed-98017 PCMU/ISI/clock/timing matrix passes in direct and delayed-E modes. No runtime deployment followed this change.

B1 acquisition now has an explicit 3000/3200-profile initializer and uses the selected 120/128-symbol observation window. It obtains labels from the independently tested mapping generator, replacing duplicated 3200 generation. The legacy initializer retains its original rate contract, including exclusion of the separate 4800 PCM path. Tests verify exact B1 boundaries, gain/phase/correlation, noise tolerance, invalid-input resets and fresh acquisition for all 23 profiles. All 29 native-suite tests, 28.8 seed-98017 waveform matrix, 31.2 mapping/stream tests, native build and CT105 ASan/UBSan checks pass. Test link commands now include the shared mapping module.

The symbol-stream receiver now accepts explicit 3000/3200 profiles. B1 replay, carrier/equalizer fits and data-frame inversion use the selected length/framing. A bounded generic trellis entry point supports the additional 3000 constellations; existing 3200 kernels retain their paths. New-profile callbacks report each frame's actual bit count, including erasures, and maintain differential history across rejected shells. The legacy initializer still excludes the separate 4800 PCM mode.

Independent symbol-stream tests cover all 23 profiles through B1/data transitions and data-frame switching, with ideal/impaired carrier and baud/half-symbol inputs. They compare exact bits, frame lengths and source-symbol positions. The complete 29-test native suite and 28.8 seed-98017 PCM/timing matrix pass. The carrier-fit fixture was updated to initialize the new length field and now checks both lengths. CT105 ASan/UBSan covers all 3000 profiles with history wrap, reacquisition, both sampling modes and generic-trellis size rejection. The native build passes. No production deployment or 3000 hardware qualification follows these symbol-level results.

The explicit PCM initializer now accepts the profile's symbol rate and high/low carrier. The matched filter uses its symbol spacing, acquisition lanes span that spacing, Gardner updates use fractional quarter-sample intervals, and half-symbol observations use the selected midpoint distance. Frame delivery consumes the current low/high-frame bit count. Invalid explicit profiles leave the receiver unchanged; legacy initialization still selects 3200/high carrier.

All 22 basic 3000-symbol/s PCM profiles (4800–28800, both 1800/2000 Hz carriers) pass four timing offsets with independent G.711 mu-law quantization and exact PPP frame comparison. A long 26400/2000 Hz seed-98017 case initially lost frame 41. Reducing initial timing phase gain from 0.1 to 0.05 only for 26400/3000 closes it; default and seeds 43127, 62091 and 98017 now pass all eight phase/±100 ppm cases on both carriers with ISI, mu-law and 192 expected frames. The 28800/3000 seed-98017 matrices pass on both carriers. These are direct PCM tests; the delayed-E/Phase4 path still selects 3200 and is explicitly outside their scope.

All 29 native tests, the legacy 28800/3200 seed-98017 direct/delayed-E matrix, native build and CT105 profile-initialization/PCM ASan/UBSan checks pass. CI adds the basic profiles, targeted stress regressions and memory checks. Production is unchanged.

Ja/CP training reception now has an explicit 3000/3200-symbol profile and carrier initializer. The matched filter uses the selected spacing and carrier; 3000 timing hypotheses interpolate adjacent quarter-sample filter outputs. Legacy 3200/high-carrier sampling is retained. Invalid profile initialization leaves state unchanged.

Independent tests pass 160 linear-PCM and 160 mu-law cases across both symbol rates/carriers, four fractional offsets and ±100 ppm, with carrier offset/noise. They compare complete decoded descriptors, reject corrupted CRCs, accept E only after valid data CP, and reject E after CPt or corrupted CP. One mu-law Ja case at 3200/low carrier initially failed; adding half-quarter-sample timing hypotheses for that profile (20 instead of 10 lanes) closes it. This change leaves the legacy high-carrier timing path unchanged. All 30 native tests, native build and CT105 expanded-history/profile-validation ASan/UBSan checks pass.

S/Sbar detection now uses the selected carrier and symbol rate, with profile-preserving resets. Phase 4 retains that profile through initial training, E-triggered history replay and rate renegotiation. Independent shaped-waveform tests pass 240 S/Sbar cases and reject noise/single tones. All 22 basic 3000 rate/carrier profiles now pass both direct and delayed-E PPP replay. Five long seed-98017 mu-law/ISI/clock/timing matrices pass: 26400 and 28800 at 3000 on both carriers, plus the legacy 28800/3200 high carrier. Both 3000 carrier renegotiation suites, all 31 native tests, the native build and CT105 ASan/UBSan profile/reset checks pass. These results supersede the earlier direct-only test limitation. Production remains unchanged.

Startup now builds INFO1d offers for 3000/3200 from the CRC-valid peer carrier capabilities, prefers the supported high carrier, and selects the low carrier when required. INFO1a must select an offered mode; its profile propagates into Ja reception, S/Sbar detection, the provisional CPt monitor and Phase 4. The 3000 offer is capped at the implemented 28800 bit/s; 3200 retains peer-dependent large-constellation limits. Retraining rebuilds the original offers from the captured call configuration. INFO0d no longer advertises the unimplemented optional 3429-symbol/s V.90 receiver.

`SIPFAX_V90_UPSTREAM_SYMBOL_RATE=3000` or `3200` restricts offers for controlled qualification; unset or other values allow both implemented symbol rates. A restriction never overrides missing peer carrier support. This setting is captured per call and survives retraining. It does not select a carrier independently of the offer.

Independent wire tests pass 512 combinations of carrier masks, configured symbol restriction, large-constellation support and selected mode, including corrupted INFO1a CRC rejection, exact INFO1d CRC/rate fields, selected training/Phase4/MP profiles and preserved retrain offers. All twelve 3200 low-carrier rates pass direct and delayed-E mu-law PPP replay. The 32-test native suite, native build and CT105 ASan/UBSan startup profile checks pass. Long 26400/28800/31200 low-carrier matrices also pass with seed 98017, ISI, mu-law, timing offsets and ±100 ppm, in direct and delayed-E modes. Hardware control `03344c6b-d94b-4bf0-8bd5-b2d78704812a` with `7ba92b5` forced to 3200/high at 28800 passed all 18 transfer hashes and the internet probe in 36.282 seconds, with zero modem errors. Native CP remained 49.333 kbit/s downstream. Its 13122-packet capture had zero drops/gaps, 3252 matching downstream payloads and 3265-to-3254 matching upstream payloads after the eleven-packet startup suffix.

The first forced 3000/high call at 28800, `04fee980-b04c-4c6b-8dde-f198e4c7bac5`, accepted the profile and decoded CP at 49.333 kbit/s, then E at 4.104875 seconds and B1 correlation 0.9717 at 4.144875 seconds. It decoded zero valid PPP frames and ended with Windows error 721. The 13224-packet capture had zero drops/gaps; 3277 downstream payloads matched exactly, and 3290-to-3279 upstream payloads matched after the eleven-packet startup suffix. This rules out observed RTP loss/payload changes in that capture, not all analogue/DAC impairment. Raw RX/TX recordings are retained privately. Next is a lower-rate 3000 control and offline decoding diagnosis. Both wrappers restored `dcb450f` at 28800/auto; hardware qualification of 3000 remains incomplete.


### Live 3000-symbol/s rate controls

The same `7ba92b5` binary subsequently passed two forced 3000/high-carrier calls:

- 7200 bit/s: attempt `b0c65de1-f282-4fa3-b236-e15afc4980fe`, 18 independently verified transfer hashes and internet access over 65.604 seconds, zero Windows modem errors. Native CP was 49.333 kbit/s downstream, B1 correlation 0.9787. Capture: 19047 packets, zero capture drops/gaps; 4728 downstream primary payloads matched, and 4741-to-4730 upstream payloads matched after the eleven-packet startup suffix.
- 26400 bit/s: attempt `f6c0539d-456f-4622-b6ee-55f140196adb`, 18 verified transfer hashes and internet access over 36.843 seconds, zero Windows modem errors. Native CP was 49.333 kbit/s downstream, B1 correlation 0.9679. Capture: 13257 packets, zero capture drops/gaps; 3285 downstream primary payloads matched, and 3298-to-3287 upstream payloads matched after the eleven-packet startup suffix.

Both calls ended with a clean disconnect and restored the verified `dcb450f` production baseline. Raw RX/TX and RTP captures are retained privately. An offline replay with automatic initial echo correction recovers 179 CRC-valid candidate frames from the 26400 recording and none from the failed 28800 recording. This localizes the observed failure beyond general 3000-symbol/s negotiation/framing, but does not establish a specific equalizer, constellation or physical-channel cause.

The highest failing profiles at 3000/28800 and 3200/31200 both use q=5; this is a diagnostic lead, not proof of an encoder/decoder defect. The current startup deadline guards missing B1 only, so a detected B1 followed by zero valid PPP frames waits for the caller timeout. A bounded recovery/downshift policy and continued high-rate receiver diagnosis are next. Short-call success does not qualify sustained 3000 operation or all mandatory rates.

## Historical development record

The code default remains 4,800 bit/s at 3,200 symbols/s. The experimental
`SIPFAX_V90_UPSTREAM_RATE=7200` selects the eight-point receiver; `9600` selects
the twelve-point receiver; `12000` selects the experimental twenty-point receiver; `14400` selects
the experimental thirty-two-point receiver; `16800` selects the experimental
fifty-six-point receiver; `19200` selects the experimental ninety-six-point
receiver with one uncoded bit per symbol; `21600` selects the experimental
160-point receiver with two uncoded bits per symbol; `24000` selects the
256-point receiver with three uncoded bits and provisional trellis-feedback
equalization.
MP advertises only the configured rate. Unset or
unsupported values select 4,800. Hardware has verified 7,200 and 9,600 modes;
12,000 initially failed its hardware upload, then passed after B1 equalization.
The lab currently selects the integrated 24,000 receiver (`36577ed`) after CI
and successful short and sustained hardware tests. Residual errors remain.
The preceding 19,200 carrier-fit build passed a sustained run with live
renegotiation recovery. Broader qualification is required before changing
the code default.

At the existing symbol clock, the first extension is 7,200 bit/s: K=6, M=2,
q=0 and eight constellation points. V.34 defines the ring ordering through its
hierarchical shell mapping. `v90shell.c` implements that ordering and its inverse,
using 64-bit counts and rejecting ring tuples outside the selected K-bit range.
See [V.34 clauses 9.2–9.6](https://www.itu.int/rec/dologin_pub.asp?id=T-REC-V.34-199802-I!!PDF-E&lang=e&type=items).

`tools/tests/v90-shell.py` independently enumerates tuples for M=1,2,3,
checks their ordering and inverse indices, and compares them with the existing
V.34 transmitter. It also tests expanded M=18/K=31 counts. The standalone
sanitizer test covers all supported M/K combinations. These checks validate
shell arithmetic, not waveform demodulation or PPP reception at a higher rate.

`v90_trellis_qam8_pair` now accepts carrier/gain-aligned eight-point pairs and
retains ring labels through the 16-state soft trellis. `v90qam8.c` reverses the
shell and differential mapping into 18 scrambled bits per eight symbols.
`tools/tests/v90-qam8.py` uses an independent Table 13 transmitter and enumerated
shell ordering to check all 16 initial states, controlled symbol errors, noise,
and rejected-frame recovery. Ring decisions use the nearest point within each
quadrant; this is not joint soft shell decoding.

`v90_qam_b1_init_rate` generates the selected-rate B1 with zero encoder state and
the final data frame's superframe inversions. Its bounded symbol-domain
correlator reports the B1 end, carrier phase, and gain. Tests independently
generate all 288 scrambled bits and 128 symbols, then check noisy acquisition,
phase/gain changes, and negative/reset controls. The continuous stream replays B1 through the trellis and descrambler, tracks
carrier/gain against the selected constellation decisions, and aligns mapping frames. Ten
matched-filter timing lanes feed this receiver in `v90upstream.c`. Invalid
symbols drop lock; reacquisition requires another B1. A rejected shell resets
the affected lane’s PPP framing/descrambler, which then self-synchronizes.

Remaining integration work:

- Broader sustained qualification and reduction of residual link errors.
- Adaptive equalization and loss-of-lock detection beyond invalid inputs.
- Higher rates and fallback after this first eight-point path is qualified.

V.90 Table 16 defines upstream rate selection and its capability mask. Higher
rates must not be enabled by default until their receive path is validated.
See [V.90 Table 16](https://www.itu.int/rec/dologin_pub.asp?id=T-REC-V.90-199809-I!!PDF-E&lang=e&type=items).

`v90-qam8-wave.py` independently generates B1, scrambled UART/PPP data, shell
and trellis symbols, and pulse-shaped PCM. Exact frames survive fractional
timing, carrier offset and noise; a deliberately invalid FCS is rejected.
Rate negotiation tests independently decode MP rate, capability mask and CRC.
These synthetic checks do not establish hardware interoperability.

## Hardware qualification of 7,200 upstream

The first hardware call acknowledged MP but missed upstream B1. Recorded audio
showed E detection about 4.7 ms after B1 began. Phase 4 now retains 160 samples
and replays them through the reset receiver; a delayed-E synthetic regression
checks exact PPP data recovery. The recorded B1 then acquires at 0.9823.

Two subsequent XP/ATA187 calls authenticated PPP and fetched an external page.
The first verified a 32 KiB download; the second verified another 32 KiB download
and 16 KiB sent upstream in 16 encoded HTTP request payloads. The server checked
the complete uploaded bytes and hash, with PPP source 10.64.0.2. Both calls
finished with zero Windows CRC/alignment errors and disconnected cleanly.
Native logs show initial downstream 160000/3 bit/s; the live process was verified
to select the 7,200 upstream receiver. These are short interoperability tests,
not a sustained throughput measurement or full V.90 conformance result.

The passive captures had zero kernel drops and no sequence gaps in the call
streams. Both ATA streams had a 120-sample timestamp increment during early
media; forwarded call audio had normal increments. RED primary payloads matched
the server streams. Internal ATA underflow was not measured.

The next-rate `v90_trellis_qam12_pair` kernel accepts the three-ring, twelve-point
minimum constellation for 9,600 bit/s at 3,200 symbols/s. Its independent tests
compute Figure 9 subsets from coordinates and then apply Table 13, covering all
16 initial encoder states and noise. Ring 2 repeats a subset class; its ring
index must not be used directly as a Table 13 subset number. The companion `v90_qam12_frame` now recovers 24 scrambled bits; exhaustive
tests cover all 4,096 shell indices, differential bits and rejection bounds.
Rate-specific B1 and continuous symbol decoding now pass independent 9,600-bit/s
tests, including gain/carrier changes and reacquisition. The native audio/MP
path now enables experimental 9,600-bit/s calls. Independent PCM tests recover
exact PPP frames, including delayed E, both signs of 100 ppm clock error,
fractional timing and CRC rejection. The first hardware trial at 9,600 bit/s authenticated PPP, fetched an external
page, verified a 32 KiB download and 16 distinct 1 KiB upstream request payloads,
then disconnected cleanly. Windows reported 54.218 seconds of connected time
and zero CRC/alignment errors. Native CP confirmed 49.333 kbit/s downstream.
The development server now runs this experimental rate; a subsequent 725.774-second bidirectional test
completed all 64 checksummed 32 KiB downloads and 64 verified 1 KiB upstream
requests, then disconnected cleanly. Native downstream CP remained at 49.333
kbit/s. Windows counted 32 CRC and five alignment errors. All 37,733 downstream
RTP primary payloads matched across FreePBX, as did all forwarded upstream
payloads after the 11-packet early-media prefix. The 151,534-packet capture
had no sequence gaps or kernel drops. Residual errors remain unexplained. The adaptive 7,200-bit/s binary is retained
as a rollback.

A subsequent sustained 7,200-upstream call completed 62 alternating 32 KiB
checksummed downloads and 1 KiB upstream requests. Download 63 timed out at the
probe's 30-second deadline after 21,417 bytes. The final Windows statistics
reported 822.392 seconds, 54 CRC errors and six alignment errors. Native logs
showed successful 53.333-to-49.333 kbit/s downstream renegotiation and upstream
B1 reacquisition before further verified transfers. No RTP sequence gaps were
captured; the only missing final downstream packet followed the caller BYE.
Post-call ATA counters included one underflow and one FIFO drop, without event
timing. This establishes partial sustained recovery, not sustained reliability.
This prompted the controlled downstream-rate comparison described below.

## Clock recovery after the downstream-rate comparison

A 49.333-downstream / 7.2-upstream comparison failed during its fifth upload,
with zero Windows CRC/alignment errors. Recorded upstream audio exposed the
fixed timing lanes drifting out of alignment: 196 valid frames were recovered,
then reception stopped. Offline resampling by +1 ppm recovered 228 frames.

The eight-point native receiver now uses a normalized Gardner timing loop,
interpolating the quarter-sample matched-filter outputs and tracking phase and
sample-clock offset separately. Actual symbol timestamps preserve duplicate
filtering across timing lanes. The same recording yields 229 valid frames and
continues decoding to the end. Synthetic PCM/PPP tests cover both signs of
100 ppm clock error; the previous receiver fails that test. Hardware validation of this timing change completed 64 alternating 32 KiB
checksummed downloads (2 MiB) and 64 verified 1 KiB upstream requests over
784.478 seconds of connected time, followed by a clean disconnect. Windows
reported 32 CRC and four alignment errors. Native CP messages show downstream
49.333 kbit/s initially and 48 kbit/s after renegotiation; transfers continued.
All 40,667 downstream RTP primary payloads matched across the PBX; upstream
payloads matched after the 11-packet early-media prefix. No RTP sequence gaps
or kernel capture drops were recorded. This is a successful sustained test,
not an error-free or full-conformance result.

## 12,000-bit/s foundation

The twenty-point minimum-constellation kernel now preserves five-bit labels
for both symbols in each trellis history entry. Independent Figure 5/9 and
Table 13 tests cover all 16 encoder states, all 20 labels, noise and invalid
input rejection. The M=5/K=18 mapping-frame inverse recovers 30 scrambled bits;
tests exhaust all 262,144 valid shell indices and check unused-shell rejection,
differential-history recovery and smaller-output cross-rate rejection. Rate-specific
B1 and continuous 30-bit frame decoding also pass independent carrier/gain/noise
and reacquisition tests. Carrier slope is estimated from the known B1 before
replay, avoiding a transient that corrupted outer-point decisions at 12,000.
Native audio/MP negotiation now supports experimental 12,000 bit/s. Independent
PCM tests recover exact PPP frames through fractional timing, delayed E, carrier
offset/noise, CRC rejection and both signs of 100 ppm clock drift. Negotiation
tests verify its rate, capability mask and CRC. The first 12,000-bit/s hardware call authenticated PPP and verified a 32 KiB
download (19.105 seconds), but its first 1 KiB upstream request timed out after
30.028 seconds. Windows counted zero CRC/alignment errors. The call disconnected
cleanly and the server was restored to the validated 9,600-bit/s binary/settings.
The passive capture had 17,993 packets, zero kernel drops and no RTP sequence
gaps. All 4,467 downstream primary payloads and the forwarded upstream suffix
matched across FreePBX. Replaying the recorded upstream audio reproduces shell
rejections from acquisition onward (297–329 per locked lane by second 85), with
55 valid PPP frames recovered. This is not the earlier late-onset clock stall;
receiver distortion/equalization and tracking need investigation before retrying.

## B1-trained equalization for 12,000

Recorded B1 exposed intersymbol distortion: a seven-tap complex FIR reduced
held-out symbol MSE from approximately 0.410 to 0.039. The native 12,000 path
now fits this filter on 80 B1 interior symbols and accepts it only if remaining
interior symbols improve by at least 20%, with bounded coefficient norm. Fits
are regularized toward identity; rejected fits retain the identity filter.
Carrier-aligned symbols pass through three symbols of lookahead before trellis
decoding, preserving original source timestamps and the B1-to-data boundary.
This is training-based equalization, not decision-directed adaptive equalization.

The failed hardware recording now yields 85 valid PPP frames versus 55 before,
with zero rejected shells in one timing lane. Other lanes still reject shells.
Independent distorted-PCM tests recover exact PPP frames; the pre-equalizer
receiver recovers only the first frame in the same regression. Separate tests
cover held-out validation, unseen symbol recovery, delay and invalid-input
state preservation. The hardware retry authenticated PPP, fetched an external
page, verified a 32 KiB download in 7.579 seconds and all sixteen 1 KiB upstream
request payloads. It disconnected cleanly after 48.179 seconds, with zero
Windows CRC/alignment errors. Native CP confirmed 49.333 kbit/s downstream.
All 3,851 downstream primary payloads and the forwarded upstream suffix matched
across FreePBX. The 15,520-packet capture had no sequence gaps or kernel drops.
CT105 retains this experimental equalized 12,000 build, with the validated
9,600 binary saved as rollback. A subsequent 677.614-second sustained call
completed all 64 alternating checksummed 32 KiB downloads (2 MiB total) and
64 verified 1 KiB upstream requests, followed by a clean disconnect. Native
CP remained at 49.333 kbit/s downstream. Windows counted 19 CRC and one alignment
error. All 35,322 downstream RTP primary payloads and the forwarded upstream
suffix matched across FreePBX, with no sequence gaps or kernel drops in the
141,866-packet capture. This is a successful soak, not error-free qualification.

## 14,400-bit/s foundation

The thirty-two-point minimum constellation (M=8/K=24 at 3200 symbols/s) now
has a soft trellis kernel and a 36-bit mapping-frame inverse. Independent
Figure 5/9 and Table 13 tests cover all states, all labels and noisy pairs.
A half-tuple enumeration oracle covers every energy-bucket boundary and random
24-bit shell indices without storing all 16 million eight-tuples. Its ordering
is cross-checked exhaustively for M=1/2/3. Rate-specific B1, continuous
36-bit decoding and native audio/MP integration now pass independent tests,
including carrier offset/gain changes, reacquisition, fractional timing,
both signs of 100 ppm clock drift and simulated intersymbol interference.
The first 14,400 hardware call authenticated PPP, fetched an external page,
verified a 32 KiB download in 7.532 seconds and all sixteen 1 KiB upstream
request payloads, then disconnected cleanly after 44.374 seconds. Windows
reported zero CRC/alignment errors; native CP confirmed 49.333 kbit/s downstream.
All 3,657 downstream primary audio payloads and the forwarded upstream suffix
matched across FreePBX. The 14,739-packet capture had no sequence gaps or kernel
drops. CT105 retains the 14,400 build, with the validated equalized 12,000 binary
saved for rollback. A subsequent 617.839-second bidirectional soak completed
all 64 verified 32 KiB downloads (2 MiB) and 64 verified 1 KiB upstream requests,
then disconnected cleanly. Windows reported one CRC error and zero alignment
errors. Native downstream CP remained 49.333 kbit/s. All 32,331 downstream
primary RTP payloads and the forwarded upstream suffix matched across FreePBX;
the 129,862-packet capture had no sequence gaps or kernel drops. Post-call ATA
counters showed zero network packet loss, 80 ms average playout, one underflow,
one FIFO drop and two starvation events. These aggregate counters do not locate
events within the call or establish the cause of the CRC error.

## 16,800-bit/s path and equalizer adaptation

The fifty-six-point minimum constellation (M=14/K=30 at 3200 symbols/s) has
a six-bit-label soft trellis, 42-bit mapping frames, B1 acquisition and native
PCM/MP integration. Independent tests cover all states/labels, energy-bucket
boundaries, unused-shell rejection, carrier/gain changes, clock drift and
intersymbol interference. A rate-dependent idle suffix in the PCM test fully
flushes trellis lookahead without relying on zero-filled audio.

The fixed trained equalizer lost an interior PPP frame under 100 ppm clock
drift at this rate. Guarded normalized LMS updates now track confident symbol
decisions for the 16,800 receiver, with bounded step and coefficient norm;
the same test recovers every frame. Lower rates retain training-only filtering.
An independent changing-channel test checks improvement over a fixed filter
and invalid-update state preservation. The first 16,800 hardware call
authenticated PPP, fetched an external page, verified a 32 KiB download in
7.500 seconds and all sixteen 1 KiB upstream request payloads. It disconnected
cleanly after 44.494 seconds with zero Windows CRC/alignment errors. Native CP
confirmed 49.333 kbit/s downstream. All 3,663 forwarded downstream primary
payloads matched; the final unforwarded server packet followed the caller BYE
by 1.614 ms. The forwarded upstream suffix also matched. The 14,769-packet
capture had no RTP sequence gaps or kernel drops. At that stage CT105 retained 16,800, with
the validated 14,400 binary saved for rollback. Sustained 16,800 qualification
now includes a 624.238-second bidirectional soak: all 64 verified 32 KiB
downloads (2 MiB) and 64 verified 1 KiB upstream requests passed, followed by a
clean disconnect. Windows counted three CRC errors and zero alignment errors.
Native downstream CP remained at 49.333 kbit/s. All 32,656 downstream primary
payloads and the forwarded upstream suffix matched across FreePBX. Capture
contained 131,165 packets, zero kernel drops and no RTP sequence gaps. This is
one successful sustained run, not broad reliability qualification.

## 19,200-bit/s mapping foundation

V.34 Table 10 selects M=12/K=28/q=1 at 3200 symbols/s: 96 constellation points
and 48 bits per mapping frame. The new kernel preserves seven-bit labels,
including Q=2*ring+uncoded_bit. The inverse implements the clause 9.3.1 parser
order: 28 shell bits followed by four groups of I1/I2/I3/Qa/Qb. Independent
coordinate/trellis tests cover all states and labels; shell tests cover bucket
boundaries, unused tuples and output bounds. Rate-specific B1, continuous
48-bit frame decoding and PCM/MP integration now pass independent tests,
including fractional timing, carrier/gain changes, clock drift and intersymbol
interference. The q-bit count is explicit in acquisition and PPP delivery so
uncoded bits remain in parser order throughout. The first 19,200-bit/s hardware call authenticated PPP, fetched an external page, verified a 32 KiB download in 7.476 seconds and sixteen distinct 1 KiB upstream request payloads. It disconnected cleanly after 39.527 seconds with zero Windows CRC or alignment errors. Native CP confirmed 49.333 kbit/s downstream. All 3,415 downstream primary payloads matched across FreePBX, as did the forwarded upstream suffix after eleven early-media packets. The 13,763-packet capture had zero kernel drops and no RTP sequence gaps. CT105 now retains the experimental 19,200 receiver, with the validated 16,800 binary saved for rollback. The first sustained 19,200-bit/s test failed after twelve successful download/upload cycles (384 KiB downloaded and 12 KiB of upstream payloads). The thirteenth 32 KiB download timed out after 30.004 seconds with 18,681 bytes received. Windows reported eight CRC errors and one alignment error at 168.161 seconds; the harness disconnected cleanly. All 9,872 downstream primary RTP payloads and the forwarded upstream suffix matched across FreePBX; the 39,690-packet capture had zero kernel drops and no RTP sequence gaps. Native logs show a rate renegotiation retaining 49.333 kbit/s downstream. B1 acquisition succeeded afterward, but offline replay of the recorded post-renegotiation audio reproduced poor recovery: only two valid PPP frames were decoded. This does not yet establish the cause of the earlier downstream CRC errors. Post-call ATA counters reported 128 lost segments, one underflow and one FIFO drop, without event timing. The short-call success therefore does not establish sustained 19,200 reliability.


## B1 carrier fitting after the 19,200 soak

The previous estimator divided observations by individual B1 reference points
and estimated slope from two half-frame averages. This weights disturbances on
inner points heavily. The revised estimator minimizes known-waveform squared
error: a bounded frequency search maximizes complex cross-correlation, then
extracts gain and phase referenced to the first B1 symbol.

An independent 48-trial noisy-constellation test measures frequency RMSE of
0.00012064 rad/symbol, versus 0.00020290 with the previous estimator, and checks
noiseless signed offsets, gain/phase and circular B1 history. Existing waveform
tests include clock drift and intersymbol interference. Replaying the failed
call after renegotiation now yields nine valid PPP frames versus two; the best
timing lane rejects one shell versus 140. Before renegotiation, replay yields
483 frames versus 481 and the best lane rejects 35 shells versus 187. This
improves receiver recovery in the saved recording; it does not yet prove the
original downstream CRC cause.

The carrier-fit hardware retry completed all 64 verified 32 KiB downloads (2 MiB) and 64 verified 1 KiB upstream requests over 710.161 seconds, then disconnected cleanly. Windows counted 31 CRC errors and two alignment errors. Native CP changed downstream from 49.333 to 48 kbit/s during renegotiation; verified transfers continued afterward and the error counters stopped increasing. All 36,953 downstream primary RTP payloads and the forwarded upstream suffix matched across FreePBX. The 148,419-packet capture had zero kernel drops and no RTP sequence gaps. All 129 response hashes were independently checked. Post-call ATA counters included 23 lost segments, one underflow and one FIFO drop, without event timing. This establishes one sustained 19,200 run with live rate-change recovery, not error-free maximum-rate reliability.


## 21,600-bit/s integration

At 3200 symbols/s the minimum constellation uses M=10/K=26/q=2: 160
points, eight-bit labels and 54 bits per mapping frame. The trellis retains
both labels in its existing 16-bit history. Mapping, B1 acquisition, continuous
PPP delivery and MP rate/capability negotiation preserve both uncoded bits.

The combined clock-drift/distortion test exposed insufficient tracking speed.
This rate uses a 0.0001 clock integrator gain with the existing phase gain and
200 ppm frequency bound, plus a 0.05 normalized-LMS step gated at squared
point distance below 0.5. The confidence radius stays below half the minimum
constellation spacing. Lower rates keep their existing settings. Separate
controls with the slower clock loop or slower equalizer lose interior frames;
the final receiver recovers all expected frames for both clock-offset signs.
An exploratory joint carrier/equalizer fit was unnecessary and is excluded.

Independent tests cover all 160 labels and trellis states, shell boundaries,
unused tuples, B1 and source timing, PCM to exact PPP, clock drift, distortion,
and their combination.

The first 21,600-bit/s hardware call authenticated PPP, fetched an external page, verified a 32 KiB download in 9.874 seconds and all sixteen distinct 1 KiB upstream payloads, then disconnected cleanly after 73.776 seconds. Windows reported zero CRC and alignment errors. Native CP confirmed 49.333 kbit/s downstream. All 5,129 downstream primary RTP payloads and the forwarded upstream suffix matched across FreePBX; the 20,649-packet capture had zero kernel drops and no RTP sequence gaps. All 18 response hashes were independently verified. Upstream checks took 1.480–11.083 seconds, slower than the previous 19,200 short test, so this establishes connectivity rather than a throughput improvement.

Offline replay decodes 188 valid PPP frames. A slower clock-loop control decodes 193 and reduces the best timing lane's rejected shells from 23 to eight, but fails the independent combined clock-drift/distortion test. Reducing equalizer adaptation instead yields only 172 frames. The next step is to reconcile drift acquisition speed with timing-noise tolerance; neither control is a qualified replacement. The subsequent staged-clock soak is documented below.

At that stage CT105 retained the experimental 21,600 build `d87f907`, with the validated
19,200 carrier-fit binary saved as `lm.pre-21600`.


## 21,600 clock acquisition and tracking

The fast clock integrator is now limited to the first 9600 symbols (three
seconds) after B1. It then uses the existing 0.00001 tracking gain while
preserving frequency and phase state. Reacquisition starts the fast interval
again. Lower-rate profiles are unchanged. The saved first hardware call
now decodes 193 valid PPP frames instead of 188; the best lane rejects eight
shells instead of 23. This matches the slower-loop control without sacrificing
the high-drift case. Independent PCM tests now extend both signs of 100 ppm
clock error with distortion to roughly 15 seconds, recovering all 192 expected
PPP frames per waveform, including after delayed-E replay. The staged-clock hardware retry passed authenticated PPP internet access, a verified 32 KiB download in 7.468 seconds and all sixteen 1 KiB upstream payload checks. It disconnected cleanly after 45.025 seconds, versus 73.776 seconds in the preceding call. Windows counted zero CRC or alignment errors. Upstream probe times ranged from 1.469 to 4.694 seconds (median 1.4835 seconds); two probes still took about 4.7 seconds. Native CP confirmed 49.333 kbit/s downstream. All 3,696 downstream primary RTP payloads and the forwarded upstream suffix matched across FreePBX; the 14,897-packet capture had zero kernel drops and no RTP sequence gaps. All 18 response hashes were independently verified.

The first sustained 21,600-bit/s run passed all 64 verified 32 KiB downloads (2 MiB) and 64 verified 1 KiB upstream requests over 722.028 seconds, then disconnected cleanly. Windows counted 24 CRC errors and five alignment errors. Native CP remained at 49.333 kbit/s downstream through a completed renegotiation, and verified transfers continued afterward. All 37,542 downstream primary RTP payloads and the forwarded upstream suffix matched across FreePBX; the 150,779-packet capture had zero kernel drops and no RTP sequence gaps. All 129 response hashes were independently checked. Post-call ATA counters included 23 lost segments, one underflow and one FIFO drop, without event timing. This establishes one sustained run with recovery, not error-free maximum-rate reliability.

CT105 now runs `047d95e` at 21,600 bit/s; the prior build remains available
as `lm.pre-staged-clock`, and the validated 19,200 binary as `lm.pre-21600`.


## Steady-state phase correction and G.711 coverage

After three seconds from B1, the 19,200 and 21,600 profiles now reduce the
phase correction gain from 0.1 to 0.02 while preserving phase/frequency state.
The first 21,600 recording yields 202 valid PPP frames instead of 193 with
integrator narrowing alone (188 before either change); the best timing lane
rejects five shells instead of eight. Recorded 19,200 renegotiation recovery
retains all nine previously decoded frames.

The waveform test can now encode/decode G.711 mu-law before the native receiver,
using an independent segmented codec with all-code reconstruction checks.
The extended 19,200 drift/distortion/PCMU case loses frames with the previous
phase gain and passes with this change. Both clock-offset signs and delayed-E
replay recover all expected frames. The equivalent 21,600 case also passes.
This revision passed CI. The previous completed soak used the integrator-only
revision `047d95e`.

The first phase-noise hardware trial passed authenticated PPP, a verified 32 KiB download in 7.492 seconds and all sixteen 1 KiB upstream payload checks. All uploads completed in 1.478–1.510 seconds (median 1.488), with no long pauses in this call. It disconnected cleanly after 38.215 seconds with zero Windows CRC or alignment errors. Native CP confirmed 49.333 kbit/s downstream. All 3,350 forwarded downstream primary payloads matched; the final server packet followed the caller BYE by 1.271 ms. The upstream suffix also matched. The 13,510-packet capture had zero kernel drops and no RTP sequence gaps, and all 18 response hashes were independently verified. This is one successful short trial; the new revision still requires sustained qualification.

CT105 now runs `816a07a`; the previous binary is saved as
`lm.pre-phase-noise`. The sustained retry has completed.

The phase-tracking build `816a07a` passed all 64 verified 32 KiB downloads (2 MiB) and 64 verified 1 KiB upstream requests over 751.781 seconds, then disconnected cleanly. All 129 response hashes were independently checked. Windows counted 60 CRC errors and three alignment errors. Native CP remained at 49.333 kbit/s downstream, including a completed renegotiation near the end. All 39,034 downstream primary RTP payloads and the forwarded upstream suffix (39,036 packets after 11 startup packets) matched across FreePBX. The 156,759-packet capture had zero kernel drops and no RTP sequence gaps. Post-call ATA counters included 23 lost segments, 16 idle segments, one FIFO drop, one underflow and two starvation events, without event timing. The call demonstrates sustained operation but does not establish improved downstream reliability or resolve the cause of the errors.

An exploratory 24,000 profile implements the 256-point M=8/K=24/q=3 mapping,
B1 and basic PCM path, but still loses frames in extended distorted-clock
tests. That work remains outside the committed receiver until qualified.


### 24,000 acquisition investigation (private prototype)

The extended drift/distortion/PCMU test still rejects the 24,000 prototype.
Symbol tracing identifies residual interference beyond the seven-tap
receiver's span. A fifteen-tap equalizer reduces the good timing lane's RMS
symbol error from 0.490 to 0.250 and passes both negative-clock-drift paths,
but positive-drift startup still loses frames. Supplying the known generated
clock passes all four direct/delayed-E signed-drift cases; this is a diagnostic,
not a deployable timing solution. The real loop overshoots during startup,
and residual near-neighbor interference remains while the equalizer adapts.

Immediate adaptation/clock-gain controls and joint carrier/equalizer fitting
do not pass the complete test. A private delayed trellis-decision feedback
prototype improves the positive-drift result to 191/192 exact frames, but
still fails and has not been qualified for deployment. No 24,000 prototype
or longer-equalizer change is included in the committed runtime. Acquisition
and timing/equalizer interaction remain the next receiver work.


### First 24,000 hardware trial (reverted after measurement)

Combining the private fifteen-tap trellis-feedback receiver with slower clock
integration passes the original extended signed-drift/distortion/PCMU test
in all four direct/delayed-E paths. A separate random-payload/timing-offset
case still loses a startup frame, so this is not a qualified implementation.
The independent 24,000 symbol and mapping test passes.

A controlled, reversible hardware trial authenticated PPP at 24,000 upstream
and native CP 49.333 kbit/s downstream. The 32 KiB download took 7.457 seconds;
sixteen distinct 1 KiB upstream checks took 1.415–1.469 seconds (median 1.444).
All 18 response hashes were independently verified. The call disconnected
after 36.943 seconds with zero Windows CRC/alignment errors. All 3,284
downstream primary RTP payloads and the forwarded upstream suffix matched.
The 13,248-packet capture had zero kernel drops and no RTP sequence gaps.
This is one short successful call; sustained qualification remains undone.

The server automatically returned to `816a07a` at 21,600 upstream after the
trial. Incoming PCM was saved privately for replay. The experimental receiver
remains outside the committed runtime pending broader startup qualification
and preservation of lower-rate behavior.


### Integrated 24,000 candidate: provisional training decisions

The remaining random-payload startup failure was resolved in the expanded
synthetic checks by using survivor decisions after eight pairs to train the
equalizer. Final decoded data still waits for all 63 lookahead pairs. Training
uses the saved normalized FIR input window with current coefficients; updates
are bounded by the existing 0.1 NLMS step and coefficient-norm limit. A 32-symbol
history retains the required 18-symbol feedback span, avoiding the private
prototype's much larger buffer.

Only 24,000 selects fifteen taps and provisional feedback; lower rates retain
seven taps and their prior adaptation rules. The 24,000 clock integrator uses
the slower lower-rate gain. M=8/K=24/q=3 yields 60 bits per mapping frame;
MP advertises drn=10 and only its corresponding capability bit. The code default
remains 4,800 and unsupported settings still fall back to it.

The original extended clock/distortion/PCMU case and random-payload sweep over
four fractional offsets and both 100 ppm clock directions recover every exact
PPP frame, including delayed-E replay. The independent constellation/trellis
and MP tests pass. Peek tests check read-only behavior, invalid ages/widths,
all encoder states, ring wrap, noisy known symbols and agreement with full-depth
output. Seven/fifteen-tap equalizer tests cover holdout validation, streaming
delay and invalid-state preservation. ASan/UBSan passes feedback history wrap
and reacquisition at five rates. The full native suite and the extended 19.2
and 21.6 regressions pass locally. CI and hardware results for this revision
must be recorded separately from the earlier private short-call success.


CI passed for `36577ed` (run 34535254143). Replay of the saved private 24,000
call yields the same 179 CRC-valid PPP frames with matching frame hashes.

The integrated `36577ed` hardware call passed authenticated PPP at 24,000 bit/s upstream and native CP 49.333 kbit/s downstream. The verified 32 KiB download took 7.463 seconds; sixteen distinct 1 KiB upstream checks took 1.411–1.459 seconds (median 1.429). All 18 response hashes were independently verified. The call disconnected cleanly after 36.673 seconds with zero Windows CRC/alignment errors. All 3,272 downstream primary RTP payloads and the forwarded upstream suffix matched across FreePBX. The 13,201-packet capture had zero kernel drops and no RTP sequence gaps.

CT105 now runs `36577ed` at 24,000 upstream. The previous binary and rate
setting are saved for rollback. Its first sustained run has now completed, as described below.


The first sustained 24,000-bit/s run on `36577ed` passed all 64 verified 32 KiB downloads (2 MiB) and 64 verified 1 KiB upstream requests over 636.996 seconds, then disconnected cleanly. All 129 response hashes were independently checked. Windows counted 20 CRC errors and zero alignment errors. Native CP remained at 49.333 kbit/s downstream through a completed renegotiation, and verified transfers continued afterward. All 33,290 downstream primary RTP payloads and the forwarded upstream suffix (33,292 packets after eleven startup packets) matched across FreePBX. The 133,709-packet capture had zero kernel drops and no RTP sequence gaps. Post-call ATA counters included 22 lost segments, 21 idle segments, one FIFO drop, one underflow and two starvation events, without event timing. This establishes one sustained run with recovery, not error-free maximum-rate reliability.

A private 26,400 prototype implements M=14/K=30/q=3 with 448 points and
66-bit frames. Its nine-bit labels use 16-bit arrays and 32-bit packed pair
history. Independent constellation/trellis/B1 tests, basic PCM and MP tests
pass, but combined drift/distortion/PCMU reception fails. Joint carrier and
fifteen-tap equalizer fitting restores the initial negative-drift cases while
still losing a frame in the following positive-drift case. A separate noisy
carrier diagnostic shows an eightfold frequency-RMSE improvement under the
tested distortion, a small regression without it, and about twentyfold local
fitting cost. This remains a private prototype, not a qualified next rate.


### Integrated experimental 26.4 kbit/s candidate

The development branch now includes the 448-point profile and preserves the
existing byte-sized lower-rate frame APIs. Internal labels use 16 bits and
packed trellis history uses 32 bits; the provisional peek supports widths
through nine bits. Only 26.4 kbit/s uses joint B1 carrier/equalizer fitting and
four-pair feedback; 24 kbit/s retains eight pairs. Final data still uses the
63-pair trellis lookahead.

`SIPFAX_V90_LINE_ECHO=1` enables experimental causal transmit-reference NLMS
in the pipe bridge. It starts with zero coefficients, uses 65 taps, a fixed
measured delay of 1428 samples and step 0.0005, and bounds coefficient energy.
Raw RX captures precede cancellation. The option is off by default. Its delay
is specific to the tested ATA path; automatic delay acquisition and drift
qualification remain incomplete. A no-echo synthetic test measures about
20 PCM units RMS of added adaptation noise, so it should not be enabled
indiscriminately.

The private predecessor passed one short hardware PPP call at 26.4 kbit/s
upstream / native CP 49.333 kbit/s downstream: 18 independently verified
transfer hashes, 36.583 seconds, zero Windows CRC/alignment errors. The
integrated source still needs deployment and sustained qualification.

The original random-payload/distortion/PCMU drift sweep passes. Additional
seeds expose isolated errors and remain unresolved. Reproduce with
`python3 tools/tests/v90-qam8-wave.py --26400 --long-clock --isi --pcmu --random-payloads --timing-sweep --seed 43127`
or the same command with `--seed 62091`. These cases previously recovered
191/192 frames at their first failing phase/drift setting. Passing CI and the
short hardware call do not establish error-free or full-rate V.90 conformance.


## Integrated 26.4 kbit/s sustained result and deployment

Revision `117ae76` passed CI run 34541455033 and the sustained hardware trial `c8bbf7b8-56f3-4049-8e15-c72483cfd548`. All 129 response hashes were independently verified: 64 downloads totaling 2 MiB, 64 upstream checks totaling 64 KiB, and a final PASS response. The call disconnected cleanly after 655.552 seconds. Windows reported 30 CRC errors and three alignment errors, with 195,672 bytes sent and 2,282,801 bytes received including protocol overhead.

Native CP began at 49.333 kbit/s downstream. A completed renegotiation at 388.975–391.006 seconds reduced downstream CP to 48 kbit/s, and verified transfers continued. The subsequent error counters stayed at 30/3 through the end. Upstream was configured and process-verified at 26,400 bit/s throughout the call. This proves one sustained transfer run with rate recovery, not error-free maximum-rate operation. The final S indication at 665.802 seconds was associated with hangup and was not another completed renegotiation.

The capture contained 137,445 packets, zero kernel drops and no RTP sequence gaps. All 34,218 downstream primary payloads matched through FreePBX/RED. All 34,220 forwarded upstream payloads matched the ATA sequence after eleven startup packets. Notebook connections were empty and SIPFAXRED registration was cleared after hangup; capture, HTTP fixture, trial and retrieval processes completed.

The reversible trial initially restored `36577ed`. After result verification, `117ae76` was installed for continued development with `SIPFAX_V90_UPSTREAM_RATE=26400` and `SIPFAX_V90_LINE_ECHO=1`. The active service, expected binary SHA-256 `badbecb9aff6f50c1201b9eacc69865273cb932e91ac5441da596b0f005142e2`, configuration and FreePBX availability were checked. The previous runtime remains saved as `lm.pre-26400-integrated`, with its configuration in `/tmp/v90-upstream-before-26400-integrated.conf`.

Full V.90 scope remains incomplete: higher upstream rates and symbol-rate coverage, broader reliability, general echo-delay acquisition/tracking, V.34 fallback and future concurrent calls still require work. The known additional synthetic-seed losses remain documented. No completion claim follows from this single sustained run.


### Automatic initial echo-delay acquisition

`SIPFAX_V90_LINE_ECHO=auto` starts with cancellation disabled and detects an
initial delay from past TX and raw RX audio. It evaluates at most 256 lags per
audio block, requires a quiet upstream window, rejects ambiguous correlation
peaks, and requires two qualifying searches to agree. Cancellation then starts
with zero coefficients while retaining reference history. The existing `1`
mode retains its measured 1428-sample delay; absent/other values keep echo
cancellation disabled.

The search uses 1024-sample windows and delays up to 7000 samples. It excludes
references not yet available under the bridge's RX-before-TX block ordering,
rejects stale/unsynchronized history, and rejects search ranges too narrow to
compare alternative peaks. It is an initial acquisition mechanism: it does not
yet reacquire a changing delay after lock. A call without an unambiguous quiet
window may remain uncancelled.

Local tests cover four independently generated delays, silence/noise/tone
rejection with unchanged output, echo reduction, fixed-mode regression and
bridge framing in automatic mode. CT105 ASan/UBSan checks pass across history
wraps, repeated bounded searches and mismatched RX state. Three recorded calls
select 1428 samples at 12.08 seconds; two formerly failing recordings each yield
ten valid PPP frames after correction. The standalone CT105 benchmark peaked
at 0.450 ms per search step; this is an observed timing, not a deadline guarantee.
Live automatic-mode hardware qualification is still pending. The deployed
`117ae76` remains in fixed-delay mode until that qualification.


## Sustained automatic-delay qualification and deployment

Revision `5151eb5` passed hardware attempt `44d5a229-929a-4750-8909-20e3a3877c44`. The live native process (11928) had the expected binary SHA-256 `b612a0c47def22ebc07fcf30d52515af3ba7f852df1f6d1bc7665cdee581487b`, upstream 26400 and echo mode `auto`.

All 129 response hashes were independently verified: 64 downloads of 32 KiB (2 MiB total), 64 upstream checks of 1 KiB (64 KiB total), and final PASS. The internet probe also succeeded. The connection lasted 656.014 seconds and disconnected cleanly. Windows reported 28 CRC and two alignment errors, with zero timeout, framing, buffer or hardware overrun errors. Its last counters were 193403 bytes sent and 2291939 received; these include protocol traffic.

Automatic acquisition selected 1428 samples at sample 96640 (12.08 seconds). An independent raw-audio 12–14-second correlation audit found the same delay, correlation -0.601796. The previous short automatic-mode call selected 1468 samples. This demonstrates initial acquisition across two live calls, not continuous tracking or universal path coverage.

Downstream CP initially negotiated 49.333 kbit/s. A completed renegotiation at 370.600–372.637 seconds reduced downstream to 48 kbit/s; transfers continued and error counters subsequently stayed at 28/2. Initial upstream B1 correlation was 0.9826 and recovered B1 correlation 0.9701. The final S/Sbar at 666.255 seconds accompanied hangup and was not another completed renegotiation. This run is successful sustained PPP with recovery, not error-free operation at 49.333 kbit/s.

The capture had 137535 packets, zero kernel drops and no RTP sequence gaps. All 34241 downstream primary payloads matched through FreePBX/RED; all 34243 forwarded upstream payloads matched after eleven ATA startup packets. The notebook was idle and SIPFAXRED registration was cleared after hangup. Trial, capture, HTTP fixture and retrieval processes are terminal.

After the reversible trial restored `117ae76`, `5151eb5` was deployed with upstream 26400 and automatic echo acquisition. Service activity, binary hash, configuration and FreePBX availability were verified. Rollback retains `117ae76` as `lm.pre-auto-5151eb5` and its fixed-mode configuration in `/tmp/v90-upstream-before-auto-5151eb5.conf`.

Remaining work includes higher upstream rates and symbol-rate coverage, error-rate reduction and broader reliability tests, ongoing echo-delay tracking, V.34 fallback and future concurrent calls. Additional synthetic-seed losses remain unresolved. The V.90 goal is still active and PR29 remains draft.


### Expanded seeded regression matrix

[Receiver regression investigation](receiver-regressions.md) records the full timing matrices: seed 43127 loses a frame at two phases; seed 62091 loses frames in all eight tested phase/clock combinations, identically for direct reception and delayed-E replay. The waveform test now supports `--keep-going` to report every mismatch while returning failure. Simple timing/carrier/equalizer gain experiments were rejected or remain insufficient; no receiver change was deployed.


## Half-symbol equalizer implementation

The 26.4 kbit/s PCM receiver now supplies a midpoint and a symbol-time sample to a 29-tap complex FIR. Both come from the existing quarter-sample matched-filter history. Midpoints are carrier-normalized at half a symbol before the current phase. The receiver keeps symbol-time outputs with seven symbols of lookahead, matching the prior 15-tap path's output alignment. Provisional trellis feedback remains four pairs; final decisions retain 63-pair lookahead.

B1 fitting uses 256 half-symbol observations and 128 known targets. It retains the 80-symbol fit and held-out validation, but increases the identity-directed ridge from 1e-6 to 1e-3 times trace/taps for the correlated half-symbol inputs. This corrected the prototype's frequent rejection of excessive coefficient norms; the norm-squared bound remains four. Bounded NLMS permits step 0.2 only for the 29-tap state. Seven/fifteen-tap paths keep their original 0.1 maximum, training API, and operation. Symbol-only 26.4 tests retain the 15-tap path unless midpoint input is supplied.

The final integrated receiver passes all 192 exact expected frames in each of eight phase/clock cases, for both direct reception and delayed-E replay, under the default payload and seeds 43127, 62091 and 98017. This closes the documented two-seed synthetic losses; it does not prove all channels or rates. The independent 24 kbit/s long-clock/ISI/PCMU/random-payload matrix also passes.

Dedicated half-symbol tests verify startup/output alignment, independently distorted held-out channel recovery, incompatible training modes, invalid input and step rejection with unchanged state. The full native suite, existing seven/fifteen-tap equalizer tests and clean native build pass. CT105 ASan/UBSan checks pass for half-symbol history wrapping, reacquisition and invalid midpoints. A CT105 normal-build replay recovered 192 frames across 567 blocks; total processing time was 439.195 ms and the largest 20 ms block took 12.868 ms. This is one timing observation, not a hard deadline guarantee.

No hardware deployment has yet been made for this change. CT105 continues to run `5151eb5` with automatic echo-delay acquisition. The next gate is CI followed by a reversible short hardware call, then sustained transfers if the short call qualifies. Full V.90 conformance, higher upstream rates/symbol rates, broader reliability, ongoing echo-delay tracking, fallback and future concurrent calls remain unfinished.


## 28.8 kbit/s receiver integration

The development source now supports configured 28800 bit/s upstream at 3200 symbols/s: M=12, K=28, q=4, 768 constellation points and 72 bits per mapping frame. Ten-bit labels fit the existing packed pair history. MP advertises rate 12 and capability bit 46; independent training/MP decoding verifies the rate, capability and CRC. Symbol-rate selection remains 3200; this does not add 3000 or 3429 symbols/s.

The first private prototype lost an early large PPP frame under imposed ISI and negative clock drift. On cached independent PCMU inputs, lowering initial Gardner phase gain from 0.1 to 0.05 closed all eight timing cases; 0.15 failed all eight. Ridge and frequency-integrator sweeps did not close the matrix. The integrated change applies only to the 28.8 profile, preserves the integrator, and retains the existing steady-state phase gain of 0.02. Earlier-feedback and half-symbol carrier-fit experiments were rejected.

The integrated default and seeds 43127, 62091 and 98017 pass all 192 expected frame hashes in all eight phase/clock cases, for both direct reception and delayed-E replay. Symbol/mapping tests cover all 768 points and 16 trellis states, shell boundaries, ten-bit provisional feedback, continuous frames and reacquisition. The native synthetic/fixture suite passed; lower-rate mapping checks and the 26.4 seed-62091 waveform regression pass. An additional invocation of the recording-dependent phase4 test without its required recording argument was invalid and removed from CI; the independent renegotiation test supplies the MP coverage.

CT105 ASan/UBSan feedback history/reacquisition checks pass, including 28.8 half-symbol input. A cached PCMU benchmark recovered 192 frames across 529 blocks, with 547.155 ms total receiver time and a 12.966 ms peak for one 20 ms audio block. This is a workload measurement, not a hard real-time guarantee.

Hardware qualification is outstanding. The deployed runtime remains `5151eb5`, upstream 26400 with automatic initial echo acquisition. No 28.8 hardware success is claimed.


## 31.2 kbit/s source qualification

The development receiver now includes a configured 31200 bit/s profile at 3200 symbols/s: M=10, K=26, q=5, 1280 points, 78-bit mapping frames and eleven-bit trellis labels. Packed pair history remains within 32 bits. Independent MP decoding verifies rate 13, capability bit 47 and CRC; the symbol rate is unchanged.

The initial 0.05 timing phase gain passed the default, 43127 and 62091 waveform matrices, but seed 98017 lost one frame at phase 0.5/+100 ppm in both direct and delayed-E reception. Using 0.02 from startup only for 31.2 kbit/s closes that failure. The final default and all three seeded matrices recover all 192 expected frame hashes in every phase/clock case, in both receiver modes. The frequency integrator and lower-rate phase gains are unchanged.

All 1280 constellation points, all trellis states, shell boundaries, continuous 78-bit frames, carrier/noise, source timing and reacquisition pass the independent symbol/mapping tests. The native synthetic/fixture suite, MP negotiation checks, 26.4/28.8 mapping regressions and the 28.8 seed-98017 waveform regression pass locally. CI adds all four 31.2 waveform matrices and memory-safety coverage for the extended feedback history. CT105 timing/memory checks and hardware qualification remain outstanding. This source work does not alter the ongoing 28.8 hardware trial or establish deployed 31.2 support.
