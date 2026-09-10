# Extending the V.90 upstream receiver

The code default remains 4,800 bit/s at 3,200 symbols/s. The experimental
`SIPFAX_V90_UPSTREAM_RATE=7200` selects the eight-point receiver; `9600` selects
the twelve-point receiver; `12000` selects the experimental twenty-point receiver; `14400` selects
the experimental thirty-two-point receiver; `16800` selects the experimental
fifty-six-point receiver; `19200` selects the experimental ninety-six-point
receiver with one uncoded bit per symbol.
MP advertises only the configured rate. Unset or
unsupported values select 4,800. Hardware has verified 7,200 and 9,600 modes;
12,000 initially failed its hardware upload, then passed after B1 equalization.
The lab currently selects the experimental adaptive 16,800 receiver after its
first successful hardware test. Broader qualification is required before changing
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
capture had no RTP sequence gaps or kernel drops. CT105 retains 16,800, with
the validated 14,400 binary saved for rollback. Sustained 16,800 qualification
remains outstanding.

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
uncoded bits remain in parser order throughout. Hardware validation at 19,200
remains pending; the deployed server is still at 16,800.
