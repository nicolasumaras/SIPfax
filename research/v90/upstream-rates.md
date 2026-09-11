# Extending the V.90 upstream receiver

## Current qualification and next implementation gate (2026-09-11)

CT105 is running `9be7827`, with a ten-second initial PPP grace period that requires a well-formed LCP Configure packet before disabling recovery. The upstream initial ceiling is 28800 bit/s, both supported symbol rates are available, and initial echo acquisition is automatic. Its verified binary SHA256 is `1195c6f8b6413e0b72c5822283a5820c4d38f19a4508ae1b8cebc81872c5b14a`. Normal negotiation passed at 3200/28800 without a startup retrain, with internet access and 18 transfer hashes. Downstream CP remained 49.333 kbit/s. The previous `efc00dc` binary/configuration and native source provenance are retained. Historical entries below describe earlier deployments.

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


### Startup recovery after B1 without PPP

Startup now watches for the first CRC-valid upstream PPP frame after B1. If none arrives within five seconds plus two round-trip delays, it reduces the current upstream rate ceiling by 2400 bit/s and initiates a retrain. This is a local recovery policy, not a V.90 protocol deadline. The original configured maximum remains available for diagnostics; the lower ceiling survives later retrains and limits both symbol-rate offers. The policy never downshifts below 4800 and is disabled after valid upstream data has ever arrived on the call, during rate renegotiation, before E, or during CPs silence.

This change addresses the observed high-rate calls that trained but waited until Windows error 721 without delivering data. It does not fix the high-rate decoder or prove that the notebook will complete a retrain before its PPP timeout. All 32 native tests and the build pass. CT105 ASan/UBSan passes 285 deadline/rate/floor/guard cases and the startup profile matrix. The first hardware recovery trial with `aa1ac2f` (binary SHA256 `21f29387e6171ea977444bc1d448420b87668683197e93dd09a5ddb6b52ec7f5`) passed. Attempt `0b74a352-c35a-4c88-9d71-718b949487fa` started at 3000/high, 28800 bit/s, detected B1 without PPP, and reduced the ceiling to 26400 at startup time 21.523250 seconds. The notebook accepted the retrain on the same call and established PPP. All 18 response hashes and internet access passed, with zero Windows modem errors over 36.853 seconds of connected PPP. Native CP remained 49.333 kbit/s downstream across both trainings.

The 16442-packet capture had zero kernel drops and no RTP sequence gaps. All 4080 downstream primary payloads matched; 4093-to-4082 upstream payloads matched after the eleven-packet startup suffix. Raw audio and logs are retained privately. The wrapper restored the verified `dcb450f` baseline after disconnect. This demonstrates one successful automatic startup recovery, not sustained qualification, general rate adaptation or a fix for the highest-rate receiver. Sustained recovery qualification and high-rate diagnosis remain next.


### Sustained recovery and deployment (2026-09-11)

CI run 34562278514 passed for `aa1ac2f`. Sustained attempt `3eab0b20-5a23-4125-bed3-6689111ae77c` completed all 129 hashes in 129 attempts, with no failed requests, on one PPP connection over 581.627 seconds. Windows reported zero modem errors. Native logs confirm one startup downshift from 3000/high/28800 to 3000/high/26400; CP remained 49.333 kbit/s across both trainings. The 125815-packet RTP capture had zero kernel drops or sequence gaps. All 31322 downstream primary payloads matched; 31335-to-31324 upstream payloads matched after the eleven-packet startup suffix. The complete TCP capture contains 4426 packets and all 129 expected flows in order, with zero retransmitted downstream segments.

After the wrapper restored the prior baseline, `aa1ac2f` was permanently installed with upstream ceiling 28800, echo auto and no forced symbol rate. Post-deployment attempt `faa8730c-626c-4027-89e0-1dec36836aeb` selected 3200/high and also recovered once from 28800 to 26400. All 18 hashes and internet access passed over 36.652 seconds, with zero modem errors and CP 49.333 kbit/s. Its 16373-packet capture had zero drops/gaps; 4063 downstream payloads matched and 4077-to-4065 upstream payloads matched after a twelve-packet startup suffix. This shows the intermittent 28800 startup problem can also occur at 3200; it is not confined to q=5 or the 3000 profile.

