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
`ab518fe0-88ca-49f0-bf0f-372b5f13b678`, connected at 49,296 bit/s,
opened PPP/IPCP, passed the 559-byte public HTTP checksum, recorded zero CRC,
timeout, alignment, hardware-overrun, framing and buffer-overrun counters, and
cleanly disconnected. Its final audit verified all 25 managed release files and
idle endpoints. This is a restoration check, not a new endurance campaign.

## Acceptance gates

| Item | Verified evidence | Remaining work |
| --- | --- | --- |
| 1. Repeatable startup, calls and PPP | Both the qualified baseline and exact packaged candidate397a090 passed ten consecutive physical calls at 49,296 bit/s with checksums and resource cleanup. Candidate installation and final rollback also pass. | Repeat if runtime files or the qualified profile change before release. This V90 series does not establish combined-profile/V34 startup reliability. |
| 2. Integrity-checked sustained download and genuine bulk upload | Attempt `13e84b91-8a75-47e4-b7ae-153d66ae37c2` ran 3,605.869 seconds: 441 verified downloads totaling 14,450,688 bytes and 89 public HTTP checks, with zero reported modem errors. | Test true request-body uploads after the notebook runs DialUpLab 1.2.0. The 441 URL-carried 1-KiB payloads establish neither bulk upload nor upstream capacity. |
| 3. Controlled impairment/recovery and V.34 fallback | Qualified native recovered from natural renegotiation and a controlled three-packet RTP loss case, with intact traffic and cleanup. | The latest V.34-only candidate passed four consecutive short 12,000-bit/s PPP calls, including two with startup retraining; a subsequent endurance dial failed with error 678 before connecting. A subsequent 1,802.783-second V.34 session passes transfer integrity and cleanup. The timeline fix also passes the bounded three-packet established-link loss case. Repeat and broaden impairment coverage, test automatic fallback, and complete the intended impairment matrix; earlier failures remain part of the evidence. |
| 4. Review, merge, reproducible release and rollback | The qualified snapshot and packaged candidate397a090 both passed installation, rollback and reinstallation with physical PPP/checksum/cleanup checks. The candidate source archive rebuilds to the tested native hash; all25 managed files were audited and production restored. | Review PR29, complete final-head CI, merge, and finish qualification of the final artifact. Repeat packaging/deployment checks if the artifact changes. Earlier binary results do not qualify later source. |
| 5. Concurrent hardware calls and isolation | One-call admission remains enforced. A concurrent PPP process regression checks cross-call hook rejection, reserved addresses until exit, safe reuse, and survival of the other session and its files during teardown. Child processes are simulated. | Obtain a second simultaneous physical modem connection and verify distinct sessions, addresses, traffic and teardown without disturbing the other call. |

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
B1 decoding despite modest seeds. The option remains off by default. The subsequent B1 audit finds the last caller B1 intact in replay. Inspection
also finds a function-static traceback warmup counter surviving receiver resets.
Source832f532 makes warmup per receiver and clears partial mapping state; a
negative-control build reproduces the old bug and the fixed regression passes.
Its first physical trial, with averaging still1024, connects12k and passes two
HTTP probes with zero RAS errors on the first training attempt. That initial call did not retrain. Three unchanged-candidate repeats subsequently
pass at12k, with one startup retrain in each of the first two repeats. All six
repeat HTTP probes have the expected checksum and zero RAS counters; teardown
is clean. This gives four consecutive successful short calls for832f532. It does
not qualify sustained traffic, an established link through impairment, automatic
V90-to-V34 selection or the final integrated release. Next run sustained V34
traffic and broaden recovery testing while preserving the qualified V90 build.

The first subsequent endurance attempt, `060ef8c2-e494-44fb-a45b-3ac69c3db719`,
failed with error 678 before connection. It remains in the campaign evidence;
the four earlier short successes do not imply all later startups succeeded.

The next unchanged-candidate endurance call,
`1b7835aa-bed6-4dc7-b7ab-7a42c269dce2`, passes 1,802.783 seconds at
12,000 bit/s. The independent audit verifies 63 downloads (2,064,384 bytes),
63 small URL-carried requests (64,512 payload bytes), 13 public HTTP checks,
server verification, continuous connection identity and duration, zero errors
in all six RAS categories, and clean disconnection. Median download payload
rate is 9,900.82 bit/s. These request-URL transfers are not bulk uploads. This
is one sustained V.34-only success; automatic fallback, impairment recovery
and startup reliability remain open. The controller restored the qualified
native binary and removed the temporary trial configuration.

