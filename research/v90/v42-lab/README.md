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

The 46 primary corrected cases also ran under ASan/UBSan on CT105, with no sanitizer findings. Forty-five meet all test conditions; the same delivery-deadline case remains incomplete. The latest sanitizer run also includes the 600-second diagnostic and the framing checks below. Add `--sanitizers` to reproduce sanitizer builds where runtimes are installed; the original baselines intentionally still contain the detected undefined shift.

The patches are supported by the standard and these tests, but two copies of one implementation are not independent protocol-interoperability evidence. Hardware interoperability, broader malformed-frame and parameter-value handling, severe-outage/disconnection behavior and integration with the native bit/byte boundaries remain required before enabling LAPM on CT105.

## Framing qualification

An exact-allocation empty-frame fixture reproduces a heap-buffer-overflow in the reference receive dispatcher under ASan. The correction checks the control-field length, requires the extra control octet for I/S frames and validates the entire XID group/TLV envelope before dispatch. All 14 explicit invalid-input fixtures are ignored without changing protocol state. This is envelope validation, not complete validation of parameter values, widths or negotiated limits.

Independent byte fixtures also expose a missing four-byte pointer advance after the XID HDLC-options field. The correction prevents the following parameter from overwriting the options. Exact 26-byte uncompressed and 44-byte compression-advertisement fixtures pass; the latter tests serialization only and does not enable compression in transfer tests or production. The dictionary-size field already advances correctly and requires no patch.

The pre-framing baseline fails both the wire and malformed-input checks. Corrected framing checks pass normally and under ASan/UBSan, with the primary transfer matrix unchanged at 45/46. No LAPM code is deployed by this lab.

## Asymmetric parameter negotiation

Independent XID fixtures expose wrong direction mapping, replies advertising configuration rather than selected values, discarded negotiation at SABME establishment, and nonstandard handling of omitted fields. The temporary patch maps peer TX to local RX and vice versa, uses the standard 128-octet/15-frame defaults in the negotiation rule, serializes selected values in responses, and preserves negotiated parameters across link establishment. Explicit modem restart restores configured preferences. See V.42 9.2.3–9.2.4 and Table 11a note 2.

The independent command fixture requests TX/RX sizes of 112/80 octets and windows of 9/4 against local preferences of 64/96 octets and 3/5 frames. It verifies the opposite-direction selected values, their exact reply bytes, persistence through SABME, restart and omitted-parameter defaults. The pre-negotiation baseline fails the direction, reply, persistence and omission checks.

Four additional transfer cases use those different preferences on the two peers and verify all 65,536 bytes in each direction, drained acknowledgements and matching final limits. Clean, delayed/backpressured, corrupted and detection-disabled cases pass. The original 46-case matrix remains 45/46 within its deadline; the expanded primary transfer total is 49/50.

These checks do not yet establish rejection of all invalid parameter values, response values outside the offered range, unsupported optional functions, safe configuration above allocated maxima, or full interoperability. Those remain integration gates.

The expanded 50-case transfer matrix, negotiation assertions, framing checks and long diagnostic also ran under ASan/UBSan on CT105 with no findings in corrected variants. The known deadline failure remains unchanged.

## Complete-interruption checks

Four additional fault cases replace both transmitted bitstreams with constant marks starting at simulated second five, with either zero or 80 ms propagation delay. A two-second interruption recovers all 65,536 bytes in each direction and drains acknowledgements without reconnecting, finishing at 23.394375/38.202500 seconds. A thirty-second interruption generates exactly one disconnect per peer at approximately 18.98–19.06 seconds, leaves both peers idle, and delivers no payload after disconnect. The simulation continues to second 40, including five seconds after the channel returns. These tests model damaged digital bitstreams, not loss of modem carrier or process scheduling.

No additional protocol correction was required for these four cases. The original dense-error deadline failure is still reported. Native carrier-loss, retraining, candidate selection and PPP teardown remain separate integration requirements.

All four interruption checks also pass under ASan/UBSan with no findings.