The live service and installed hash were rechecked after the clean disconnect. All fixture/capture processes are terminal. Rollback files: `/opt/sipfax/vendor/linmodem/lm.pre-28800-aa1ac2f` and `/tmp/v90-upstream-before-28800-aa1ac2f.conf`. Higher-rate receiver diagnosis, wider interoperability, downstream maximum-rate qualification, fallback and future concurrency remain unfinished.


### Downstream ceiling trials on deployed aa1ac2f (2026-09-11)

A reversible `SIPFAX_V90_MAX_BPS=56000` trial, attempt `be9d1376-8bda-41fe-90a1-ba32d123805a`, negotiated CP 54.667 kbit/s initially and 56 kbit/s after the upstream startup downshift from 28800 to 26400 at 3200 symbols/s. PPP and the internet probe succeeded. The first 32 KiB download then timed out at 29996 ms with 21601 body bytes and nine reported CRC errors. Native logs show downstream rate renegotiation to 53.333 kbit/s during the stall. This failed the short transfer gate; it is not a qualified 56 kbit/s connection. The 16150-packet capture had zero capture drops/gaps, 4009 matching downstream primary payloads and 4011 matching upstream payloads. No TCP capture was taken for this short trial, so a specific TCP-loss mechanism is not established.

A second trial capped at `53334`, attempt `f49bf5ae-3c09-44ab-ab01-1b1b96ff6469`, selected CP 53.333 kbit/s both before and after one upstream downshift to 26400 at 3200 symbols/s. Internet access and all 18 transfer hashes passed over 36.112 seconds with zero modem errors. The download took 6962 ms; median upload time was 1381 ms. The 16302-packet capture had zero drops/gaps, 4045 matching downstream payloads and 4058-to-4047 matching upstream payloads after the eleven-packet startup suffix.

Both calls disconnected cleanly; their temporary override was removed and the active service returned to the qualified 49334 ceiling. Raw RX/TX and RTP captures are retained privately; all trial/fixture/capture processes are terminal. Next is sustained qualification at 53.333 kbit/s. The 56 kbit/s stall remains recorded as a failure.


### Sustained 53.333 ceiling trial (2026-09-11)

Attempt `79c76f40-2547-4b29-bde9-04dd6ffa17e6` completed all 129 target hashes in 129 attempts on one PPP connection, with no failed application requests. The call began with CP 53.333 kbit/s, recovered upstream from 28800 to 26400 at 3200 symbols/s, and renegotiated downstream to 50.667 kbit/s at Phase4 time 229.730375 seconds. Final PPP duration was 768.014 seconds, with 83 CRC and eight alignment errors. Windows continued reporting the original connection speed; the native CP records establish the rate change.

The 163191-packet RTP capture had zero capture drops/gaps. All 40633 downstream primary payloads matched; 40646-to-40635 upstream payloads matched after the eleven-packet startup suffix. The TCP capture contained 4682 packets and all 129 expected flows in order, with 230 retransmitted downstream segments. This proves complete transfers through downstream renegotiation, not error-free sustained 53.333 operation.

For the same transfer workload, the recent 49.333-ceiling recovery run completed in 581.627 seconds with zero modem errors and no downstream TCP retransmissions. The higher-ceiling run was about 32% longer. This is a comparison of two observed calls, not a general causal benchmark. Keep the deployment ceiling at 49334. The temporary override was removed, the active service verified, and all trial/capture/fixture processes are terminal. Raw audio, RTP and TCP evidence are retained privately.


### Correction: normal PPP startup delay versus decode timeout

A recorded-data comparison found a defect in the five-second startup downshift policy added in `aa1ac2f`. The successful 3200/28800 control recording delivers its first CRC-valid PPP frame 5.819125 seconds after B1 detection. Its measured RTD is 178.625 ms, so a five-second-plus-two-RTD guard expires at 5.357250 seconds, before that legitimate first frame. `[v90p4]` E/B1 timestamps and `[v90data]` PPP timestamps use different sample origins; subtracting those log values directly is invalid.

