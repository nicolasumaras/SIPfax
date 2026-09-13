# 26.4 kbit/s receiver regression investigation

The deployed `5151eb5` binary remains active with automatic echo acquisition. This investigation used local independent synthetic transmitters; no hardware call or server configuration change was made.

## Expanded baseline evidence

The previous test stopped at its first assertion. Running all four fractional phases and both ±100 ppm clock offsets reveals the following failures. Direct reception and delayed-E replay produce the same frame counts and first mismatches in every listed case; the delayed-E handoff does not explain these losses.

| Seed | Fractional phase | Clock ppm | Frames | First mismatched index |
|---|---:|---:|---:|---:|
| 43127 | 0.5 | +100 | 191/192 | 17 |
| 43127 | 0.75 | +100 | 191/192 | 17 |
| 62091 | 0 | -100 | 191/192 | 8 |
| 62091 | 0 | +100 | 188/192 | 2 |
| 62091 | 0.25 | -100 | 191/192 | 9 |
| 62091 | 0.25 | +100 | 188/192 | 2 |
| 62091 | 0.5 | -100 | 191/192 | 8 |
| 62091 | 0.5 | +100 | 190/192 | 2 |
| 62091 | 0.75 | -100 | 191/192 | 8 |
| 62091 | 0.75 | +100 | 189/192 | 2 |

Every unlisted seed-43127 case recovers all 192 expected frames. Seed 62091 fails all eight cases. First mismatch is zero-based; the receiver may omit a frame rather than deliver corrupted data. The PPP FCS remains enforced. These results expand, and supersede, earlier descriptions of only one failing case per seed.

## Rejected or insufficient experiments

All changes were private copies; none was deployed or retained in the receiver source.

- Reducing 26.4 kbit/s timing phase gain from 0.1 to 0.05 or 0.02 worsened positive-clock acquisition. At 0.02, only nine frames were recovered in the three tested positive-clock cases for seed 43127.
- Raising initial timing integrator gain from 0.00001 to 0.0001 for three seconds passed the full seed-43127 matrix, but seed 62091 phase 0 / -100 ppm recovered only 189 frames. The first 189 matched; the final three were absent.
- A smaller initial integrator gain of 0.00005 also passed the full seed-43127 matrix and corrected seed 62091 phase 0 / -100 ppm. It still recovered only 188 frames at seed 62091 phase 0 / +100 ppm, the same count as baseline. It is insufficient as a general fix, rather than evidence of a newly introduced failure at that phase.
- Moving 26.4 kbit/s carrier/gain correction to provisional trellis decisions left the tested failures unresolved.
- Halving trellis-driven equalizer adaptation from 0.1 to 0.05 worsened the tested seed matrices.

## Symbol-level observation and next target

For baseline seed 43127 phase 0.5 / +100 ppm, one of three acquired lanes tracks the expected symbols closely. Its normalized symbol-error RMS is about 0.459 over the first 6000 outputs after excluding B1. Error RMS rises from about 0.420 in symbols 3000–3199 to 0.599 in 3200–3399, with a peak of 1.632. The omitted PPP frame falls around this startup interval. Other acquired lanes have substantially larger errors. This establishes a local distortion increase; it does not by itself prove its physical cause.

The clock integrator is also slow initially: that useful lane estimates about 0.000253 quarter-sample units per symbol at 1.1 seconds versus the injected 0.001. Faster acquisition reduces that lag but does not solve all payload/phase cases. Simple gain changes are therefore not a sufficient next deployment. The next receiver work should examine coupled sampling-phase/equalizer distortion, including whether fractionally spaced equalization improves acquisition, with the full two-seed matrix retained as a gate.

`tools/tests/v90-qam8-wave.py` now accepts `--keep-going`. It aggregates frame mismatches for direct and delayed-E reception and returns nonzero if any occur. Acquisition or other structural failures still stop immediately. Default behavior still fails on the first frame mismatch. Reproduce with:

```sh
python3 tools/tests/v90-qam8-wave.py --26400 --long-clock --isi --pcmu --random-payloads --timing-sweep --seed 62091 --keep-going
```

Use seed 43127 for the second matrix. These are known failing diagnostic commands, not passing qualification checks. The full V.90 goal remains incomplete.


## Half-symbol equalizer implementation