Release candidate `aa571ec` now builds reproducibly across separate CT105
directories (native `f0b8aadb…ec42e00d`) and passes the focused target
regressions. This exact binary passes one short physical V90 call at 49,296
bit/s and one V34-only call at 12,000 bit/s, with checksums and cleanup.
Production is restored. These checks do not replace final-artifact endurance,
repeatability or automatic fallback qualification.

The reproducible candidate passes a three-packet inbound RTP-loss test in
V90 mode (`988bdc1e-a8ff-4635-9a5a-e29881cf14a4`): four verified HTTP
checks, continuous PPP, zero RAS errors and clean restoration. The corresponding
V34 test (`259257c5-6042-4d03-a25d-bcca6c7ce726`) fails its first post-loss
HTTP request after a valid baseline. Receive bit delivery drops and remains
below the negotiated rate; no retrain response is recorded. V34 erasure
recovery is therefore a demonstrated remaining failure. Production is restored
and all 25 managed files and idle endpoints are verified.

Source `d8c3f33` fixes data-mode sample loss at the silence gate and passes
the permanent regression with a failing old-code negative control. Its target
binary `164008c9…624b39f` builds reproducibly and passes focused native tests.
An initial dial fails678 before injection; an unchanged repeat
(`3b8412e6-7633-4223-8d46-a60e21257604`) passes the three-packet inbound
loss case at12,000bit/s with four HTTP checks, zero RAS errors and cleanup.
This supersedes the failure for that single bounded recovery case, but leaves
startup reliability, broader impairments and automatic fallback open. The
qualified V90 deployment is restored and audited.

Two unchanged-candidate recovery repeats (`20852cde-b882-41e2-a743-05c05b46bd33`
and `4a825d64-a884-4296-a485-53ec94fa28d2`) also pass. The observed series
is one startup678 failure followed by three consecutive passing bounded-loss
calls, all with verified restoration. This is repeat evidence for that one
loss case, not a general reliability estimate.

The combined profile (V90 offered with V34 CMA/LAPM enabled) has one failed
V90 call777 followed by one successful49,296-bit/s call after a passing
normal-profile control. The successful combined call passes checksum, counters
and restoration audits. This establishes basic coexistence, not reliability
or fallback: a caller requesting V34 is still needed for that test.

## Immediate dependencies

Updated application candidate9156097 passes its own V90 endurance call,
`8fbaa1da-32e7-48d9-9d48-943555c9c983`: 1,802.686 seconds, 221 verified
downloads totaling7,241,728 bytes, 221 small URL transfers and45 public HTTP
checks. Independent audit verifies all hashes, continuous connection identity
and timing, zero six-category RAS errors, final fixture PASS and disconnection.
Median download payload rate is40,592.13bit/s. The updated controller also
verifies zero server sessions, pppd processes, leases and media lines before
automatic rollback, which succeeds. These transfers still do not qualify bulk
upload. Fresh restored-baseline attempt
`4e4b8b1e-86ba-4cb3-b36d-5c7dff4b75d7` passes at49,296bit/s with PPP,
checksum and cleanup; all25 restored files are verified and the fixture is
inactive. Updated candidate9156097 also passes all ten consecutive planned
calls in campaign `release915-repeatability-1789271469`, each at49,296bit/s
with PPP/IPCP, expected public HTTP hash, zero six-category modem errors and
resource cleanup. Installation, automatic rollback and all25 restored files
pass independent audits. The final call belongs to the candidate; the subsequent
live audit verifies restored files and idle endpoints, not another baseline
dial. This completes the bounded V90 repeatability check for candidate915's
normal profile. V34, bulk upload, broader impairments and the other release
requirements remain open.