The guard now permits ten seconds plus two RTDs after B1. A regression explicitly preserves a six-second quiet startup, then verifies that arriving data disables later downshift. All 286 guard/deadline/profile cases pass under CT105 ASan/UBSan; startup, negotiation and rate-selection suites and the native build pass. Hardware validation of the corrected grace period is next.

This supersedes the interpretation that the `aa1ac2f` 3200/28800 downshifts prove a receiver failure: the watchdog can itself interrupt normal startup. The earlier 3000/28800 and 3200/31200 calls failed before this watchdog existed and remain unresolved. In a 15–32-second recorded replay, the successful 3200/28800 and 3000/26400 controls each recover 17 CRC-valid frames; the interrupted 3200/28800 and original failed 3000/28800 recordings recover none. A successful and an interrupted 3200/28800 lane both decode all interior B1 labels correctly. Substituting known B1 labels during provisional equalizer feedback leaves all four recordings' recovered frame hashes unchanged, so that experiment is not adopted.

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

### Ten-second PPP grace hardware qualification (2026-09-11)

Native commit `efc00dc` passed 286 startup recovery cases, including the recorded six-second initial PPP delay regression, with CT105 ASan/UBSan. Targeted startup, 512 negotiation cases, rate selection and native build also passed.

Normal-negotiation attempt `f321fd02-757d-4266-99f4-bbcf99bdbd1f` selected 3200/high/28800. First CRC-valid PPP arrived at upstream sample time 5.871625 seconds, without any startup retrain or rate reduction. Internet access and all 18 transfer hashes passed over 35.501 seconds of PPP, with every Windows error counter zero. CP was 148000/3 bit/s. Capture: 12962 packets, zero kernel drops or RTP sequence gaps, all 3213 downstream primary payloads exact, 3226-to-3215 upstream payloads exact after eleven startup packets.

Forced-3000 attempt `3daba540-1037-41a8-9c69-71abb79f728a` selected 28800 and triggered one initial PPP timeout at startup time 26.527500 seconds, reducing the ceiling to 26400. The same call established PPP and passed internet access plus all 18 hashes over 56.100 seconds. Windows counted one alignment error, zero CRC errors and zero other errors. Capture: 17148 packets, zero kernel drops or RTP sequence gaps. All 4251 forwarded downstream payloads match; the final server packet at epoch 1789104823.561761 followed the ATA BYE at 1789104823.556899 and the server-leg BYE at 1789104823.559675. Upstream 4264-to-4253 payloads match after eleven startup packets. The strict equal-length audit initially rejected the hangup tail; the SIP timing accounts for it explicitly.

After both reversible trials restored the preceding binary, `efc00dc` was deployed with normal symbol negotiation and the existing 49334 downstream ceiling. This corrects premature 3200 startup recovery; it does not resolve the original 3000/28800 receiver failure or qualify full V.90 conformance. Higher rates, broader interoperability, fallback and concurrency remain unfinished.

### Recorded 3000/28800 receiver diagnosis (2026-09-11)

Full CI run 34565749878 passed for deployed native `efc00dc`. The active binary, normal symbol negotiation, 28800 upstream ceiling and 49334 downstream ceiling were reverified. No experimental decoder variant below was deployed.

The current successful 3200 recording and failed initial 3000 training were retained privately from native processes 17962 and 18219. Causal echo cancellation acquired the same 1428-sample delay at sample 96640 for both. Replaying seconds 15–32 recovers 20 CRC-valid frames from the successful call and zero from the initial failed 3000 phase; this window excludes the later successful 26400 retrain.

`audit_upstream_b1.py` now provides a reproducible diagnostic for known training labels, per-bit error counts, fit/held-out residuals and simple distortion models. It compiles a temporary instrumented receiver and reports aggregates without decoded payloads. Example with a private echo-corrected signed 16-bit little-endian 8 kHz recording:

