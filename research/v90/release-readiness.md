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
`47e3b108-21f2-444e-8d82-0c76b2bea720`, connected at 49,296 bit/s,
opened PPP/IPCP, passed the 559-byte public HTTP checksum, recorded zero CRC,
timeout, alignment, hardware-overrun, framing and buffer-overrun counters, and
cleanly disconnected. Its final audit verified all 25 managed release files and
idle endpoints. This is a restoration check, not a new endurance campaign.

## Acceptance gates

| Item | Verified evidence | Remaining work |
| --- | --- | --- |
| 1. Repeatable startup, calls and PPP | Qualified native passed ten consecutive physical calls at 49,296 bit/s with checksums and resource cleanup. | Repeat qualification on the final integrated release; preserve failures rather than replacing samples. |
| 2. Integrity-checked sustained download and genuine bulk upload | Attempt `13e84b91-8a75-47e4-b7ae-153d66ae37c2` ran 3,605.869 seconds: 441 verified downloads totaling 14,450,688 bytes and 89 public HTTP checks, with zero reported modem errors. | Test true request-body uploads after the notebook runs DialUpLab 1.2.0. The 441 URL-carried 1-KiB payloads establish neither bulk upload nor upstream capacity. |
| 3. Controlled impairment/recovery and V.34 fallback | Qualified native recovered from natural renegotiation and a controlled three-packet RTP loss case, with intact traffic and cleanup. | Two of four V.34-only hardware calls pass at 12,000 bit/s with PPP and verified HTTP probes; the other two fail. Fix repeatability, test longer transfers and automatic fallback, and complete the intended impairment matrix; earlier failures remain part of the evidence. |
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
lost it. Later diagnostics below provide direct live DTE counts and waveform evidence;
this earlier replay alone does not justify changing detection thresholds.

The later diagnostic trial (`7596e320-5d64-4b75-867d-2f341a1239de`)
confirmed live ODP detection, ADP transmission and stream selection. The initially
logged HDLC frames failed CRC and LAPM never connected; Windows ended with error
777. This narrows the next investigation to post-startup receive integrity.
The qualified V90 restoration check passed again at 49,296 bit/s.

Later capture 66406 shows the caller signal disappearing partway through B1,
followed by sustained 1200-Hz Tone B, consistent with caller retraining. A scoped
transport trial (`f5c1c230-c546-4b36-9a7b-4f83ee1c1286`) found continuous RTP
sequence numbers on all four observed legs and regular incoming timestamps at
CT105; this does not prove ATA reception or correct analog playout. The only
PBX-to-ATA timestamp rewind coincides with the early-media handoff, about 17
seconds before the first observed retrain. The deployed RED encoder preserves
that timestamp and carries no stale redundant block across the handoff.
A causal link to the startup rewind remains unproven. The retrain milestone
below establishes a working V.34-only path, not automatic fallback qualification.

Development `0221dfb` fixes the answer retrain entry: exactly 70 ms silence,
then Tone A instead of replaying the initial INFO0a exchange. Physical attempt
`b43f1f65-8bdc-432e-9de8-719e0b41b933` reaches LAPM and PPP/IPCP at
12,000 bit/s after two retrain responses. Two 559-byte public HTTP probes match
the expected checksum and show zero six-category RAS errors; teardown is clean.
The 21-second post-connect observation is a short milestone, not endurance or
repeatability. The initial training problem is not eliminated. Next repeat this
candidate without discarding failures, extend V.34 traffic testing, and verify
selection through normal V.90/V.34 negotiation. Production remains the qualified
V.90 build; its fresh restoration call and manifest audit pass.

Three unchanged-candidate repeats produced failures 777 and 678, followed by a
second successful 12-kbit/s call (`666be31e-447e-4660-9654-4ad9b0c34faf`). Both
of its public HTTP checksums and all RAS counters pass. Thus the observed series
is two successes and two failures; this small sample fails the repeatability
gate and is not an estimated long-term success rate. Raw traces are retained
under unique per-trial paths on CT105. Both successful and failed calls can
acquire below 0.2 lattice RMS; the second success uses a +167.9-ppm clock seed.
Neither acquisition RMS nor a near-zero clock seed alone explains the outcome.
The failed678 recording starts with clean B1 and ODP, then delivers a median
5,056 receive bits/s while transmitting at12k. Replaying it with its +226.4ppm
seed yields zero valid HDLC frames; zero seed recovers four valid77-byte XID
commands but still leaves gaps. Successful calls deliver the full receive rate.
The clock-publication replay confirms this seed came from a CRC-valid MP pass.
A trailing1,024- or2,048-symbol mean improves that recording and preserves four
valid frames on a second failed-call recording; a256-symbol mean regresses the
second recording to zero valid frames. The longer windows also preserve55 byte-identical valid frames on a successful
call replay and recover frames at two less favorable restart offsets. Source
e47bd0a adds the method as an opt-in bounded history with sanitizer coverage.
Its first1024-symbol hardware trial fails678 before ODP detection, with poor
B1 decoding despite modest seeds. The option remains off by default. Next
inspect the caller E/B1 transition and receiver initialization in that trial;
clock averaging alone has not qualified fallback.

## Immediate dependencies

The last notebook API health check reports DialUpLab 1.1.1.0. The tested XP
1.2.0 package provides the bulk-upload API; its installation remains pending.
A second physical modem is also needed for item 5. Neither dependency blocks
continued V.34 diagnosis or release review.

CI completed successfully for `c5962e0` (serial framing), `5f7c5cd`
(channel reset) and `9e1c135` (LAPM integration, run 34724716055). Later
heads require their own final result; queued/running checks are not passed checks.
