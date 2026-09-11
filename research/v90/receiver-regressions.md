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