```sh
python3 research/v90/audit_upstream_b1.py 28800 3000 corrected.s16 audit.json --start 15 --end 32
```

The old failed 3000 recording has held-out B1 MSE 0.734–0.844 across locked lanes; the latest failed recording's lane 5 has MSE 0.762 and seven wrong interior training labels. Errors include the two low label bits protected by the trellis, so a purely uncoded shell-bit explanation is insufficient. These residuals are in normalized constellation units, not an end-to-end line SNR measurement. Complex linear, conjugate-linear and radial-cubic corrections fitted on symbols 7–86 do not consistently improve the held-out symbols 87 through length-minus-eight. They do not identify a static nonlinearity repair.

Private recorded experiments also tested four-point causal cubic interpolation in place of linear quarter-sample interpolation, matched-filter rolloff values 0.05/0.1/0.15/0.2/0.3/0.5, and carrier-frequency scoring with the actual 29-tap half-symbol equalizer rather than the existing 15-tap symbol equalizer. The two earlier passing controls retained 17 CRC-valid frames each in the replay window; the original failed 3000 and prematurely interrupted 3200 records remained at zero. The carrier-fit comparison additionally retained 20 frames in the new passing control and zero in the new failed initial 3000 phase. None supplies evidence for deploying a receiver change. Rejection of these specific candidates does not rule out all timing, filtering, nonlinear or carrier problems.

The portable audit was rerun against the latest failed recording and reproduced the private diagnostic JSON exactly. Next receiver work should measure decision/equalizer divergence through the transition from known B1 to data, using both failed recordings and the passing controls, rather than treating CRC-only parameter sweeps as a sufficient diagnosis.

### Training-to-data trace and LCP recovery guard (2026-09-11)

Private 1000-symbol aggregate traces show failed 3000/28800 equalizer output energy falling from about 672 during B1 toward 391–433 by the 24th data bin, with tap norm falling from 0.95–0.98 to 0.77–0.83. Passing 3000/26400 and 3200/28800 controls retain stable energy and tap norm. Nearest-point distance alone masks the failure because a collapsed constellation can remain near other valid points.

Freezing adaptation reduces recovered frames in passing controls and does not recover either failed recording. Half-symbol NLMS step 0.02 or preserving tap norm recovers two structurally valid LCP Configure-Requests (29-byte frame, ff03/c021 header, 23-byte LCP length) from the older failed recording, but none from the latest failure. Steps 0.01, 0.03, 0.04, 0.05, 0.075 and 0.1 also recover those two requests; 0.015 and 0.025 do not. The 0.04 case additionally yields a five-byte FCS coincidence with an invalid PPP protocol field. Raw CRC counts therefore overstate useful decoding. No adaptation change is selected for deployment.

A reversible hardware trial preferred the supported 1800 Hz low carrier at 3000 symbols/s, changing only the INFO1d carrier choice. Attempt `1a2d1f78-2cf4-4088-b70a-4575235b6c34` accepted 3000/low/28800 and B1 correlation 0.9751, but still required one startup retrain to 26400 at startup time 26.528750 seconds. It passed internet access and all 18 hashes over 55.891 seconds, with one alignment error and no CRC errors. The 17080-packet capture had zero drops/gaps, 4238 exact downstream primary payloads and 4251-to-4240 upstream payloads matching after eleven startup packets. The prior `efc00dc` binary and normal configuration were restored. Lower carrier alone does not resolve this failure.

The FCS coincidence exposed a separate startup watchdog defect: any received FCS-valid frame permanently disabled recovery, including malformed protocol data. The watchdog now requires an uncompressed, well-formed LCP Configure packet (codes 1–4), including bounded length and complete option TLVs, before disabling initial recovery. Other FCS-valid frames continue to pppd unchanged for protocol handling; frame delivery and duplicate suppression remain intact. This follows RFC 1661 sections 2, 5, 6.5 and 6.6: invalid protocol fields are handled as unrecognized protocols, while initial LCP is uncompressed. Sources: https://www.rfc-editor.org/rfc/rfc1661.html and https://www.rfc-editor.org/rfc/rfc1662.html.

