# Isolated V.42 reference qualification

This lab exercises SpanDSP revision `8f1e1646bdec99eac5fd2cd92c35563f736b9b89` without installing it or contacting hardware. The runner verifies the hashes of all 21 source/header files used, builds temporary executables and emits metadata only. Upstream code and its licence headers remain in the supplied checkout; the lab does not vendor or deploy the library.

```sh
python3 research/v90/v42-lab/run.py \
  --source /path/to/pristine/pinned/spandsp \
  --output /path/to/report.json
```

Requirements: Python 3 and GCC. The supplied source root can be a checkout or extracted archive. The runner exits **1** if any corrected case remains incomplete; do not interpret report generation or successful compilation as a passing suite.

## Defects and corrections

The upstream RR/RNR response helper always sets F=1. V.42 sections 8.4.2.1–8.4.2.2 require F to reflect whether the received I command requested a poll. Unsolicited F=1 responses are ignored by the reference's response receiver when no poll is outstanding. In the unequal-rate clean-line test, this prevents all bytes from being sent before the 120-second deadline. The runner corrects the helper in a temporary source copy by using the received P bit. The pristine source is not modified.

The sender also checks whether *any* timer is running before starting T401. An active T403 idle timer therefore delays acknowledgement recovery. V.42 8.4.1 requires T401 when transmitting an I frame; the correction distinguishes the active timer callback and replaces T403. The lab checks that every transmitted I frame has an acknowledgement timer bounded by one second. The final-bit-only baseline produces thousands of violations in the dense-error cases; the corrected matrix produces none.

ASan/UBSan exposed a signed left shift of the detection register while it held -1. Only the ten-bit received character is inspected, so the correction masks to nine previous bits before shifting in the new bit. This preserves the used detector history without undefined signed arithmetic.

Primary references: [pinned response helper](https://github.com/freeswitch/spandsp/blob/8f1e1646bdec99eac5fd2cd92c35563f736b9b89/src/v42.c#L653), [ITU-T V.42 (03/2002)](https://www.itu.int/rec/T-REC-V.42).

## Coverage and actual outcome

Each peer sources 65,536 deterministic, non-periodic bytes. Every received byte is compared with the opposite source's expected stream, so missing, reordered, duplicated and corrupted data fail verification. Successful cases must also drain outstanding acknowledgements, remain connected, report no link errors, use the acknowledgement timer for transmitted I frames, and deliver no data while the receiving application signals busy.

The simulated clocks run at 28,800 upstream and exactly 148,000/3 downstream bits/s, scheduled in 8 kHz time steps. The reference's private timer-rate field is set to the corresponding integer rate; production integration still needs a deliberate timing API. Compression is disabled on both peers. No codec, PCM, modem training or operating-system scheduling is simulated.

The matrix covers 0/40/80/120 ms delay in each direction, receiver backpressure, fixed and variable frame lengths, periodic bit flips, 160-bit damaged bursts, explicit detection and direct LAPM establishment. Delay queues have a checked 8192-entry bound per direction. The final one-second acknowledgement-drain interval is included in the completion time.

`results.json` contains the actual outcomes:

- Pristine equal-rate delivery completes, but fails the final acknowledgement condition. Pristine unequal-rate delivery stalls at 63,232 bytes in one direction and misses the 120-second deadline.
- Corrected fixed-frame cases: 21/22 pass. Continuous error periods of 11,000/11,997 bits at 120 ms delay do not complete before the deadline; the 80 ms case completes in 119.744 seconds. This remains a limitation; the test is not relaxed to hide it.
- Corrected variable-frame cases: 22/22 pass, including the same error/delay combinations.
- Two recovery cases with the dense faults stopped after 30 seconds pass, recovering all bytes and acknowledgements without re-establishing the link.

A separate `long-stress` diagnostic retains continuous faults and allows up to 600 simulated seconds. The remaining case completes in 164.961250 seconds, with all data and acknowledgements correct after 424/666 injected bit flips. This demonstrates eventual recovery but does not change the original 120-second failure.

The 46 primary corrected cases also ran under ASan/UBSan on CT105, with no sanitizer findings. Forty-five meet all test conditions; the same delivery-deadline case remains incomplete. The 600-second diagnostic was not part of that sanitizer run. Add `--sanitizers` to reproduce sanitizer builds where runtimes are installed; the original baselines intentionally still contain the detected undefined shift.

The patches are supported by the standard and these tests, but two copies of one implementation are not independent protocol-interoperability evidence. Hardware interoperability, malformed-frame handling, severe-outage/disconnection behavior and integration with the native bit/byte boundaries remain required before enabling LAPM on CT105.
