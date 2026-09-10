# Extending the V.90 upstream receiver

The code default remains 4,800 bit/s at 3,200 symbols/s. The experimental
`SIPFAX_V90_UPSTREAM_RATE=7200` selects the eight-point receiver; `9600` selects
the twelve-point receiver. MP advertises only the configured rate. Unset or
unsupported values select 4,800. The 9,600 mode has synthetic validation only.
The lab server now explicitly selects 7,200 after the short hardware trials
below; broader qualification is required before changing the code default.

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

`v90_qam8_b1_init` generates the selected-rate B1 with zero encoder state and
the final data frame's superframe inversions. Its bounded symbol-domain
correlator reports the B1 end, carrier phase, and gain. Tests independently
generate all 288 scrambled bits and 128 symbols, then check noisy acquisition,
phase/gain changes, and negative/reset controls. The continuous stream replays B1 through the trellis and descrambler, tracks
carrier/gain against eight-point decisions, and aligns mapping frames. Ten
matched-filter timing lanes feed this receiver in `v90upstream.c`. Invalid
symbols drop lock; reacquisition requires another B1. A rejected shell resets
the affected lane’s PPP framing/descrambler, which then self-synchronizes.

Remaining integration work:

- Sustained hardware qualification of timing recovery and 9,600-bit/s calls.
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
The development server now runs this experimental rate; sustained 9,600-bit/s
qualification remains outstanding. The adaptive 7,200-bit/s binary is retained
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
A controlled downstream-rate comparison is the next reliability experiment.

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
differential-history recovery and smaller-output cross-rate rejection. This
does not yet enable 12,000-bit/s B1 acquisition or native audio/MP negotiation.