Validation before hardware qualification: all 32 native tests and build pass; 31 focused LCP framing cases pass; CT105 ASan/UBSan passes 288 recovery cases, including FCS-only traffic at the recovery deadline and preservation through retraining. Local sanitizer linking is unavailable because the installed libasan path is missing; CT105 supplied the sanitizer result. The receiver diagnostics and low-carrier experiment remain private and were not applied to the production receiver.

### LCP watchdog hardware qualification and deployment (2026-09-11)

Native `9be7827` attempt `78e72b18-d771-4bec-b901-2b8d6df7fe39` selected 3200/high/28800 without a startup downshift. First upstream PPP was at 5.879125 seconds; CP remained 148000/3 bit/s downstream. Internet access and all 18 transfer hashes passed, with every modem error counter zero over 36.092 seconds of PPP. The 13080-packet RTP capture had no capture drops or sequence gaps; 3242 downstream primary payloads matched exactly and 3255-to-3244 upstream payloads matched after eleven startup packets. The notebook disconnected cleanly.

After the reversible trial restored `efc00dc`, the tested `9be7827` binary was installed permanently. Its hash, active service, normal symbol negotiation and 28800/49334 ceilings were verified, and its source archive/build manifest were retained with matching hashes on CT105. Rollback files are `/opt/sipfax/vendor/linmodem/lm.pre-9be7827` and `/tmp/v90-upstream-before-9be7827.conf`. All capture/fixture processes are terminal.

The B1 audit now separately reports `initial_lcp_seen`, avoiding confusion between raw FCS coincidences and recognizable startup traffic. Recorded control and failure checks returned 20/true and 0/false respectively. This change hardens recovery; it does not resolve 3000/28800 decoding. The adaptation lead remains incomplete, and the low-carrier trial did not solve it.

### Replay-origin and echo checks (2026-09-11)

The deployed `9be7827` service and installed binary hash were reverified before these offline tests. No server configuration or decoder change was made.

Repeating the 15–32-second replay with start offsets 0–7 samples shows that the step-0.02 adaptation lead is not robust. The original failed 3000 recording yields two structurally consistent LCP Configure-Requests at offsets 0, 2, 3, 4, 6 and 7, but none at offsets 1 and 5. The newer failed recording yields none at any offset. Both passing controls preserve their 17/20 frame counts and four initial LCP packets at every offset for baseline and step-0.02. This sensitivity is further reason not to deploy the adaptation change.

Replacing only the trellis input pairs during known B1 with their exact reference points leaves all six baseline counts unchanged (20, 0, 17, 0, 0, 17). Combining it with step-0.02 still recovers only the two requests in the older failure. This experiment does not support training-state corruption as a sufficient explanation.

Causal echo replay at NLMS steps 0, 0.0005, 0.001, 0.002 and 0.005 never recovers initial LCP in either failed recording. Disabling adaptation also loses both passing controls; the deployed 0.0005 and 0.001 recover 17/20 frames; 0.002 yields 15/0 and 0.005 yields 0/0. Using a transmit reference quantized through the same PCMU encode/decode mapping preserves the 17/20 control counts at 0.0005 and 0.001 but still recovers neither failure. Faster echo convergence or this reference substitution alone therefore has no demonstrated repair benefit. Aggregate results are retained in `receiver-experiments-2026-09-11.json`; private audio and decoded payloads are excluded.

Next hardware experiment: the existing INFO1d implementation always requests flat transmit pre-emphasis. V.90 section 6.4 and Table 9 provide for analogue transmit pre-emphasis selected by the digital modem; V.34 section 5.4 Tables 3/4 define indices 0–10. Unlike receiver-only gain changes, this changes the caller's transmitted spectrum. A bounded, reversible 3000-symbol/s test with a supported nonzero index can establish whether transmit spectral shaping helps this path. No such setting has yet been applied or qualified.

