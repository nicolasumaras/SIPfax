# V.90 release readiness

Status as of 2026-09-12. PR29 remains a draft. The five acceptance items below
are the release scope; passing the current one-call V.90 subset does not close them.
Detailed historical experiments and corrections are in
[phase2-qualification.md](phase2-qualification.md).

## Qualified deployment

CT105 runs application `9e0f242d40ed65fe2e4af315d0f9c94b45f852ce` and native
source `9c493c3fb06ffe4c4e722ce8cc5dbd535af8bac5`, with native SHA-256
`e62a02b2f2868b096b61957b666cdabb8f25815b2bf47f56926431f6bfea55ea`.
The erasure guard is enabled and admission is limited to one call.
Subsequent development binaries are experimental and are restored after trials.

The latest restoration verification, attempt
`30bd3778-6bce-42ff-b860-ebf7487b69c3`, connected at 49,296 bit/s,
opened PPP/IPCP, passed the 559-byte public HTTP checksum, recorded zero CRC,
timeout, alignment, hardware-overrun, framing and buffer-overrun counters, and
cleanly disconnected. Its final audit verified all 25 managed release files and
idle endpoints. This is a restoration check, not a new endurance campaign.

## Acceptance gates

| Item | Verified evidence | Remaining work |
| --- | --- | --- |
| 1. Repeatable startup, calls and PPP | Qualified native passed ten consecutive physical calls at 49,296 bit/s with checksums and resource cleanup. | Repeat qualification on the final integrated release; preserve failures rather than replacing samples. |
| 2. Integrity-checked sustained download and genuine bulk upload | Attempt `13e84b91-8a75-47e4-b7ae-153d66ae37c2` ran 3,605.869 seconds: 441 verified downloads totaling 14,450,688 bytes and 89 public HTTP checks, with zero reported modem errors. | Test true request-body uploads after the notebook runs DialUpLab 1.2.0. The 441 URL-carried 1-KiB payloads establish neither bulk upload nor upstream capacity. |
| 3. Controlled impairment/recovery and V.34 fallback | Qualified native recovered from natural renegotiation and a controlled three-packet RTP loss case, with intact traffic and cleanup. | Complete the intended impairment matrix and obtain physical V.34 PPP plus integrity-checked traffic. Current V.34 trials fail with errors 678/721. |
| 4. Review, merge, reproducible release and rollback | The retained integrated snapshot passed installation, rollback and reinstallation, each with a physical PPP/checksum/cleanup test; all 25 managed files were audited. | Review PR29, complete final-head CI, merge, package the final source, build on the target, and qualify that final artifact. Earlier binary results do not qualify later source. |
| 5. Concurrent hardware calls and isolation | One-call admission remains enforced. | Obtain a second simultaneous physical modem connection and verify distinct sessions, addresses, traffic and teardown without disturbing the other call. |

## Current V.34 investigation

Development source now includes corrected MP field bit order, receive role,
clockwise mapping and trellis tables, acquisition-buffer handling, stale channel
estimate reset, initialized LSB-first serial framing, and an opt-in LAPM bridge
(`SIPFAX_V34_V42=1`). The LAPM bridge passes exact 16-KiB transfers in both
directions, constrained FIFO backpressure and retraining/rate-change tests,
including CT105 ASan/UBSan. These generated protocol tests do not prove hardware
interoperability. V.34 remains unqualified.

An earlier LAPM trial (`22628e55-93cd-4a3c-972b-ca2fae86105c`) failed with
error 678 without LAPM establishment. Retained TX65423 wire decoding verifies
correct zero precoder coefficients across all four retries. An in-place receive
replay acquired at RMS 0.135 with no 20-ms callback overruns, but the actual ODP
detector found no startup sequence in its first 6,000 decoded bits. Positive
synthetic detection passed. The replay does not reproduce each live reset, and
these findings do not establish whether the caller sent ODP or whether tracking
lost it. Next collect direct live DTE-path counts and determine the caller's
post-B1 waveform before changing detection thresholds or claiming fallback.

The later diagnostic trial (`7596e320-5d64-4b75-867d-2f341a1239de`)
confirmed live ODP detection, ADP transmission and stream selection. The initially
logged HDLC frames failed CRC and LAPM never connected; Windows ended with error
777. This narrows the next investigation to post-startup receive integrity.
The qualified V90 restoration check passed again at 49,296 bit/s.

## Immediate dependencies

The last notebook API health check reports DialUpLab 1.1.1.0. The tested XP
1.2.0 package provides the bulk-upload API; its installation remains pending.
A second physical modem is also needed for item 5. Neither dependency blocks
continued V.34 diagnosis or release review.

CI completed successfully for `c5962e0` (serial framing), `5f7c5cd`
(channel reset) and `9e1c135` (LAPM integration, run 34724716055). Later
heads require their own final result; queued/running checks are not passed checks.
