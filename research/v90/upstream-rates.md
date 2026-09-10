# Extending the V.90 upstream receiver

The code default remains 4,800 bit/s at 3,200 symbols/s. The experimental
`SIPFAX_V90_UPSTREAM_RATE=7200` selects the eight-point receiver; `9600` selects
the twelve-point receiver; `12000` selects the experimental twenty-point receiver; `14400` selects
the experimental thirty-two-point receiver; `16800` selects the experimental
fifty-six-point receiver; `19200` selects the experimental ninety-six-point
receiver with one uncoded bit per symbol; `21600` selects the experimental
160-point receiver with two uncoded bits per symbol.
MP advertises only the configured rate. Unset or
unsupported values select 4,800. Hardware has verified 7,200 and 9,600 modes;
12,000 initially failed its hardware upload, then passed after B1 equalization.
The lab currently selects the experimental 21,600 receiver after its first
successful short hardware test. Upstream throughput still needs improvement.
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