### Transmit pre-emphasis hardware trials (2026-09-11)

Three reversible 3000/high trials based on native `9be7827` tested pre-emphasis index 2, index 8, and index 2 combined with half-symbol NLMS step 0.02 limited to 3000/28800. Each wrapper restored the installed `9be7827` binary and normal configuration; the final active service, binary hash and 28800/49334 ceilings were reverified. All calls, fixture and capture processes are terminal. No experimental preference or adaptation change was deployed. Full CI run 34567715643 passed for deployed `9be7827`.

Both pre-emphasis-only builds passed 512 negotiation cases, including the requested filter field, CRC and retrain preservation. The combined build passed the targeted 3000/28800 PCMU random-payload waveform test (seed 98017). Its first temporary test invocation could not find the independent shell reference module; rerunning with the existing tests directory on PYTHONPATH passed. Transmit recordings independently demodulated to the complete expected 109-bit INFO1d with a valid CRC for indices 2 and 8; matching scores were 0.999960 and 0.999870. This verifies the outgoing request, not the analogue modem's exact spectral response.

All three live calls selected 3000/high/28800 but produced no recognized initial PPP before recovery, then retrained once to 26400 and passed internet access and all 18 transfer hashes. Each recorded one Windows alignment error, zero CRC errors and zero other errors. Downstream CP remained 148000/3 bit/s.

| Variant | Attempt | PPP duration | Capture packets | Downstream exact payloads | Upstream source / forwarded |
|---|---|---:|---:|---:|---:|
| Index 2 | f0c9965b-4bea-4435-81ca-e15362639a57 | 55.410 s | 17004 | 4219 | 4232 / 4221 |
| Index 8 | dc1b2d9a-8374-4e1d-8680-8572e94ff36c | 56.030 s | 17133 | 4250 | 4264 / 4252 |
| Index 2 + slow adaptation | 127996c1-11e1-4d65-9b01-72159a142c2f | 56.041 s | 17128 | 4249 | 4262 / 4251 |

Every capture had zero kernel drops and RTP sequence gaps. Forwarded primary payloads match exactly, with upstream startup suffixes of eleven, twelve and eleven packets respectively. Aggregate hardware results are in `preemphasis-trials-2026-09-11.json`; raw recordings remain private.

In the 15–32-second replay, the index-2 B1 audit's best lane had held-out MSE 0.540 and zero wrong interior labels; index 8 had MSE 1.082 and thirteen wrong interior labels. Both baseline replays recovered zero frames. Slower adaptation steps 0.02/0.05/0.1 recovered two LCP Configure-Requests from the index-2 recording, none from index 8 or the previous failed flat-pre-emphasis recording, and preserved the 17/20 passing-control frame counts. Despite that offline lead, the combined live trial still failed to start at 28800. These selected settings do not establish that every pre-emphasis index is ineffective.

Next diagnosis: determine from recorded signals whether the interval after B1, before the first PPP request, is predictable scrambled idle data. If verified, it may provide more supervised equalizer training than the 40 ms B1 window. Do not assume that idle persists for a fixed duration or force known labels across unknown traffic.

### Post-B1 traffic is V.42 detection, not prolonged idle (2026-09-11)

Extending the B1 scrambled-ones reference to 4096 symbols disproves a long fixed-idle assumption. In passing controls and failed recordings the reference initially agrees, then diverges sharply within the first few hundred symbols. For example, the passing 3200 lane has MSE 0.405 over symbols 128–247, then MSE 749 over 248–727; its first PPP packet still arrives much later. Forcing known labels throughout that interval would corrupt real traffic.

Inspecting the early descrambled asynchronous characters identifies alternating 0x11 and 0x91 in both passing controls: each contains 683/682 of those characters and 1364 adjacent alternations in the 15–25-second replay. These correspond to V.42 originator detection (ODP), rather than user PPP traffic. The index-2 failed recording has 252 adjacent alternations on one lane; the newer flat-pre-emphasis failed recording has only one.