The 26.4 kbit/s PCM receiver now supplies a midpoint and a symbol-time sample to a 29-tap complex FIR. Both come from the existing quarter-sample matched-filter history. Midpoints are carrier-normalized at half a symbol before the current phase. The receiver keeps symbol-time outputs with seven symbols of lookahead, matching the prior 15-tap path's output alignment. Provisional trellis feedback remains four pairs; final decisions retain 63-pair lookahead.

B1 fitting uses 256 half-symbol observations and 128 known targets. It retains the 80-symbol fit and held-out validation, but increases the identity-directed ridge from 1e-6 to 1e-3 times trace/taps for the correlated half-symbol inputs. This corrected the prototype's frequent rejection of excessive coefficient norms; the norm-squared bound remains four. Bounded NLMS permits step 0.2 only for the 29-tap state. Seven/fifteen-tap paths keep their original 0.1 maximum, training API, and operation. Symbol-only 26.4 tests retain the 15-tap path unless midpoint input is supplied.

The final integrated receiver passes all 192 exact expected frames in each of eight phase/clock cases, for both direct reception and delayed-E replay, under the default payload and seeds 43127, 62091 and 98017. This closes the documented two-seed synthetic losses; it does not prove all channels or rates. The independent 24 kbit/s long-clock/ISI/PCMU/random-payload matrix also passes.

Dedicated half-symbol tests verify startup/output alignment, independently distorted held-out channel recovery, incompatible training modes, invalid input and step rejection with unchanged state. The full native suite, existing seven/fifteen-tap equalizer tests and clean native build pass. CT105 ASan/UBSan checks pass for half-symbol history wrapping, reacquisition and invalid midpoints. A CT105 normal-build replay recovered 192 frames across 567 blocks; total processing time was 439.195 ms and the largest 20 ms block took 12.868 ms. This is one timing observation, not a hard deadline guarantee.

No hardware deployment has yet been made for this change. CT105 continues to run `5151eb5` with automatic echo-delay acquisition. The next gate is CI followed by a reversible short hardware call, then sustained transfers if the short call qualifies. Full V.90 conformance, higher upstream rates/symbol rates, broader reliability, ongoing echo-delay tracking, fallback and future concurrent calls remain unfinished.


## Half-symbol hardware qualification

CI run 34547725448 passed both jobs for `0207a6d`. Three earlier echo-corrected hardware recordings replayed to exactly the same ordered PPP hashes as the prior receiver (10, 10 and 179 frames).

Short hardware attempt `d91237f9-de18-4f4c-9ddf-6fcf4eb7aa0f` passed all 18 response hashes and the internet probe. The live native process 12372 used binary SHA-256 `3d2664a38f84b2748e07a05b9aaf809287f0c90af4064e41a2df6079e8d33342`, upstream 26400 and echo mode `auto`. Downstream CP was 49.333 kbit/s. Echo acquisition selected delay 1428 at sample 96480; B1 correlation was 0.9801. The PPP call lasted 36.452 seconds with zero CRC/alignment/overrun errors. Download took 7.466 seconds; median upstream check was 1.3985 seconds. Its 13160-packet capture had zero kernel drops, no RTP sequence gaps, 3262 matching downstream primary payloads and 3264 matching upstream payloads after eleven ATA startup packets.

Sustained attempt `004398a5-c1f8-49c6-8e61-604290a948f5` did **not** qualify. Live process 12567 used the same verified binary and configuration. Fifty completed response hashes were independently verified. Download 25 then returned HTTP 200 but incomplete data: 27257 bytes, IOException, elapsed 30003 ms. The last Windows counters at PPP duration 290.077 seconds were 20 CRC and one alignment error, with no timeout/framing/buffer/hardware-overrun counters. The application deadline is distinct from Windows' modem timeout counter. The harness explicitly disconnected after this failed probe.

On the native Phase4 clock, renegotiation began at 298.800 seconds, CP remained 49.333 kbit/s at 300.525750, and upstream E/B1 completed at 300.857/300.897 seconds. These timestamps have a different origin from the Windows PPP duration. The final S/Sbar at 301.285/301.293 accompanied hangup and was not another completed recovery. Echo acquired delay 1428 at sample 96640; initial B1 correlation was 0.9782 and recovered B1 correlation 0.9808.

The sustained capture contained 64265 packets with zero kernel drops and no RTP sequence gaps. All 15991 downstream primary payloads matched through FreePBX/RED; all 15993 forwarded upstream payloads matched after eleven startup packets. This does not establish analog playout quality. The notebook was idle and SIPFAXRED registration was cleared after hangup.