Failed-worker cleanup review found that `ExternalModemProcessBackend` emits
`backend-exit`, but `Line` did not forward it to the session manager. A focused
regression reproduces the resulting allocated-call leak without a PTY-close
event. Development source now forwards the exit and terminates only the owning
current call, preserving asynchronous RTP release and ignoring stale events
after Call-ID reuse. The regression also covers a surviving second call and
synchronous exit notification during teardown. All89 JavaScript tests pass.
Source9156097 has now passed a bounded physical worker-failure cleanup/redial
trial. Its reproducible source archive rebuilds to the existing native hash;
the installer verifies all25 managed files. After a verified49,296-bit/s call,
only its service-owned native worker is interrupted with SIGKILL. Attempt
`d3551379-2b49-4de8-b841-19e7e427a0ac` releases server call/PPP/media
resources within0.922 seconds of the first post-injection observation window;
the notebook active-connection list clears within2.841 seconds. A new attempt
`342ef450-8444-4365-af11-ab3a8f2de14f` connects at49,296bit/s, reuses the
released RTP port, passes the public checksum with zero RAS errors, and cleans
up. Rollback and all25 restored files pass live audit. DialUpLab's attempt
record still reported Connected when its connection list cleared; that API
status discrepancy is retained. This qualifies worker-crash cleanup and one
redial, not network-blackout recovery or final-artifact endurance/repeatability.
Earlier candidate397 results remain evidence for candidate397, not this changed
application runtime. The qualified baseline is restored.

The opposite-direction bounded-loss case also passes on candidate397:
attempt `b965d53d-65fa-4e2e-9ba0-8f0815d1de9e` drops exactly three RTP
packets leaving the active server port toward PBX192.168.1.29. The audit
checks the output hook, destination address, UDP source port, measured counter,
four public HTTP hashes, continuous PPP duration and zero RAS errors at49,296bit/s.
Teardown, injection removal, rollback and all25 restored files pass.
This is one outbound three-packet case; controlled jitter and failed-link
recovery remain unverified.

Candidate397 also passes repeated established-link loss in attempt
`d1e4b62c-8984-4d09-a48c-d08fcc139fb5`: three separate events each drop
exactly three inbound RTP packets on the active call port. The baseline and
nine recovery HTTP checks pass at49,296bit/s with unchanged PPP identity,
monotonic connection duration and zero six-category RAS counters. Independent
audit verifies all three nft counters, hashes, cleanup, removed injection rules,
and all25 restored baseline files. This extends the observed recovery evidence
to repeated bounded loss in one call; jitter, opposite-direction impairment,
failed-link cleanup and normal V34 selection remain separate requirements.

The exact packaged candidate `397a090` (native `164008c9…624b39f`) passes
a 1,804.810-second V90 endurance call, attempt
`64fac0ad-b5a6-456b-8847-2ad4d1261918`: 224 verified 32-KiB downloads
(7,340,032 bytes), 224 small URL-carried transfers, and 45 public HTTP checks.
Independent verification checks continuous call identity, hashes, timing,
zero six-category RAS counters, final server PASS and notebook disconnection.
Median download payload rate is 40,585.85 bit/s. This does not qualify bulk upload.
The automatic rollback initially refused because server teardown had not yet
completed. Subsequent live inspection found all resources idle; rollback then
succeeded. A fresh baseline call `f4a95e34-7343-444a-a837-33f042179cb3`
passed at 49,296 bit/s, and all 25 restored files and idle endpoints were audited.
The fixture is stopped. The test controller now waits for server teardown as
well as notebook disconnection; the original rollback failure is retained.
The same packaged candidate subsequently passes all ten planned physical calls
in campaign `release397-repeatability-1789268042`, each at49,296bit/s with
PPP/IPCP, the expected559-byte public payload hash, zero six-category modem
errors and resource cleanup. Ten distinct attempts are retained. Installation,
rollback and final live hashes of all25 restored files pass independent checks.
The final call was on the candidate; the subsequent live audit verifies restored
files and idle endpoints, not a fresh baseline call. This closes the bounded
V90 repeatability check for that artifact/profile, not combined-profile or V34
reliability. CI also passes source
`0bbd271` (run 34733177081), whose changes since the packaged candidate are
tests and documentation.

The last notebook API health check reports DialUpLab 1.1.1.0. The tested XP
1.2.0 package provides the bulk-upload API; its installation remains pending.
A second physical modem is also needed for item 5. Neither dependency blocks
continued V.34 diagnosis or release review.

CI completed successfully for `c5962e0` (serial framing), `5f7c5cd`
(channel reset) and `9e1c135` (LAPM integration, run 34724716055). Later
heads require their own final result; queued/running checks are not passed checks.