The new standalone `v42detect.c/h` helpers implement recognition of four alternating-parity DC1 characters with 8–16 intervening mark bits, and generate ten E/NUL answerer detection patterns indicating that modem error correction is not desired. They do not implement or advertise LAPM. The bit-level detector recognizes ODP 65.625 ms after B1 in the passing 3000/26400 and index-2 failed recordings, and 106.625 ms after B1 in the passing 3200/28800 recording. It does not recognize ODP in the newer failed flat-pre-emphasis recording. Aggregate evidence is in `idle-and-odp-2026-09-11.json`.

`v42-detect.py` passes 209 framing/parity/gap cases, rejects isolated corruption and too few characters, and compares all 360 generated reply bits with an independent literal Table-3 pattern. It is included in the native CI test list. These helpers are intentionally not connected to the running modem yet: runtime integration must gate initial DTE output, respect the 750 ms T400 default, send the complete reply through the normal scrambling/PCM path, and preserve established-call behavior through retrains and rate renegotiation. Detection and a successful reply must never substitute for actual LCP startup in the existing recovery watchdog.

Source: ITU-T V.42 (03/2002), sections 7.2.1.2, 7.2.1.3, Table 3 and 9.1.1, https://www.itu.int/rec/T-REC-V.42. The observed detection traffic explains why initial PPP timing cannot be modeled as pure idle; whether a reply shortens this notebook's startup or improves 3000/28800 reception remains to be verified in hardware. Production remains native `9be7827`.


### Opt-in V.42 decline integration (2026-09-11)

`681ea4d` connects the detection helpers to each upstream lane and gates initial downstream DTE bits when `SIPFAX_V90_V42_DECLINE=1` is explicitly set. Mandatory B1d remains unchanged. ODP starts the complete ten-pattern E/NUL reply; otherwise valid LCP releases DTE early, or the 750 ms post-B1 timeout releases it. Established PPP sessions bypass detection across retrains and renegotiation. ODP alone never satisfies the startup recovery watchdog. The option defaults off.

All 34 native tests and the native build pass. CT105 ASan/UBSan passes the integration test and 289 startup recovery cases. The native candidate binary SHA-256 is `1c9670ce97a1bd53c450b599dbd75452acd7625870e4a5333f137ef71f4c4c30`.

Normal hardware control `f89d4d0c-8e42-41df-85d4-d4582caf407c` selected 3200/28800 upstream and 148000/3 bit/s downstream without recovery. ODP was detected at upstream sample 1333 and the E/NUL reply was emitted, but the first PPP frame still arrived at 5.871625 seconds: no measured startup improvement. Internet access and all 18 transfer hashes passed, with zero reported modem errors over 35.992 seconds. The capture contained 13066 packets, zero kernel drops or sequence gaps, 3238 identical forwarded downstream payloads and 3251/3240 upstream payloads with the eleven-packet startup suffix matching exactly. The call disconnected and baseline `9be7827` was restored.

The paired 3000/28800 experiment added the previously tested index-2 pre-emphasis request to `681ea4d` (private binary SHA-256 `e0603c969594fa28812b65ecc3b81cc3936c278d5a95c9671228fbfcab0fa927`). Attempt `87c9b7ed-ad16-4ab8-a666-0b4749aaddba` detected ODP at upstream sample 1240 and sent E/NUL, but still required the 28800-to-26400 recovery at startup time 26.522125 seconds. Internet access and all 18 hashes passed after recovery, over 55.911 seconds with one alignment error and zero CRC/other errors. The 17087-packet capture had zero kernel drops or sequence gaps. All 4239 forwarded downstream primary payloads match; one final server packet at epoch 1789109205.803530 follows the ATA BYE at 1789109205.800769 and was not forwarded. Upstream 4252/4241 payloads match after the eleven-packet startup suffix. The call, capture and fixture were closed; the baseline binary and configuration were restored.

The handshake experiment has not demonstrated either faster PPP startup or a fix for 3000/28800. Leave it disabled by default. The next receiver investigation should use the decoded ODP structure as limited, validated evidence for locating the loss of decoding; it must not assume continuous idle bits across this traffic. Any proposed training change must preserve independent PPP decoding and passing controls.