An independent replay reproduced the live echo acquisition delay/sample exactly. Both `5151eb5` and `0207a6d` recovered 944 CRC-valid PPP frames from that same corrected recording, with identical ordered hashes. This provides no evidence of an upstream decoding regression on the recording. It does not prove the cause of the live application stall or its eventual recovery, because the live test disconnected after the deadline.

The trial restored `5151eb5` with automatic echo acquisition. Service activity, binary hash and FreePBX availability were verified. Trial, capture, fixture, CI watcher, retrieval and replay processes are terminal. `0207a6d` remains available in `/tmp/v90-build-0207a6d` for further testing; it has not replaced the qualified runtime.

Next: run a recovery diagnostic that retains failed probe records, requires the same PPP connection ID, and permits one retry of a failed request (aborting on repeated failure or three total failures). This tests post-stall transfer recovery without relabeling an interrupted request as successful. A clean sustained qualification remains outstanding; full V.90 work remains incomplete.


## Recovery diagnostic on the same PPP connection

Attempt `fef97137-64f7-4c07-b60c-7c84170165ca` used `0207a6d`, native PID12767, verified SHA-256 `3d2664a38f84b2748e07a05b9aaf809287f0c90af4064e41a2df6079e8d33342`, upstream 26400 and automatic echo acquisition. It completed all 129 target checks with one failed request followed by a successful retry: 130 HTTP probe attempts in total, all on the original PPP connection ID. Every successful response hash was independently verified. This is successful recovery, not an uninterrupted or error-free qualification.

After 98 successful checks, `download-49` attempt 1 reached its 30.000-second deadline after 12841 body bytes with IOException. The failed record remains in both repeatProbes and probeFailures. Its second attempt returned the complete 32768 bytes with the expected hash in 14.655 seconds. The following `download-50` completed in 28.844 seconds. No further request retries were needed. All successful target downloads total 2 MiB; upstream target checks total 64 KiB. Retried/partial traffic adds overhead beyond those totals.

The connection lasted 746.994 seconds, then the harness disconnected cleanly. Windows ended with 55 CRC and eight alignment errors; timeout, framing, buffer and hardware-overrun counters were zero. Last byte counters were 197127 sent / 2391753 received, including protocol/retry traffic. Downstream started at 49.333 kbit/s and renegotiated to 48 kbit/s on the native Phase4 clock at 629.537500–631.569500 seconds. The error counters subsequently stayed at 55/8. Initial B1 correlation was 0.9746 and recovered B1 correlation 0.9749. The final S/Sbar at 757.237500/757.252500 accompanied hangup. Native and Windows timestamps have different origins.

The RTP capture contained 155787 packets, zero kernel drops and no sequence gaps. All 38790 forwarded downstream primary payloads matched; one final server RTP packet arrived 3.322 ms after FreePBX had sent SIP BYE and was not forwarded. All 38792 forwarded upstream payloads matched after eleven ATA startup packets. This final-packet exception was verified against SIP timestamps rather than ignored as an arbitrary suffix difference.

The new TCP capture contained 4629 packets with zero kernel drops. Its 130 TCP flows match the 130 probe kinds in order, and each includes the server SYN. The audit detected 154 downstream segments overlapping earlier transmitted sequence ranges. The timed-out request's flow had eight such retransmissions; its successful retry had ten. The original TCP flow remained visible for 86 seconds including later teardown traffic: that lifetime and its late ACK gap must not be mistaken for the application's 30-second request duration. TCP retransmissions coexist with intact RTP transport; the captures alone do not localize analog/DSP errors.

Packet-clock fits over the steady portion of this capture gave ATA input -0.924 ppm and server output -0.240 ppm relative to the same capture clock, a difference below 1 ppm. The source pacer uses nominal packet durations, but this measurement does not support a large RTP clock-rate mismatch on this call. It does not prove continuous analog DAC playout, so no pacing change is justified from this evidence alone.

The wrapper restored `5151eb5`; service activity, binary hash, FreePBX readiness, notebook disconnection and cleared SIPFAXRED state were verified. Trial, fixture, both captures and retrieval/analysis jobs are terminal. The next comparison is the same recovery diagnostic on that qualified baseline with TCP and RTP captures. `0207a6d` has not replaced it, and a clean sustained qualification remains outstanding.
