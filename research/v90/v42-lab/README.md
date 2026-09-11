# Isolated V.42 reference qualification

This lab exercises SpanDSP revision `8f1e1646bdec99eac5fd2cd92c35563f736b9b89` without installing it or contacting hardware. The runner verifies the hashes of all 21 source/header files used, builds temporary executables and emits metadata only. Upstream code and its licence headers remain in the supplied checkout; the lab does not vendor or deploy the library.

```sh
python3 research/v90/v42-lab/run.py \
  --source /path/to/pristine/pinned/spandsp \
  --output /path/to/report.json
```

Requirements: Python 3 and GCC. The supplied source root can be a checkout or extracted archive. The runner exits **1** if any corrected case remains incomplete; do not interpret report generation or successful compilation as a passing suite.

## Defect and correction

The upstream RR/RNR response helper always sets F=1. V.42 sections 8.4.2.1–8.4.2.2 require F to reflect whether the received I command requested a poll. Unsolicited F=1 responses are ignored by the reference's response receiver when no poll is outstanding. In the unequal-rate clean-line test, this prevents all bytes from being sent before the 120-second deadline. The runner corrects the helper in a temporary source copy by using the received P bit. The pristine source is not modified.

Primary references: [pinned response helper](https://github.com/freeswitch/spandsp/blob/8f1e1646bdec99eac5fd2cd92c35563f736b9b89/src/v42.c#L653), [ITU-T V.42 (03/2002)](https://www.itu.int/rec/T-REC-V.42).

## Coverage and actual outcome

Each peer sources 65,536 deterministic, non-periodic bytes. Every received byte is compared with the opposite source's expected stream, so missing, reordered, duplicated and corrupted data fail verification. Successful cases must also drain outstanding acknowledgements, remain connected, report no link errors, and deliver no data while the receiving application signals busy.

The simulated clocks run at 28,800 upstream and exactly 148,000/3 downstream bits/s, scheduled in 8 kHz time steps. The reference's private timer-rate field is set to the corresponding integer rate; production integration still needs a deliberate timing API. Compression is disabled on both peers. No codec, PCM, modem training or operating-system scheduling is simulated.

The matrix covers 0/40/80/120 ms delay in each direction, receiver backpressure, fixed and variable frame lengths, periodic bit flips, 160-bit damaged bursts, explicit detection and direct LAPM establishment. Delay queues have a checked 8192-entry bound per direction. The final one-second acknowledgement-drain interval is included in the completion time.

`results.json` contains the actual outcomes:

- Pristine equal-rate delivery completes, but fails the final acknowledgement condition. Pristine unequal-rate delivery stalls at 63,232 bytes in one direction and misses the 120-second deadline.
- Corrected fixed-frame cases: 20/22 pass. Continuous error periods of 11,000/11,997 bits at 80/120 ms delay do not complete before the deadline. This remains a limitation; the test is not relaxed to hide it.
- Corrected variable-frame cases: 22/22 pass, including the same error/delay combinations.
- Two recovery cases with the dense faults stopped after 30 seconds pass, recovering all bytes and acknowledgements without re-establishing the link.

The patch is supported by the standard and these tests, but two copies of one implementation are not independent protocol-interoperability evidence. Hardware interoperability, malformed-frame handling, severe-outage/disconnection behavior, sanitizers and integration with the native bit/byte boundaries remain required before enabling LAPM on CT105.