### ODP timeline and supervised-feedback diagnosis (2026-09-11)

A 200 ms timeline of descrambled asynchronous bits places the failure before PPP. The older 3000/28800 failed recording loses sustained alternating DC1 by the second interval; the index-2 recording loses it by 0.4 seconds. Passing 3000/26400 and 3200/28800 controls decode approximately 1.2 seconds of detection traffic with fourteen extra mark bits between characters, followed by mostly mark bits and short non-ODP bursts before PPP. The newer flat-pre-emphasis failed recording has no recognized ODP run and remains outside the aligned-pattern experiment.

An independent waveform now reproduces 1.2 seconds of that ODP cadence and 4.5 seconds of idle before the existing PPP packets. Both 3000/28800 and 3200/28800 pass PCMU, intersymbol interference, four fractional timing positions, carrier offset/noise and delayed-E replay, including ODP recognition and exact subsequent PPP bytes. `--odp-prefix` is added to the waveform test and CI. This rules out a simple repeated-pattern failure under the tested synthetic channel; it does not reproduce every hardware impairment.

A reference encoder aligned on early hardware symbols locates ODP at bit 1632 for the two failed recordings, 1536 for the passing 3000/26400 control and 2880 for the passing 3200/28800 control. In the index-2 recording, the best lane's normalized symbol MSE rises from 0.55 at symbols 600–719 to 1.60 at 1320–1439 and 2.25 at 1440–1559. Passing controls stay around 0.3–0.4 across those held-out intervals. The reference alignment is supplied evidence, not an online receiver decision.

`audit_odp_feedback.py` is an offline diagnostic accepting a manifest of recording paths, rate, baud, label and supplied ODP start bit. It feeds reference labels only into equalizer adaptation; trellis decoding and PPP FCS remain independent. Over the 15–32-second replay, baseline and 128-symbol supervision recover zero frames from both failed recordings. Supervision through 600 symbols recovers two valid LCP Configure requests from the index-2 recording; through 1200 or 3000 symbols it recovers two from each failed recording. The passing controls retain all 17/20 frame counts and initial LCP recognition at every setting. The portable script reproduces all twenty case/limit results. Aggregate evidence is in `odp-receiver-diagnosis-2026-09-11.json`; raw recordings remain private.

This is an oracle-assisted experiment, not a live fix: alignment and continued ODP are supplied, not established causally. Next work must derive any training reference from validated received traffic, limit prediction across unknown traffic transitions, and verify that independent payload decoding improves in hardware. No receiver-training change was deployed. CI for the preceding opt-in handshake commit `681ea4d` passed.


### Causal ODP training candidate (2026-09-11)

`SIPFAX_V90_ODP_TRAINING=1` opts into a new per-lane predictor, disabled by default. It anchors on four alternating DC1 characters with three consecutive fourteen-mark gaps, reconstructs the scrambler from received bits and the convolutional state from four observed label pairs, and predicts at most twenty-four mapping frames. It supplies equalizer targets only, never PPP bits or trellis decisions. Predicted targets are accepted only when their squared residual against the saved input/current filter is below 16. Absolute symbol tags expire predictions, and B1 reacquisition resets the predictor.

Ungated causal prediction regressed both passing controls when detection ended; it was not deployed. The residual-gated integrated candidate recovers two valid LCP Configure requests from each of the older flat and index-2 failed recordings, retains 17/20 passing-control frames and does not invent recovery in the newer failed recording with no recognized ODP. No supplied timing or recording-specific start bit is used by this candidate.

The independent waveform test validates 81216/86592 predicted labels at 3000/3200 baud, horizon expiration, reset, and option usage. Both enabled waveform paths pass PCMU, ISI, fractional timing and subsequent exact PPP data. CT105 ASan/UBSan validates another 21120/20928 independent labels, including horizon and reset bounds. All 34 native tests and the build pass after adding the new source dependency to their build commands. Hardware qualification remains required before retaining this option.
