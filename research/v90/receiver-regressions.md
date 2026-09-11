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
