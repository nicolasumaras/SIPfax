# Phase-two hardware qualification

For the current deployment and all five release gates, start with
[release-readiness.md](release-readiness.md). The sections below retain historical
trials, including failed and superseded candidates.

> Correction: earlier V.34 MP rate/trellis numbers below were decoded with
> reversed field bit order. See “MP numeric field bit-order correction” before
> using those numbers as evidence of negotiated settings.


The unchanged CT105 native build `70614d7`, SHA-256 `62b8190eed86cb4267447229d9ac1e8af0af0e4cdac93c67dda77204ae922623`, was exercised through the Windows XP notebook and Cisco ATA187.

## Repeated calls

Nine of ten attempts completed PPP and two public HTTP probes. Attempt `2e5a0bf5-50f3-47ec-9a70-d7f04f8f5d0d` reached DeviceConnected and failed during authentication with Windows error 718. The failure is retained; the campaign does not establish reliable cold-call startup. An initial harness cleanup issue changed that failed attempt's API status to Disconnecting, although both the notebook connection list and server session/lease state were empty. The corrected harness resumed the remaining calls without replacing the failed sample.

## Thirty-minute sustained call

Attempt `7e4c771c-d05f-456d-926d-23f885796ead` passed on one PPP connection for 1,802.317 seconds of transfer-test time:

- 224 downloads of 32 KiB: 7,340,032 verified payload bytes (7 MiB).
- 224 uploads of 1 KiB: 229,376 verified payload bytes (224 KiB).
- 45 successful public HTTP probes from the PPP address.
- Final server-side upload verification passed.
- Every expected transfer hash matched, with no failed application checks.
- Final CRC, alignment, framing, timeout and overrun counters were zero.
- Reported modem connection speed was 49,296 bit/s.
- Median download-request payload throughput was approximately 40.6 kbit/s.
- The harness disconnected and the saved final attempt is Disconnected.

Upload payloads were carried in request URLs with per-request confirmation. They prove upstream data integrity under this workload; their throughput is not a bulk upstream capacity measurement. An independent audit recomputed expected payload hashes from fixture bytes rather than trusting the harness success flag.

## Remaining gates

The 60-minute run, initial authentication-timeout investigation, controlled impairment recovery, V.34 fallback, application deployment/rollback qualification, and real simultaneous hardware calls remain incomplete. The subsequent application and routing fixes are staged separately; this result does not validate those undeployed changes. Synthetic two-hook concurrency tests do not replace simultaneous modem-call evidence.

## Application deployment and rollback qualification

Application release `6d06bdf15f27bb289944e69878bb8592d17285cb` was installed on CT105 with the unchanged qualified native binary. CI run `34671135333` passed both jobs, and all 68 application tests passed from the staged source inside CT105. The installer preserved the existing shared configuration directory permissions. PPP credential migration retained existing bytes, moved writable targets into the service-owned mode-0700 directory, and installed mode-0600 files behind root-managed links.

The first upgraded-release call `235015b1-5092-4340-9eb5-71ca4c1129ac` passed two matching public HTTP probes at 49,296 bit/s with zero reported modem errors and disconnected. The previous application, helpers, service configuration and regular credential files were then restored from the verified root-only snapshot. Rollback call `962d2295-ff26-4a08-a04e-def60ce3b7cc` passed the same checks at 49,296 bit/s and disconnected.

After reinstalling the upgraded release, call `fa0e7b35-1966-41d2-882b-f867fa80454e` authenticated and established PPP but failed both 30-second HTTP probes with RequestCanceled. Notebook receive bytes remained at 248, despite zero reported modem errors. During the call, forwarding was enabled and the per-call nftables tables existed; direct HTTP from CT105 worked. These observations do not establish a root cause or exclude a routing defect. The failure remains part of qualification evidence.

A subsequent diagnostic call `a466236c-6124-4487-942a-579dfaee1b6b` passed a 32 KiB download directly from the PPP server and a public HTTP download. A passing retry does not resolve the intermittent failure. The upgraded application remains deployed; release reliability is not yet qualified. A 60-minute run was launched separately and must reach its duration, integrity and cleanup gates before being counted.

## Intermittent-stall investigation

The first 60-minute attempt, `019efedd-e756-42ec-9ac9-2d36539e0d48`, failed its initial public HTTP probe after PPP connected. It completed no transfer rounds and disconnected cleanly. It is a failed attempt, not an endurance pass.

Six subsequent diagnostic calls passed a public HTTP request and a hash-verified 32 KiB download from the PPP server, with clean disconnects. The first three started packet capture after PPP connected; the next three armed capture before dialing to avoid adding a setup delay. Two of those latter calls started public probes approximately 2.4 and 2.5 seconds into PPP and passed. A simple minimum delay after connection therefore does not explain the prior stalls. No production code or configuration was changed during these comparisons.

Diagnostic attempt IDs: `1f7c4071-9e8d-4398-965f-13e73606f16a`, `9c3fa1c1-64c8-42b0-bebe-5faf65404851`, `1969f1ab-685a-416f-8ec8-a9a8063e465d`, `cde91179-cb01-43bc-b342-31633f15a4df`, `519db65b-13f8-4b57-a7d5-056f85369cc9`, and `bcdffe1c-72d0-4f02-97a7-a6af0224f161`. The successful captures show actual PPP TCP handshakes and responses. They do not locate the failure in earlier uncaptured calls or resolve the intermittent defect.

## Local-transfer stall with expanded packet capture

Attempt `022cc49a-e687-4e4b-9711-030c64d6beba` failed `download-37` after about 327 seconds of test time. The local PPP HTTP response began successfully but stopped after 4,380 body bytes. TCP capture shows the notebook acknowledging through sequence 4,496, then the server retransmitting without further notebook acknowledgments. A diagnostic local request failed too. Direct HTTP from CT105 to both public test addresses returned 200 during diagnosis, with IPv4 forwarding enabled and the call's policy present. This failure occurred on the local PPP path and cannot be explained solely by NAT or the public endpoint.

Native logging continued to show V.90 audio activity, but existing logs lack ongoing LAPM frame/queue counters. The optional `SIPFAX_V90_LAPM_DIAGNOSTICS=1` now emits one metadata-only state line per second: decoded/valid frame counts, pending bytes, DTE FIFO occupancy, sequence acknowledgments, busy flags, timer, errors and restarts. It is disabled by default and does not log packet or credential contents. A clean build and framed-audio check with the option enabled passed locally. Hardware qualification of this diagnostic build is a separate next step; no fix to the stall is claimed.

## LAPM recovery regression and candidate fix

A new established-link regression reproduced the decoder-reset defect: the original implementation immediately cleared `connected`. The candidate fix preserves LAPM sequence numbers, retransmission queues and pending DTE bytes, resets partial receive framing, and reacquires a candidate from two CRC-valid addressed HDLC frames without requiring ODP/XID again. In candidate `56cee19`, the original reset behavior still applied before initial LAPM establishment; the later startup fix below changes that case.

Selector tests reject flags alone, a corrupt frame, mixed-candidate evidence and invalid addresses. CT105 AddressSanitizer/UndefinedBehaviorSanitizer tests passed with zero, one and two simulated decoder resets, each transferring exactly 16 KiB in both directions. The reset tests include a one-second physical gap and candidate changes, preserve sequence state at reset, and verify byte order and absence of duplicate delivery. Local sanitizer libraries were unavailable; the sanitized evidence comes from CT105. These tests establish protocol recovery for the simulated conditions; hardware renegotiation and endurance qualification are still required.

## First hardware renegotiation recovery observed

On candidate `56cee19`, attempt `9fdada24-49cd-4bf1-8c92-f5f7f4eb4dce`
remained connected through a real rate renegotiation. The modem log records
S/Sbar and TRN2d at Phase4 time 530.09 seconds, followed by selection from two
CRC-valid resume frames. LAPM counters at runtime 548–550 seconds retain
connected state and outstanding sequence state, then advance acknowledgements;
`resumptions` becomes 1 while `restarts` and protocol errors remain 0.

An interim independent audit of that run at 856 seconds verified 104 downloads
of 32 KiB and 104 upload checks of 1 KiB, plus 21 public HTTP probes. All
completed fixture hashes match and all reported modem error counters are zero.
The audit also checks the recorded connection identity and monotonically
increasing connection duration across probes. This is evidence of recovery
with continued intact traffic on this call, not a completed one-hour result or
proof of recovery under all impairments. The final outcome is recorded below; this interim result did not qualify the run. Upload checks remain URL-carried payloads,
not a bulk upstream throughput measurement.


## Internet-only failure isolated to source-port preservation

The same `9fdada24-49cd-4bf1-8c92-f5f7f4eb4dce` run ultimately failed
`internet-230` at 1,920.5 seconds. It had completed 230 hash-verified 32 KiB
downloads and 230 upload checks. It disconnected cleanly, but did not pass the
one-hour gate or final server verification.

A diagnostic 32 KiB download over the still-connected PPP link passed immediately
after the failed public request. CT105 capture shows the notebook's TCP SYNs
from source port 1080 arriving over PPP and leaving eth0 after NAT, without
replies. Direct CT105 HTTP requests bound to ports 1080 and 4444 reproduced the
timeouts; adjacent-port requests succeeded. A gateway header capture showed the
failed SYNs leaving its WAN interface. This locates the missing replies beyond
that capture point; it does not identify which upstream network or endpoint
filters them.

Temporary high-port NAT restored HTTP 200 for both failing ports. The generated
SIPfax policy was then tested with a temporary source address on idle CT105:
TCP 1080 and 4444 translated to high ports and passed HTTP; UDP 1080 translated
and received a DNS answer. Test rules and the address were removed. Commit
`97e0b15` applies 49152–65535 translation to TCP/UDP per call, retains ordinary
NAT for other protocols, and preserves destination restrictions.

## Startup negotiation recovery and current candidate

The first post-NAT hardware attempt, `46840b0c-7a52-46f0-b75d-e21189ebc7c1`,
failed with modem error 777 before PPP. ODP and flag-based stream selection were
followed by a decoder reset that returned the server to detection. Repeated
physical renegotiations did not restore LAPM. This remains a failed call in the
reliability record, separate from internet routing.

Commit `cc64526` preserves negotiation after stream selection, including before
LAPM is connected. Two new regressions exercise a reset after selection and a
reset after valid XID receipt. The latter exposed repeated two-byte SABM frames
being rejected as reacquisition evidence; valid two-byte U frames are now
accepted, still requiring two CRC-valid addressed frames from one candidate.
The previous established-link cases and both startup cases transfer exactly
16 KiB in each direction. CT105 AddressSanitizer/UndefinedBehaviorSanitizer,
clean native build, and 100-record audio framing checks passed.

The deployed combination is application `1f58ee0` and native `cc64526`, binary
SHA-256 `65bd6c4855c78828e0c0d2fca1fb6177cb4496a016e5c042f389092be32cea15`.
Application `1f58ee0` also guards against signalling a failed PPP spawn without
a child PID; its absence interrupted the staged tests on CT105's Node 24.20.0.
The corrected complete application suite passed 80 tests on CT105.

Hardware attempt `de96b0a5-0457-4875-a3ec-899a40b7fb70` passed at 49,296 bit/s
with two matching public HTTP responses and zero reported modem errors. Cleanup
left no PPP process, address lease, retained forwarding snapshot, or per-call
firewall tables. This is one successful short call, not a reliability campaign.
The captured one-hour run `8b43d200-3179-43d7-b82a-1cb20f331a88` subsequently
passed as recorded below. The later RTP-readiness application change `10b8228`
and modem PID guard `668732c` were not deployed during this run.

During the new hardware run, read-only connection tracking confirmed the actual
PPP client using the new NAT mapping: source ports 1189 and 1200 were translated
to 51381 and 58200 for completed public HTTP connections. An interim independent
audit at 377 seconds verified 47 download/upload pairs with zero reported modem
errors. This confirms live rule use; it is not the final endurance result.

### Completed one-hour qualification on the recovery and NAT fixes

Attempt `8b43d200-3179-43d7-b82a-1cb20f331a88` completed 3600.063 seconds
on the unchanged application `1f58ee0` / native `cc64526` combination.
The independent final audit passed 445 downloads of 32 KiB, 445 uploads of
1 KiB carried in request URLs, and 90 public internet checks. All payload
hashes matched, final fixture verification passed, and reported CRC, timeout,
alignment, hardware-overrun, framing, and buffer-overrun counters were zero.
Median download payload throughput was 40,617 bit/s including request overhead.
These URL uploads do not qualify bulk upstream capacity.

The harness exited successfully and recorded no remaining notebook connections.
A subsequent server check found zero sessions, active lines, PPP leases, or
pppd processes, and no per-call nftables tables. Capture SHA-256:
`e6b8910d7afe4132993ea685d8ad843102f63fbf70963823cf26592120aadde4`.

The final report and independent audit are named
`v90-sustained-8b43d200-3179-43d7-b82a-1cb20f331a88.json` and its `-audit.json`
companion in the operator workspace. Prior failed runs remain part of the
qualification record. This pass establishes one hour on this build, not
repeatable startup, controlled impairment, V.34 fallback, bulk upload, or
simultaneous hardware-call qualification. Both CI jobs for `668732c` also passed;
its pending application changes still need deployment qualification.

### Ten-call campaign after the one-hour pass

On the same application `1f58ee0` / native `cc64526` build, all ten calls in
`v90-reliability-cc64526-post-endurance.json` connected at 49,296 bit/s and
passed two complete public HTTP probes each. An independent audit checked all
20 payload lengths and hashes, PPP source addresses, zero reported link-error
counters, final Disconnected states, and server cleanup evidence. All passed.

The harness stopped after call 1 because an immediate post-disconnect check
observed a PPP lease still being released. A follow-up check confirmed cleanup;
the harness was changed to poll for at most 35 seconds, and the remaining nine
calls resumed without changing the server. Their measured cleanup checks took
about three seconds. This interruption is retained in the campaign report.
These ten successful samples improve the startup evidence but do not erase
previous failures or qualify other builds, V.34 fallback, or simultaneous calls.

### Application update, rollback, and restoration

Application `31f57b9` passed all 84 application tests in CT105 and installer
preflight before deployment. Native `cc64526` was retained byte-for-byte.
Configuration and PPP credential hashes were verified unchanged. The initial
hardware smoke call `20a73069-f4e2-479e-b3d1-d058372c09d1` passed at 49,296 bit/s.

The application rollback snapshot `pre-31f57b9-app.tar.gz` has SHA-256
`505515dc43201968bc60244775292c97f692aae5ea025913345ae38788346579`.
Restoring it returned the application to `1f58ee0`; the three changed JavaScript
files matched the archive, service health passed, and native/configuration
hashes were preserved. Hardware call `252dfec5-57f4-40d6-9e4d-ad34a16db70c`
passed at 49,296 bit/s. Reinstalling the staged `31f57b9` application then passed
those file and preservation checks, followed by successful hardware call
`2f6dd99a-0357-4998-bc6b-a1dd1d36d3b8` at 49,296 bit/s.

Both calls passed two complete matching public HTTP responses, zero reported
link-error counters, and final Disconnected status. An independent audit
verified their records and final server health with zero sessions, leases, and
active lines. The server remains on application `31f57b9` / native `cc64526`.
This qualifies the application update/rollback path; native-binary rollback,
V.34 fallback, controlled impairment, bulk uploads, and concurrent hardware
calls remain separate requirements.

### V.34 fallback acquisition investigation

Two calls with V.90 disabled failed before PPP with Windows error 678. The
second attempt, `219798fb-1fbd-471d-830d-232c7d6c2b08`, reached native `SM_V34`
and repeatedly restarted Phase 2 after failing Phase 3 acquisition. Native
logs are redirected by the configured `linmodem-trial` launcher; the service
journal alone did not contain this evidence. Normal settings were restored.

The captured caller signal contains an S-like alternating phase pattern at
11.03 seconds. The native WAIT_S1 amplitude gate exceeded 13,000 only once in
387,827 logged waiting samples, near the call's end. Offline replay identified
a level mismatch: the existing offline decoder applies fivefold saturating
input gain, whereas the live legacy V.34 demodulator used unscaled PCM.
Replaying the same training segment at the live level stayed in WAIT_S1;
fivefold input advanced through S and PP to TRN.

The candidate change shares a saturating level conversion between that offline
decoder and the live legacy V.34 receive path. Phase 2, transmit output, CMA,
and V.90 paths are untouched. Exhaustive signed 16-bit conversion checks pass;
replay reaches TRN on the training segment and remains in WAIT_S1 for silence
and captured quiet audio. This does not establish rejection of arbitrary noise
or complete training: hardware qualification and V.34 PPP remain pending.

### Negotiated caller MD interval

The receive-level candidate built and passed sanitizer checks in CT105, but
hardware attempt `f0b30b6f-0df2-4769-9d16-d14b8196d74a` still failed before PPP.
The qualified `cc64526` binary and normal configuration were restored.
Subsequent capture analysis decoded a CRC-valid INFO1c with MD length 20 units
(700 ms) from both this call and the preceding fallback retry. The legacy
receiver advances directly from the first S/S-bar pair to the second, without
waiting for that negotiated interval; the PP training position is consequently
not aligned to this caller's requested sequence.

`v34info1c.c` now provides a standalone streaming 1200 Hz/600 bit/s DPSK decoder
and a framing/CRC validator. Both saved captures decode to 700 ms with input
chunks of 1, 13, and 160 samples. Tests cover all single-bit mutations of the
captured frame, every truncation, silence, and all 128 legal MD values. This
module is not yet connected to the live Phase-2/Phase-3 state machine. Timing
integration and a fresh hardware qualification remain required.

The INFO1c decoder is now integrated into Phase 2. A missing CRC-valid message
prevents entry to Phase 3 rather than supplying an assumed interval. The live
legacy receiver waits for the negotiated MD duration, then reacquires the second
S pair; zero MD proceeds directly to PP. The second S-bar duration is corrected
to 16 symbols. Replays of both full caller captures pass the validated 700 ms
value through Phase 2. A separate native segment replay checks exactly 700 ms
of waiting and subsequent S/PP/TRN progression, plus zero-MD control flow.
The zero-MD control uses the same segment to exercise branching, not as evidence
of a zero-MD physical caller. Acquisition timing, equalizer convergence and PPP
still require hardware validation of this integrated candidate.

### S-to-S-bar timestamp prototype

The transition-only hardware trace reached TRN on each retry but failed before
PPP. Offline waveform fitting located the first S-bar boundary at 11.093165 s;
the native amplitude gate entered its S state at about 11.095 s. Counting a full
128-symbol S burst from that boundary placed PP approximately 39 ms late.

The standalone `v34sdetect.c` prototype matches a pulse-shaped 128-S/eight-S-bar
window with unknown carrier phase, using normalized energy and a bounded sample
ring. It reports the transition timestamp rather than treating its decision time
as the start of S. Three captured transitions at 3429 symbols/s matched the
independent symbol-domain fits within 1 ms. Silence, three unrelated tones and
one deterministic noise stream were rejected. Other rates, channel conditions,
noise distributions and live integration remain unqualified. The prototype is
not linked to the production receiver yet.

### Opt-in matched-S receiver integration

The `SIPFAX_V34_MATCHED_S=1` experiment now uses the matched detector's
S-bar timestamp in the legacy receiver. It synchronizes during acquisition,
accounts for the receive FIR's coefficient-centre delay, and waits until the
filtered sample reaches the actual S-bar end before starting MD or PP. It
resets acquisition for the second pair after negotiated MD. Default operation
retains the previous acquisition path; this experiment is not deployed.

Replay of PID 47019's training segment (capture starts at 10.95 s) places
first/second S-bar ends at 0.147875 / 0.900375 s, versus independent waveform
fits of 0.147831 / 0.899785 s. The integration reaches PP and TRN with MD=700;
MD=0 on the same recording checks only the skip branch. This is timing evidence,
not decoded data or hardware PPP success. The replay checker additionally
bounds the PP filtered-sample offset to two 3x-baud samples. The legacy replay
check allows one millisecond of integer-log truncation at MD boundaries.

Validation: local native build, strict standalone detector compilation and
negative controls, `tools/tests/v34-matched-replay.py`, and legacy
`tools/tests/v34-md-replay.py`. Live acquisition, equalizer convergence, other
symbol rates, broader noise/channel tolerance, and fallback PPP remain open.

Live trial of `44265fc` (candidate SHA-256
`3ba454d4dc8a428d461a648d26ed1cc04c8ec4956e6e2862aa652dbe04ea4459`):
CT native build and standalone detector ASan/UBSan controls passed. Notebook
attempt `264964e8-c2bf-451d-8570-113c65dfeb62` failed with Windows 678 before
PPP. All four negotiation rounds acquired both matched S-bar transitions,
waited the CRC-validated 700 ms MD, and reached PP/TRN. First-round PP filtered
sample was 27229.980 against detector boundary 27229.000. This validates live
acquisition execution, not successful equalization or protocol completion.
Logs: `work/v34-fallback-1789228413.*`; RX/TX PID 47440 captures and hashes:
`work/v34-matched-candidate-evidence/manifest.json` in the development workspace.
The qualified native SHA-256 `65bd6c4855c78828e0c0d2fca1fb6177cb4496a016e5c042f389092be32cea15`
was restored, the override removed, and health verified with zero sessions,
leases and media lines. Next inspect training/Phase-4 handling: the legacy TRN
path currently calls the data baseband decoder directly, while caller-J
recognition is implemented in the separate streaming receive path.

### Streaming receiver comparison: J/Phase-4 proceeds, data remains unlocked

Source inspection confirms legacy `V34_demod` sends TRN equalizer outputs into
`baseband_decode_impl` directly, while caller-J recognition is in the separate
streaming/CMA path. Replaying PID 47440 caller audio cropped to 10.8–13.8 s
through `SIPFAX_STREAM_FILE` recognizes J with a J4=192/J16=180 vote.
`tools/tests/v34-caller-j-replay.py` makes that capture check repeatable and
rejects equal-duration silence. It does not test PPP or certify all channels.

A temporary configuration-only trial on qualified native `cc64526` enabled
`SIPFAX_RX_CMA=1` and `SIPFAX_RX_DBG=1`, with V.90 capped out. Attempt
`d967876e-afd6-488e-8024-bd20f013c739` still failed with Windows 678. All four
rounds detected caller J and voted J4. Two block-decoder MP reads reported
ack=1, ca=16800/ac=26400, trellis=2. The final round received E and entered
data acquisition, but measured lattice RMS 0.564 (the decoder's no-lock
reference is 0.577), followed by approximately random descrambled bits. These
are negotiated parameter fields, not an achieved modem connection rate.
This moves the immediate investigation to reliable data acquisition/equalization;
a DATA state transition is not evidence of PPP success.

Evidence: `work/v34-fallback-1789228687.*`, PID 47623 RX/TX audio and manifest,
cleaned native log, negotiation timeline, and audit under
`work/v34-cma-candidate-evidence/`. Verified native checksum unchanged,
experiment override removed, and zero sessions, leases, and media lines after
restoration. No permanent configuration change was made.

### MP integrity and E-length corrections

PID 47623 saved-audio replay reproduced the unlocked data constellation. At a
44 s capture offset, default/400-symbol acquisition/block-FFE seeding produced
lattice RMS 0.565/0.549/0.566: none is a data lock. These are diagnostic
comparisons, not qualified operating settings. The standalone streaming harness
also starts with minimum shaping while live initialization defaults to expanded
shaping, so exact data-mode replay parity remains to be fixed.

Two source defects were corrected independently of the unsuccessful acquisition:
MP header consensus no longer authorizes parameters after a failed complete-frame
CRC, and E requires all 20 descrambled ones specified by V.34 10.1.3.2 (previously
19). E additionally requires CRC-validated MP parameters. The saturating run
counter resets on zero and does not overflow during prolonged ones.

Local native build, strict `tools/tests/v34-e.c` controls, caller-J/silence replay,
and `tools/tests/v34-mp-crc-replay.py` pass. The supplied recording has four
failed MP folds before a CRC-valid fold; failed folds no longer authorize MP/E,
and the valid frame still permits E. Data remains unlocked after that valid
transition. Added E ASan/UBSan coverage to CI; no CI pass is claimed yet.
Artifacts: `work/v34-cma-candidate-evidence/data-replay-comparison.json`,
`strict-crc-replay.json`, and associated replay logs. These changes are not
deployed; qualified V.90 production was not modified in this work.

### Live-initialized replay and echo reconstruction

`SIPFAX_STREAM_LIVE_INIT=1` now initializes the offline stream receiver through
the live demodulator initializer: peer role=caller, initial R=19200, negotiated
3429 baud, expanded shaping by default, and 160-sample processing blocks.
`SIPFAX_SHAPE` and `SIPFAX_STREAM_CALLING` remain explicit overrides. Existing
standalone/synthetic replay behavior is retained without this option.

The PID 47623 RX capture precedes line echo cancellation in `linpipe.c`.
Reconstructing cancellation from the paired RX/TX captures using the existing
`tools/tests/v90-echo-delay-file.c` reproduces the live delay=1428 and
lock_sample=79680. These agree with the live log; they do not alone establish
sample-for-sample equivalence throughout the call.

Post-echo replay beginning at 44 s, with live initialization and block size,
produces exactly the live negotiated RX frame parameters:
R=16800, S=3429, J=8, P=15, N=588, b=40, r=3, K=28, q=0, M=14, L=56,
shape=1, trellis=64. Lattice RMS remains 0.566, so matching initialization
and echo processing does not resolve data acquisition. The initial crop still
omits earlier receiver history; full DSP state parity is not claimed.

Local native build and `tools/tests/v34-live-init-replay.py` pass, along with
caller-J/silence and MP-CRC replays. An initial edit applied the variable block
size to the wrong harness and failed compilation; it was corrected before these
checks. Artifacts are under `work/v34-cma-candidate-evidence/`, including
`postecho-audit.json`, reconstructed PCM and replay logs. No production changes.

### E-to-data trace: acquisition skips B1

Added opt-in offline `SIPFAX_STREAM_STATE=<csv>` diagnostics and
`tools/v34-state-summary.py`. The CSV records the 24 kHz frontend counter,
receiver symbol count, E/data state, acquisition/decode counters, equalizer
outputs, and tracked carrier phase. It does not enable additional live logging.

On post-echo PID 47623 replay starting at 44 s, E is observed at trace time
51.755083 s. Equalizer-output power kurtosis changes from 1.003 in the preceding
200 ms to 1.763 in the next 50 ms and stays near 1.8 for about a second. This
supports a waveform change at E; it does not prove the subsequent symbols decode
correctly. A long constant-amplitude training tail is not supported in this case.

The first data-decoder symbols occur 583.667 ms after E, following collection of
2000 symbols. V.34 10.1.3.1 defines B1 as one data frame of scrambled ones with
reset encoder state. At the negotiated P=15 and eight symbols per mapping frame,
that is 120 symbols, approximately 35 ms. The acquisition path therefore skips
B1 entirely. B1 handling and data/superframe alignment need examination before
another blind gain/phase tuning exercise. No live test or production change.

Local native build, live-frame-parameter replay and MP CRC replay pass. Evidence:
`work/v34-cma-candidate-evidence/state-trace.csv`, `state-replay.log`, and
`state-window-audit.json`. Corrected the previous E clause reference to 10.1.3.2.

### B1 reference and transmitted MP verification

Added `SIPFAX_B1_REFERENCE=<path>` to export one caller B1 frame as Q7 symbol
coordinates from the existing encoder. `SIPFAX_B1_RATE`, `SIPFAX_B1_TRELLIS`,
`SIPFAX_SHAPE`, and six Q14 coefficients in `SIPFAX_B1_H` configure the reference.
Defaults are 16800/3429, 64 states, expanded shaping, zero taps and no nonlinear
encoding. Production B1 reset and this reference share `v34_begin_b1`; the
production reset operations are unchanged. This is not an independent standards
reference or evidence of hardware interoperability.

The local reference test verifies 120 symbols, deterministic reset, shaping/
trellis controls, nonzero-tap effects and rejection of malformed taps. Native
build and live-frame replay also pass; reference test added to CI.

Decoding the actual PID 47623 transmitted MP around 50.5 s yields 36 frames:
ca=16800/ac=26400, trellis=2, shape=1, ack=1, nonlin=0, with nonzero h. This
supports the parameter assumptions from waveform evidence rather than defaults.
Rounded Q14 taps from its four-decimal printout are
4476,1768,-3722,329,2710,-924 (approximately one unit uncertainty).

A constant phase/gain correlation over offsets -20 through +40 symbols around E
finds maximum coherence 0.038 without precoding, 0.069 with these approximate
taps, including conjugated hypotheses. Neither establishes B1 alignment.
Residual channel effects and encoder-reference correctness remain unresolved;
this does not show the caller sent invalid B1. Artifacts:
`work/v34-cma-candidate-evidence/{b1-reference-alignment.json,b1-precoded-alignment.json,tx-mp-decode.log}`.
No live calls or deployment in this work.

### Independent B1 framing check and bounded channel comparison

`tools/tests/v34-b1-framing.py` independently computes the caller scrambler
recurrence from 1 + D^18 + D^23 (clause 7), then checks all 588 bits of the
16800/3429 B1 reference. It verifies the fifteen 39/40-bit mapping-frame lengths,
60 four-dimensional intervals, and Table 12's final-half-frame inversion pair
"10" at half-frame indices 14 and 15. These checks pass and are added to CI.
They validate framing, not shell mapping, symbol coordinates, precoding, or the
complete B1 waveform.

A diagnostic comparison fitted 1/3/5/7-tap stationary symbol-spaced linear
channels on alternating reference symbols and evaluated the others. The known
three-tap synthetic channel gives explained held-out power 1.0. The best
capture fit over offsets -200..200 and both conjugation hypotheses gives 0.101,
using three taps at offset -20. The initial narrower search ended at that
boundary, so it was widened; the wider search did not improve the result.
This does not resolve B1 alignment, and does not rule out transmitter-reference
errors or other channel effects. Evidence:
`work/v34_b1_channel_check.py` and
`work/v34-cma-candidate-evidence/b1-channel-audit.json`.
No native algorithm or production configuration changed in this work.

### Corrected-label branch table was silently missing

Figure 9 confirms that the middle subset bit is x0. The existing
`SIPFAX_FIG9=1` path selected `trellis_trans_16_fig9`, but no source defined
that table: `v34priv.h` contained a tentative definition and `-fcommon` silently
linked 1024 zero bytes. `nm` reported a BSS symbol, and a clean 16800-bit/s
symbol round trip scored 49.9%.

Added explicit `vendor/linmodem/v34fig9.c`, generated by
`tools/gen_trellis_trans_16.py --label x0 --rot 1`, included it in the native
build, and changed the header to `extern`. An absent definition now fails at
link time. `tools/tests/v34-fig9-table.py` checks all 16 labels against a literal
Figure 9 transcription and verifies 256 unique generated tuples. The legacy
construction still matches all 32 blocks of the shipped original table.

This repairs a build defect, not full corrected-label decoding. Clean corrected-
label round trips are 69.8%, 65.6%, and 83.4% at 7200/16800/33600 respectively;
the default labeling scores 99.7% at 16800. The generator's older claim of 100%
for the corrected path is not reproduced by the current decoder. Further branch/
state consistency work is required; the default remains unchanged. B1 framing
regression passes. No hardware trial or deployment performed.

Evidence: `work/v34-cma-candidate-evidence/fig9-before-table.log`,
`fig9-restored-audit.json`, `fig9-default-regression.log`. A Figure-9 B1 reference
still gives only 0.103 held-out explained power in the bounded capture/channel
comparison (`b1-fig9-channel-audit.json`), not a valid alignment.

### Corrected-label branch halves derived from modulation geometry

The restored Figure-9 table still misclassified about half of clean emitted
branches. The former generator chose branch halves using a rotation heuristic.
From clause 9.6.1, Z1=Z0+2*I0+U0 mod 4, while base constellation coordinates are
both 1 mod 4. Thus the correct half is directly
`U0=(u0 XOR v0 XOR u1 XOR v1)&1` in the table's coordinate-coset representation.
The revised generator uses that parity with the Figure-9/Table-13 transition.
It reproduces all 32 original-label table blocks and matches all 16000 recorded
corrected-label encoder branch tuples. The old rot=1 construction matched 7947.

After regeneration, both original and corrected-label clean traceback states
match encoder states exactly after the 29-symbol decoder delay (checked after
initial transient). Corrected-label bit agreement over the whole scored runs
is 99.6–99.9% across 7200/16800/33600 and shaping on/off. The remaining errors
are confined to startup; with expanded shaping, no errors remain after two
280 ms superframes over 63033/147078/181184 subsequent bits respectively.
This is symbol-level decoder evidence, not successful startup/audio/PPP.
`tools/tests/v34-fig9-state.py` preserves that distinction and runs in CI.
An initial fixed 10000-bit settling cutoff failed at 33600; the check now uses
the protocol-duration cutoff of two superframes and reports all startup errors.

Artifacts: `fig9-state-before-parity-audit.json` (recorded pre-change tool
results), `fig9-state-after-parity-audit.json`, `fig9-emitted-membership.json`,
and `fig9-parity-audit.json` under `work/v34-cma-candidate-evidence/`.
The default labeling remains unchanged; no deployment or live call performed.
Next: B1 and initial superframe alignment, rather than settled trellis tracking.

### Startup error source isolated with independent synchronization control

At 16800 with expanded shaping and corrected labeling, automatic synchronization
has 579 startup bit errors, the last at bit 8873. Supplying the correct sync
sequence through the existing diagnostic oracle produces 0 errors across all
156486 compared bits. `tools/tests/v34-startup-sync.py` then reproduces this
without using encoder dumps: it constructs the sequence independently from
Table 12's J=8 pattern and the 30-four-dimensional-symbol half-frame interval.
A one-4D-symbol phase shift produces 10423 errors, providing a negative control.

This confirms that the corrected symbol decoder can decode the clean signal
from the beginning *given the right synchronization phase*. It does not establish
automatic alignment, correct B1 detection, or operation on captured audio. The
next receiver requirement is to obtain and retain that alignment from B1 rather
than an oracle, while preserving the samples currently discarded by acquisition.
The new regression and the three-rate state/settled-decoding checks pass locally;
new regression added to CI. CI for 6b2e439 was still running at last observation;
bc02b04 run 34704994670 completed successfully. No deployment or hardware call.

Evidence: `work/v34_startup_oracle_check.py` and
`work/v34-cma-candidate-evidence/startup-oracle-audit.json`, with separately
preserved automatic/known-sync input/output bits and logs.

## Bulk upstream fixture and XP client ready (2026-09-12)

DialUpLab commit 4e3361013ee4de3d74105470ec6a71d78c73f034 adds bounded,
PPP-source-bound POST uploads in XP 1.2.0. Windows CI run 34705848796 passed,
including exact 12,345-byte and 1 MiB receipt, checksum/source binding,
cancellation and lock recovery. PR1 includes the change. Portable ZIP SHA256:
`0c3506ca3f2098adaaa9f1d641b6e40f3cd8d9a75f162cff98d0e68976fc6a91`.

The new `tools/ppp-upload-fixture.py` records actual received length, SHA256,
pattern validity and peer. Four tests pass, including real HTTP receipt and
corruption rejection. The initial loopback test was denied by the local socket
sandbox; rerunning with network permission passed. `--freebind` allows staging
on the idle PPP address. See `tools/ppp-upload-fixture.md` for acceptance gates.

CT105's temporary sipfax-ppp-upload-fixture service was verified active and
listening on 10.64.0.1:8084 with a two-hour lifetime. SIPfax health showed zero
sessions, media lines and leases. The laptop still reported XP 1.1.1.0 and no
connections at 16:45 UTC; installation of the built 1.2.0 package was requested.
No real bulk upload has been qualified yet, and no production modem/application
binary was changed. All five release objectives remain open.

## Captured startup carry comparison (2026-09-12)

Using the same live-initialized post-echo capture, the existing NO_REACQ option
feeds 8,285 symbols and starts decoding 0.292 ms after E, versus 6,285 symbols
and 583.667 ms with default 2,000-symbol acquisition. A 64-symbol acquisition
starts at 18.958 ms and feeds 8,221 symbols; its lattice RMS is still 0.508
(default 0.566, with 0.577 representing no lock). These are replay observations,
not hardware qualification.

A constant-complex-scale fit to the approximate precoded Figure 9 B1 reference,
trained on alternate symbols and evaluated on the others, found at most 0.027
explained power for carried startup across offsets 0..40 and conjugate variants.
The default and short-acquisition streams scored at most 0.037 and 0.045.
This limited model does not establish caller waveform validity, but it gives
no basis for treating NO_REACQ as a B1 recovery fix. Startup symbols can already
be preserved with an existing option; recovering their channel/timing and
validating the B1 reference remain necessary before changing production.

Evidence: `work/v34_startup_carry_check.py` and separate baseline/carry/short
logs, symbol files, state traces, binary/capture hashes and audit JSON under
`work/v34-startup-carry-evidence/`. No production changes or hardware calls.

## Controlled single incoming RTP loss fails recovery (2026-09-12)

Baseline hardware call 87ab3991-3cac-4291-8b15-1a882dc07976 connected at
49,296 bps, completed the example.com probe with zero RAS errors, and cleaned
up. The first fault attempt ddb15fbc-70a0-4a96-93f0-234b15fae59a failed nft's
syntax check before installing any rule; it is retained as a harness failure.

Corrected attempt d08c3a08-f09f-43ec-8f9a-67e7aac1de56 connected at 49,296 bps.
After a successful baseline probe, a temporary input rule on its allocated RTP
port 40000 dropped exactly one packet (nft counter verified), then was removed.
The next probe failed with ConnectFailure after 10,854 ms. Native logs show a
physical decoder reset followed by reacquiring=1 and resumptions=0 until call
teardown. Thus this run fails one-packet recovery; nominal endurance is not
impairment tolerance. Three post-loss successful probes were required but the
first failed. No claim is made about eventual recovery after a longer wait.

Cleanup verified zero SIP sessions, PPP leases and media lines. A subsequent
firewall inventory showed only the original inet filter table. Reports and
native log: `work/v90-controlled-loss-*.json` and
`work/v90-controlled-loss-d08c3a08-native.log`; scoped injector and harness are
`work/v90_scoped_packet_loss.py` and `work/v90_controlled_loss_qualification.py`.
No production binary or persistent firewall configuration changed.

The health snapshots also exposed ordinary pppd stdout log lines being parsed
as notification JSON. The supervisor now emits plain lines as pppd-log and
reserves JSON parsing for object-shaped messages. A regression covers split
logs mixed with a split ip-up event and malformed JSON. All 85 application
tests pass using the bundled Node runtime with local socket permissions.
The initial run without Node on PATH/socket permissions failed and was rerun
with those prerequisites. This change is not deployed. It removes false JSON
errors; the live missing lifecycle notification (state remains starting despite
PPP connectivity) still requires correction and hardware verification.

## PPP hook lifecycle wiring (2026-09-12)

The existing ip-up/down routing helper already posts events to /ppp/events,
but the operator endpoint only retained history; it never notified the pppd
supervisor. Commit 1dc5cb2 connects these events to session state with a random
per-process token in the root-readable lease descriptor and an HTTP header.
The supervisor checks the current token, event type, PPP interface and assigned
addresses. A token from a replaced process cannot update its successor. Tokens
are omitted from public history and supervisor diagnostics.

All 86 application tests pass, including a real HTTP regression for valid,
missing, wrong, stale and wrong-address events, plus split pppd log handling.
The application changes from deployed 31f57b9 are exactly four files: index,
operator, pppd-supervisor and egress helper. They were applied to CT105, including
the installed /usr/lib/sipfax helper copy, with startup-failure rollback.
Backup: /opt/sipfax/releases/pre-1dc5cb2-lifecycle.tar.gz, SHA256
16924998b42cc3bc1bd8dffc25197ca7f5f6df20669ef992ae23925874d3991c.
Native SHA remains
65bd6c4855c78828e0c0d2fca1fb6177cb4496a016e5c042f389092be32cea15.
The first startup HTTP check saw connection refused; the bounded retry succeeded.
Deployment script, overlay archive and hash audit are under work/ with the
sipfax-lifecycle / deploy_ppp_lifecycle names. Hardware verification follows.

Hardware attempt abb6c464-16fc-4f96-ac23-3ed2aa8beb46 passed: 49,296 bps,
559-byte public HTTP response with the expected ff67a9d7...871a299d checksum,
zero RAS errors, supervisor and controller ipcp-open, interface ppp0, and no
false JSON error. After disconnect, active sessions, leases and media lines
were all zero. The independently inspected report accepted=true is
work/v90-ppp-lifecycle-abb6c464-16fc-4f96-ac23-3ed2aa8beb46.json.
This qualifies the lifecycle fix on one real call, not impairment recovery,
V.34 fallback, bulk upload or concurrent hardware operation.

## Erasure recovery isolated to feedback adaptation (2026-09-12)

The failed hardware recording contains exactly one zero run of 160 samples at
sample 274880 (34.36 s), matching the injected 20 ms loss. RTP continuity
preserved sample time. A rejected mapping frame invokes qam_bits(NULL), which
invalidates LAPM candidate selection; this alone does not explain persistent
failure after the erasure.

Added optional --erase-sample/--erase-samples to the independent upstream PCM
harness. It retains CRC, ordering and exact final-frame checks, while allowing
frames spanning a gap to be lost. The default no-loss acceptance remains exact.
At 28,800 bit/s, PCMU, +/-100 ppm, the no-loss control passes. Injecting 160
zero samples at sample4000 fails at the first -100ppm case: only expected
frames0..7 of24 arrive, and the final frames do not recover. An 80-sample gap
also fails with8/24. This reproduces a physical receiver failure independently
of LAPM reselection. The failing erasure mode is a diagnostic, not a green CI
gate or evidence of qualification.

A temporary source control disables provisional equalizer feedback only while
symbols1450..1899 are processed, bracketing the known injected gap. No-loss
control passes; the 20ms erasure recovers23/24 frames (only frame8 lost) at both
+/-100ppm through direct and delayed-E paths, with all final frames exact.
Disabling feedback for the entire signal fails even without loss (14/24), so
permanent disablement is not a fix. The window uses known fault timing and
is not an automatic receiver solution. Next implement a signal-derived
adaptation guard with sufficient history coverage, then test shifted erasures,
clean controls, captured audio and hardware LAPM recovery.

Evidence: work/v90-controlled-loss-d08c3a08-evidence/zero-runs.json and preserved
synthetic/control logs; work/v90_erasure_feedback_control.py and
work/v90_erasure_feedback_window_control.py with separate source copies/logs
under work/v90-erasure-feedback[-window]-evidence. Production remains
application1dc5cb2/nativecc64526; no native changes were deployed.

## Automatic energy-based erasure guard candidate (2026-09-12)

Commit cd1434a adds opt-in SIPFAX_V90_ERASURE_GUARD=1. An eight-sample power
window detects energy below5% of the running reference after B1. It pauses
provisional equalizer feedback through matched-filter, equalizer, survivor
and saved-feedback history, while all clocks and decoding continue. It uses
signal energy, not known injection timing. Defaults remain unchanged.

The original20ms gap now recovers23/24 frames, and all six additional cases
pass: clean28800,10ms gap, shifted20ms gap at6500,60ms gap, clean26400, and
28800/3000baud20ms gap (PCMU with +/-100ppm). Native build passes. Added clean
and20ms opt-in CI checks; remote completion not yet verified. Evidence:
work/v90-erasure-guard-evidence and work/v90_guard_matrix.py.

First hardware trial4cb31917-1f03-497a-bbf3-e323de172d56 failed before modem
startup because the locally built executable required GLIBC_2.43. Qualified
binary restored and idle health verified. Built committed source in CT105
with bundled vendor/spandsp-v42 (first partial archive omitted that dependency;
completed build and dynamic-loader check then passed). Compatible candidate
SHA cdb345909a8a2793b58b72949263a314c47736bd609f9f9a9e502ef4ac298ca6.
Trial645db964-4790-4b23-8374-a1163ed8e8b4 reached B1/ODP/XID/LAPM but ended
with619 before PPP, so no packet was injected and hardware guard acceptance
remains unproven. Qualified binary restored. A guard-disabled comparison of
the identical candidate is needed to isolate startup behavior.

The CT compiler additionally exposed b2s[4][2] being indexed at2 and3 during
V.34 16-point training. Expanded its second dimension to4. Native rebuild
and live-initialized replay pass frame-parameter checks, but replay lattice
RMS remains0.566 (no lock). Local bounds-sanitizer linking failed because the
runtime libubsan.so.1.0.0 is absent, so no sanitizer pass is claimed. The
sanitizer attempt and logs are retained under work/v34-training-bounds-evidence.
The V.34 fix is not deployed and does not qualify fallback.

The identical compatible candidate with guard=0 also failed619 before PPP
(attempt02621e5f-36fe-4e6e-880b-80fe870569fe). Both failures occurred before
packet injection; they do not qualify or disqualify post-connect guard recovery.
The qualified native was restored in each case. Comparison with cc64526 shows
additional V.34 training/init changes in the full candidate, so the next
isolation is a build from cc64526 with only the four guard files changed,
plus a qualified-binary hardware control. Preserve both failed candidates:
work/v90-guard-hardware-1789233409.* and
work/v90-guard-disabled-control-1789233522.*.

## Isolated guard passes one-packet hardware recovery (2026-09-12)

Qualified-native control a1de2983-361b-4669-8e18-6a741cd6d6be passed49,296bps,
HTTP checksum, ipcp-open/ppp0 and clean teardown. Built cc64526 plus exactly
the four guard files from cd1434a in CT105, excluding subsequent V.34 changes.
Source manifest and loader/build evidence: work/build_isolated_guard.py and
work/v90-erasure-guard-evidence/isolated-build-audit.json. Candidate SHA256:
33e3d28c1d7c7d5661a2e4509303a9a52575ae4332f8daec7883f5972d086135.

Attempt da792ecf-c8a1-4b88-89df-5d6c608897c7 passed: baseline49,296bps, exactly
one incoming RTP packet dropped on the allocated port (nft counter), followed
by three complete HTTP200 probes on the same PPP connection with the same
559-byte content SHA256. All six RAS error counters remained zero. Native
logs show continuous connected state without physical decoder reset during
recovery. Call teardown verified zero sessions/leases/media lines. Qualified
native restored and experimental override removed by the trial wrapper.

This is hardware evidence for one controlled20ms incoming loss on one call,
not jitter tolerance, arbitrary loss, V.34 fallback or multiple simultaneous
calls. It also narrows full-candidate startup failure to changes outside this
isolated guard source combination; the full development native still needs
startup investigation before release.

Evidence: work/v90-controlled-loss-da792ecf-c8a1-4b88-89df-5d6c608897c7.json,
work/v90-isolated-guard-hardware-1789233774.{json,call.log,native.log}.
Full CI for application1dc5cb2 and docs8867107 succeeded; runs34707043244 and
34707118818. Guardcd1434a and latest495416b runs remained in progress at check.

The three-packet burst extension also passed on the same isolated candidate:
attempt2136108c-5352-407e-8c61-dafe9c1a748c, exactly3 incoming RTP packets
counted as dropped, three subsequent complete checksummed HTTP probes on the
same49,296bps PPP connection, all six RAS error counters zero. Cleanup report
shows zero sessions/leases/media lines. Evidence:
work/v90-controlled-loss-2136108c-5352-407e-8c61-dafe9c1a748c.json and
work/v90-isolated-guard-three-packet-1789233916.*. The injector now accepts a
bounded1..3 packet count; prior one-packet evidence is unchanged.

Final remote check confirmed qualified native SHA65bd6c48...3cea15, no guard
override and only the original inet filter firewall table. The isolated guard
is not the permanent deployed native. XP API still reports1.1.1.0, so bulk POST
qualification remains pending its update.

## Current full native passes startup and burst-loss controls (2026-09-12)

Built complete native source f732aa02631e231db833b3e2aefb4242278f9e87 inside
CT105, including the V.34 four-bit buffer fix. Dynamic-loader check passed.
Binary SHA58365ad1823ed7821561adb09b7de2778aa91330edaaff1e832696927e229a77;
build/source manifest under work/v90-erasure-guard-evidence/full-f732aa02631e*.

Guard-disabled startup control bbd45c7e-b317-43b5-b72a-91e61670c10d passed at
49,296bps with exact HTTP response, ipcp-open/ppp0 and clean resource teardown.
Guard-enabled three-packet test1df82400-983f-40c5-94de-3023a3e45fad also passed:
exactly3 RTP packets dropped, same PPP connection, three subsequent exact HTTP
responses, all six RAS error counters zero and zero sessions/leases/media lines
after teardown. Both reports accepted=true were independently inspected.

These two passes show that the current full build can start and recover from
this burst. They do not establish that the V.34 buffer fix alone caused the
startup improvement or that the earlier619 failures cannot recur. Repeatability
and endurance on this full native remain required before permanent deployment.
Wrappers restored qualified native65bd6c48...3cea15; final remote checksum,
zero resource counts and absence of the guard override were verified.
Evidence: work/v90-full-boundsfix-control-1789234260.*,
work/v90-full-guard-three-1789234343.* and corresponding attempt JSON files.

Deployment hardening: install preflight now checks ELF workers with ldd and
rejects missing dependencies/version reports before installation. Tests cover
an invalid ELF, a loadable executable and a compiled worker whose shared
library is removed. Targeted regression passes. This prevents the observed
class of executable-but-unloadable deployment failure; run checks on the
target host. Commitsd577ddc and923f7f6. Production files were not changed by
this installer edit.

## Ten-call full-candidate repeatability passed (2026-09-12)

Full f732aa0 native SHA58365ad1823ed7821561adb09b7de2778aa91330edaaff1e832696927e229a77
with erasure guard enabled passed10/10 sequential hardware calls. Independent
audit verified49,296bps on every call, source10.64.0.2, HTTP200/559 bytes with
exact expected checksum, all six RAS error counters zero, ipcp-open/ppp0 and
zero sessions/leases/media lines after every teardown. The trial then restored
the qualified native65bd6c48...3cea15; final checksum, idle health and absence
of the override were checked remotely.

Evidence: work/v90_full_guard_repeatability.py and
work/v90-full-guard-repeatability-1789234549.{json,native.log,progress.json},
per-call logs and independently generated -audit.json. This qualifies this
specific candidate's repeatability, not later V.34 changes made during the run.
Candidate endurance, bulk POST, broader jitter/failure recovery and simultaneous
hardware calls remain open.

Build reproducibility: commit6c72c60 makes the tested GNU99/_GNU_SOURCE/fcommon
flags default in the native Makefile. The default-build regression passed:
fresh build, native and SpanDSP header-triggered rebuilds, no-op build and clean.
Guard behavior/opt-in configuration is now documented in the deployment guide.
CIcd1434a run34707635268 and991bf54 run34707424916 succeeded at the last check.

## V.34 invalid-ring frame erasure (2026-09-12)

Using a copied CT105 libubsan runtime locally enabled the blocked bounds replay.
Both original and four-bit-buffer-only variants failed at rings_to_index:
index178 into a137-element lookup array. The four-bit buffer fix alone did not
address this separate noisy-data path. New ring validation rejects values
outside0..M-1 before lookup. Invalid mapping frames and the following23-bit
descrambler recovery interval are suppressed while frame and descrambler clocks
advance. SIPFAX_RX_BIT_POSITIONS optionally records emitted bit positions for
offline validation; default output remains unchanged for valid frames.

The same captured replay now completes with no bounds runtime error. Clean
Figure9 traceback/settled-data tests pass at7200/16800/33600: zero settled errors
and no settled bits erased. At7200,239 invalid startup bits are suppressed;
other rates suppress none. Position-aware comparison accounts for erased
startup bits rather than misaligning every later comparison. The initial
comparison failed after suppression shifted positions; at33600 the trace also
outlived the capped saved-bit sink, so comparison now checks the corresponding
trace prefix. Correct known-phase startup remains0/156486 errors; wrong-phase
control still fails. These are decoder/memory-safety checks, not V.34 lock or
hardware fallback qualification. The native used in the ten-call campaign
predates this change and was not modified while running.

Evidence: work/v34_ring_erasure_bounds_check.py,
work/v34-ring-erasure-bounds-evidence/{audit.json,before.log,after.log}, and
work/v34-training-bounds-runtime-evidence for the preceding failure. New change
commit9a5b186 remains undeployed.

## Guarded candidate endurance failure and reset fix (2026-09-12)

The ten-call candidate `f732aa02631e`, SHA-256
`58365ad1823ed7821561adb09b7de2778aa91330edaaff1e832696927e229a77`,
failed its one-hour hardware qualification after 648.942 seconds. Attempt
`15ea1ffe-4b5c-45fb-bd40-b58b860b5650` completed 77 transfer rounds before
`download-77` returned HTTP 200 followed by IOException and loss of the RAS
connection. The audit verified 2,523,136 download bytes and 78,848 bytes carried
in request URLs before the failure. The last available RAS counters were zero;
counters for the failed probe are unavailable. This is not endurance acceptance
or bulk upstream qualification.

In the same call, the first rate renegotiation at phase time 185.650 seconds
resumed LAPM after changing timing candidates. The second at 646.5025 seconds
reached CRC-valid CP/CP-prime and B1 correlation 0.9529, but no physical-reset
notification reached LAPM; valid-frame count remained 24,213 until termination.
The upstream structure was reinitialized at E, while selector invalidation
waited for a first mapping frame from the old selected lane. A lane that never
reacquires can therefore leave LAPM ignoring the replacement candidates.

Commit `9c493c3` invalidates every candidate immediately before that upstream
reset. The E-recovery regression fails before the change and passes afterward.
The LAPM integration test now invalidates all lanes and never returns the old
selected lane. All five cases transfer 16,384 exact bytes each way, including
two resets and resets during negotiation. Existing renegotiation training tests
and the native build pass. Local LAPM tests were uninstrumented; sanitizer CI
and physical recovery acceptance are separate gates.

The failed trial restored production native SHA-256
`65bd6c4855c78828e0c0d2fca1fb6177cb4496a016e5c042f389092be32cea15`;
SSH independently confirmed that hash and zero active sessions/leases.
Retained local evidence:

- `work/v90-full-guard-endurance-1789235553.{json,native.log,transfer.log}`
- `work/v90-sustained-15ea1ffe-4b5c-45fb-bd40-b58b860b5650{-audit,}.json`
- `work/v90-guard-endurance-failure-evidence/manifest.json` and paired audio
  captures for native PID 51894, plus the PPP packet capture.

A fresh full native `9c493c3` was built on CT105 in a separate directory and
passed the loader check. Its SHA-256 is
`e62a02b2f2868b096b61957b666cdabb8f25815b2bf47f56926431f6bfea55ea`.
It includes the newer V.34 changes, so it is a different qualification candidate.
Its hardware trial was started with automatic production restoration; a start
is not a pass. The earlier failure remains a release blocker until stronger
recovery and endurance evidence resolves it.

### Captured-audio recovery comparison

The failed candidate's paired audio has no zero-filled run of at least 16 samples
(2 ms) after 30 seconds, and both directions retain energy around the two
renegotiations. This does not exclude other network or analogue impairments.
Offline echo reconstruction reproduces the live delay of 1428 samples and lock
at sample 96480.

Fresh upstream receivers on cropped post-echo input find B1 scores matching the
live log: 0.963553 for the first renegotiation and 0.952891 for the second.
The second crop contains CRC-valid LAPM frames on alternative lanes. With the
old selector seeded to lane 1, the replay delivers no valid frames and never
selects a replacement. Immediate global invalidation selects lane 8 and delivers
19 valid frames. It later loses selection again; this supports the stale-lane
mechanism but is not sustained recovery or a reconstruction of all live state.
Fresh crop origins also change lane numbering relative to live operation.

Evidence: `work/v90_failed_retrain_lanes.{c,py}`,
`work/v90_failed_retrain_selector.{c,py}`, and the audio-gap, postecho,
lane-replay and selector-replay audits in
`work/v90-guard-endurance-failure-evidence/`.

CI for recovery commit `9c493c3`, run `34710948313`, completed successfully.
Hardware endurance is a separate gate and remains pending during the current
trial. Its first observed renegotiation at phase time 364.350 seconds performed
immediate invalidation and resumed LAPM on candidate 0. An interim independent
checksum/counter audit at 1317.964 seconds covers 355 successful probes; this
partial result does not qualify the requested one-hour session.

## Recovery candidate one-hour hardware PASS (2026-09-12)

Full native `9c493c3fb06ffe4c4e722ce8cc5dbd535af8bac5`, guard enabled,
SHA-256 `e62a02b2f2868b096b61957b666cdabb8f25815b2bf47f56926431f6bfea55ea`,
completed 3605.869 seconds on attempt
`13e84b91-8a75-47e4-b7ae-153d66ae37c2`. Independent verification found:

- 441 checksum-valid 32 KiB downloads: 14,450,688 payload bytes.
- 441 verified 1 KiB URL-carried uploads: 451,584 bytes, not bulk POST.
- 89 public internet checks and successful final receiver verification.
- All six RAS error counters zero, consistent connection identity and duration.
- One natural rate renegotiation, immediate decoder invalidation, and successful
  LAPM reselection. Protocol restarts remained zero until final disconnection;
  the final disconnected state records one restart. Raw HDLC CRC failures were
  present and recovered; zero RAS errors does not mean every HDLC frame was valid.
- PPP capture contains 31,552 packets on Linux ARPHRD_PPP interface index 163,
  spanning 3607.566 seconds and including both fixture and public internet
  traffic. Complete pcap records and its retained SHA-256 were verified.
- After teardown, no notebook connections, SIP sessions, PPP leases or media
  lines remained. Independent SSH verification confirmed the original native
  SHA-256 `65bd6c4855c78828e0c0d2fca1fb6177cb4496a016e5c042f389092be32cea15`
  was restored and the temporary guard override removed.

Evidence: `work/v90-reset-guard-endurance-1789237479.{json,native.log,transfer.log}`,
`work/v90-sustained-13e84b91-8a75-47e4-b7ae-153d66ae37c2-audit.json`,
`work/v90-reset-guard-endurance-{restoration,route}-audit.json`, and
`work/v90-reset-guard-endurance-ppp.pcap` (SHA-256
`d72467f70449ce29305efafec8bb6ba472729715c5e2e5839bc921291206c813`).

This establishes one-hour transfer integrity and observed natural recovery for
this exact candidate. It does not replace repeated-call qualification of this
new build, broader controlled impairments, genuine bulk uploads, V.34 hardware
fallback, full release deployment/rollback or simultaneous hardware calls.
The failed predecessor run remains retained.

## Exact recovery candidate loss and repeatability PASS (2026-09-12)

The same CT105-built `9c493c3` binary, SHA-256
`e62a02b2f2868b096b61957b666cdabb8f25815b2bf47f56926431f6bfea55ea`,
with the erasure guard enabled passed two additional hardware gates:

- Three deliberately dropped incoming RTP packets on the active call's port.
  The nftables counter confirmed exactly three drops. Attempt
  `c4fc7b59-09ee-482a-9858-e5fdc955132d` retained PPP and completed three
  post-loss HTTP probes, all matching the expected 559-byte response hash,
  using the PPP source address with zero RAS errors. The rule was removed.
- Ten consecutive calls, all at 49,296 bit/s, with independently verified HTTP
  checksum, zero RAS errors, `ipcp-open` state, and zero sessions/leases/media
  lines after each teardown. Campaign `v90-reset-guard-repeatability-1789241429`
  completed without a failure.

The original production binary was restored after both campaigns. Final SSH
verification confirmed its expected hash, no guard override, and an idle server
and notebook. Evidence: `work/v90-controlled-loss-c4fc7b59-09ee-482a-9858-e5fdc955132d-audit.json`,
`work/v90-reset-guard-three-1789241306.*`,
`work/v90-reset-guard-repeatability-1789241429{,-audit}.json`, per-call reports,
and `work/v90-reset-guard-repeatability-restoration-audit.json`.

These results establish the same candidate's one-hour session, ten-call
repeatability, one observed natural renegotiation and a three-packet loss case.
They do not establish arbitrary impairment tolerance, working V.34 fallback,
bulk POST throughput, simultaneous physical calls, or final release validation.

## Qualified native baseline promoted on CT105 (2026-09-12)

CT105 now retains native `9c493c3` with SHA-256
`e62a02b2f2868b096b61957b666cdabb8f25815b2bf47f56926431f6bfea55ea`.
The guard is enabled by persistent systemd drop-in
`/etc/systemd/system/sipfax.service.d/v90-erasure-guard.conf`; its presence in
the service process environment was verified. Application code remains
`1dc5cb2`, and the existing one-call cap remains in effect.

`/opt/sipfax/releases/native-9c493c3/` holds the previous binary, qualified
binary, and a manifest with both hashes. Local procedure
`work/rollback_v90_recovery.py` restores the previous native and removes the
persistent guard, requiring an idle server and matching expected hashes. It
has been prepared but not exercised as a separate release rollback campaign.
Earlier trial restoration and this post-deployment call are distinct evidence.

Post-deployment attempt `c7df5692-dbd3-48dc-8c87-f37251b530c7` passed at
49,296 bit/s with expected response checksum, PPP source, zero RAS errors,
`ipcp-open` and clean teardown. Final SSH verification confirmed the promoted
hash, persistent guard, and zero active resources. Evidence:
`work/v90-recovery-promotion{,-final-audit}.json`,
`work/v90-ppp-lifecycle-c7df5692-dbd3-48dc-8c87-f37251b530c7.json`.

Older local trial scripts that assert the pre-promotion hash must be updated
before reuse; do not blindly restore that older baseline after new trials.
This native-only deployment is not the final merged application release.

### Native rollback exercised

`work/v90_native_rollback_exercise.py` exercised the retained deployment assets:
rollback to the previous hash, hardware call, re-promotion of the qualified hash,
and another hardware call. Both passed at 49,296 bit/s with expected checksum,
PPP source, zero RAS errors, and clean teardown. Attempts:
`5b77d589-6bf5-49c5-9c3e-571cb9dff869` (previous) and
`99613f29-c07e-4042-97ea-c1ee0cd81859` (qualified).

Final independent SSH inspection confirmed qualified SHA-256 `e62a02b2...55ea`,
guard enabled in the running service environment, and zero sessions/leases/media
lines; the notebook had no connection. Evidence:
`work/v90-native-rollback-exercise-1789242351{,-audit}.json` and
`work/v90-native-rollback-final-state.json`. This supersedes the earlier note
that the native rollback procedure was only prepared. It covers the native-only
release, not a rollback of the final integrated application release.


## Integrated application installation and rollback passed (2026-09-12)

CT105 now runs application commit `9e0f242d40ed65fe2e4af315d0f9c94b45f852ce`
with the previously qualified native `9c493c3` binary, SHA-256
`e62a02b2f2868b096b61957b666cdabb8f25815b2bf47f56926431f6bfea55ea`.
The persistent erasure guard and existing lab configuration remain enabled.
The only changed application runtime file relative to production was
`src/session.js`, which releases an RTP allocation and stops a partially
constructed modem when modem/line construction throws.

The committed source bundle was transferred with SHA-256 verification:
`2260f26db664f86d60a62d5707a8700da710883a8b317a52641bbc378d13ebcc`.
An isolated CT105 build with GCC 14.2.0 succeeded; installer preflight and all
87 application tests passed with Node 24.20.0. The fresh native build hash was
`bb044a73346ed828fff5342e9dd5a9053725ef845b590229a73797e8d3ccfcd5`.
It was retained as unqualified, not substituted for the hardware-qualified
binary. Native source is unchanged between `9c493c3` and this application
commit; this is not a claim of bit-identical native build reproducibility.

The full `deploy/install-systemd.sh --engine=linmodem` procedure installed the
staged application with the qualified native artifact. A rollback restored all
25 installer-managed application, helper, hook, service, and native files from
a hash-verified archive; the same installer then reinstalled the candidate.
Each leg completed a real hardware call:

- Installed: `4a6119fa-174c-4502-bb38-7fa00cb0d04b`.
- Rolled back: `ecb3d296-916e-4730-b7fa-d96f98f38afe`.
- Reinstalled: `566b5d04-b8fc-4bbc-a4f8-c95b25a2ecd4`.

All three independently audited reports had a complete public HTTP response
with the expected 559-byte checksum, PPP source `10.64.0.2`, zero errors in
all six RAS counters before and after the probe, `ipcp-open`, and clean teardown.
Final SSH inspection verified every installed file against the release
manifest, the recovery guard in the service environment, and zero active
sessions/leases/media lines. The notebook also had no remaining connection.

Retained assets: `/opt/sipfax/releases/integrated-9e0f242/manifest.json` and
`previous.tar.gz` (SHA-256
`1e9b2e9c6b14d4159571b6b77ecfa7f63806b5ac52da2e65a1dc3925120e2dac`).
The staged source is `/tmp/sipfax-integrated-9e0f242`; local source bundle is
`/tmp/sipfax-integrated-9e0f242/`. Local procedures and evidence:
`work/integrated_release_remote.py`, `work/integrated_release_trial.py`,
`work/audit_integrated_release.py`,
`work/v90-integrated-release-1789242925{,-audit}.json`, per-leg logs/reports,
and `work/integrated-9e0f242-{manifest.json,app-tests.log,native-build.log}`.
The remote controller is staged at `/run/sipfax-integrated-release.py` and
supports `rollback` and `install`, requiring idle state and matching hashes.
Temporary staging paths are not durable release distribution.

This verifies full installer deployment and rollback in the current lab. It
does not establish the final merged release, sustained bulk POST upload,
working V.34 fallback, broader impairment tolerance, or simultaneous physical
calls. DialUpLab still reported 1.1.1.0 before this campaign. The exact source
commit's application CI passed; its native CI was still running at the last
observation, so a complete CI pass is not yet claimed.


## V.34 shell endpoint defect found and repaired (2026-09-12)

A broader B1 reference search exposed an encoder failure at 12000 bit/s,
3429 symbols/s, minimum shaping. Its K=16, M=4 mapping domain includes
index 65535, the all-(M-1) tuple. `index_to_rings` searched for a terminal
cumulative-count sentinel that `build_rings` does not allocate, so the final
shell read beyond the populated lookup table and could eventually produce
invalid constellation indices. The B1 generator aborted for all six tested
12000/minimum-shaping trellis/precoder combinations.

The shell search now stops at the highest valid shell. Its cumulative-count
comparison is unsigned, avoiding signed subtraction for thresholds above
INT_MAX; the table representation itself is unchanged. This is not a general
claim that every arithmetic operation in the full native modem is sanitized.

`tools/tests/v34-shell-boundary.py` compiles the production mapper functions
and exhausts M=1, M=2 and M=4 domains, checking range, uniqueness, inverse
mapping, nondecreasing shell energy, and independently known first/last tuples.
The pre-fix source failed the undefined/bounds sanitizer check; the fixed source
passes all 1 + 256 + 65536 indices. Local linking used the previously retained
UBSan runtime because the local system linker target is missing. CI runs this
test with its installed compiler runtime.

The B1 reference regression now covers every 4800..33600 rate in 2400-bit/s
steps, three trellises, two shaping modes, and zero/exact advertised precoder
coefficients: 156 configurations, each generated twice. All pass after the
fix, along with independent 588-bit B1 framing, the clean startup/incorrect-phase
control, and Figure 9 settled-decoding regressions. These remain offline tests;
startup errors in the unsynchronized Figure 9 cases are not counted as passes
for hardware startup.

The captured B1 search still found no convincing match after all 156 references
were generated: best held-out explained power 0.20705 versus 0.21334 for the
full unrelated-noise search; the constructed positive control scored 1.0.
The search spans rate/trellis/shaping/taps, either conjugation, 321 timing
positions, and coarse/fine carrier rotations under a three-tap linear model.
Its best-of-many score uses validation data for ranking, so it is exploratory
and does not prove that the notebook transmitted the wrong waveform. The
shell endpoint fix does not resolve the captured 16800-bit/s mismatch.

Evidence: `work/v34-b1-parameter-search-{before-audit,audit}.json`,
`work/v34-shell-boundary-{before,after,build}.log`, and
`work/v34-shell-{startup,fig9}-regression.log`. Production remains on the
qualified native binary; this source fix has not been deployed or hardware
qualified. The deployed application source `9e0f242` has now completed CI run
`34715252317` successfully in both application and native jobs, superseding
the previous pending-CI observation.


## Fixed shell candidate tested on hardware; V.34 still fails (2026-09-12)

Committed native source `fafcc3c` was built in `/tmp/v34-shell-fafcc3c` on
CT105. The source archive SHA-256 was
`7397e34e08c67f253bfff90f2793fdecbeaf04ed45ecd04f3f36c37468a2b9d5`;
the target binary SHA-256 was
`0833b8aeee1f925b675bd5f4b32d7cc12ea89a90eee2b024b154e54a79fac973`.
Target loader preflight, shell sanitizer, and expanded B1 generation checks
passed before either hardware trial.

Both calls disabled V.90 and requested V.34 CA/AC caps of 12000 bit/s with
minimum shaping. The first used the default legacy receiver; the comparison
added `SIPFAX_RX_CMA=1` and `SIPFAX_RX_DBG=1`, matching the earlier experimental
CMA setup. Debug mode is part of this experimental configuration and is not
claimed to be behavior-neutral.

- Default receiver: attempt `50d79b71-d4e3-4f93-b7ef-f44ed55fea17` failed with
  Windows error 678. The log shows repeated Phase 2/3 attempts and no MP/E
  data transition. This call did not exercise the repaired shell endpoint.
- CMA receiver: attempt `3d5a15b8-a5b9-4fe6-a7b3-4c76b38e73ba` also failed
  with error 678. It reached MP-prime, transmitted E/B1, and received E.
  Both TX and RX logged R=12000, K=16, M=4, L=16. The former encoder abort
  did not occur. Data acquisition reported lattice RMS 0.564 (the diagnostic
  labels roughly 0.577 as no lock), then the caller retrained. No PPP session
  was established, and no V.34 interoperability pass is claimed.

Each trial restored the qualified native binary and removed its temporary
systemd override. Each restoration was followed by a passing V.90 hardware
call at 49,296 bit/s: `ca89f202-137c-48dc-9c47-7c83140e7955` and
`399bf136-fe89-47f3-8455-7304092cb202`. An independent audit verified the
expected HTTP checksum/PPP source, zero RAS errors, clean teardown, all 25
production file hashes, enabled erasure guard, no trial override, and final
server/notebook idle state.

Procedures/evidence: `work/v34_shell_hardware_trial.py`,
`work/v34_shell_cma_hardware_trial.py`,
`work/v34-shell-hardware-1789243814.*`,
`work/v34-shell-cma-hardware-1789244020.*`,
`work/v34-shell-fafcc3c-target.log`, and
`work/v34-shell-hardware-final-audit.json`.

Raw RX/TX captures remain on CT105 under `/var/log/sipfax/`, with PID suffixes
58123 (legacy; 880640 bytes each) and 58408 (CMA; 303104 bytes each).
Automatic approval review rejected exporting these recordings to the local
workspace because they could contain sensitive communications. No alternate
export was attempted. Only remote-computed sizes/timestamps/SHA-256 metadata
was saved locally in `work/v34-shell-remote-audio-manifest.json`; further audio
analysis can run on CT105. This restriction does not block source development.

The remaining fallback investigation concerns legacy training and CMA data
acquisition; repairing the shell endpoint alone does not solve either. The
qualified application/native baseline remains installed, and DialUpLab still
reported version 1.1.1.0 before the trials.


## In-place V.34 negotiation audit and Table 11 ordering defect (2026-09-12)

Analysis of PID58408's recordings ran entirely on CT105. Reconstructed echo
cancellation matched the live delay 1428 and lock sample 75200. The transmitted
MP in the 14.5-second window decoded as 12000/12000, minimum shaping, initial
16-state trellis, ACK=0. The 15.0-second window decoded the final 64-state
request, ACK=1, with the same rate/shaping and exact h coefficients
`4413,1769,-3741,423,2586,-446` (Q14). This rules out an assumed final 16-state
request in that trial. Replaying its receive capture reproduced R=12000,
M=4, L=16, trellis64 and lattice RMS0.564.

NumPy 2.2.4 and its Debian dependencies were installed on CT105 for this
in-place analysis. No recordings or receiver symbol traces were exported.
The files `/tmp/v34-shell-58408-postecho.s16` and
`/tmp/v34-shell-58408-state.csv` remain on CT105. Metadata-only evidence:
`work/v34-shell-tx-negotiation{-fine,}-audit.json`,
`work/v34-shell-58408-postecho-audit.log`, and
`work/v34-shell-58408-stream-audit.json`.

A specification check found a separate concrete encoder ordering defect.
V.34 (02/98) Table11 steps4-6 and equation9-32 require
U0(m)=Y0(m) xor C0(m) xor V0(m), combining the current interval's modulo and
superframe bits before mapping its second 2D symbol. The current transmitter
instead saves the previous interval's C0/V0 in `s->U0`. The new diagnostic
`tools/audit-v34-table11.py` checks generated B1 traces against the equation,
using precoder c values before symbol mapping and the convolutional state
before advancement. For 60 4D intervals at 12000/minimum shaping it reports
2 violations with zero taps and 27 with the exact advertised taps above.
The diagnostic deliberately exits nonzero on this known defect; it is not
an enabled CI gate or a passing interoperability test.

An isolated prototype in `/tmp/v34-table11-prototype` computes c(2m+1) after
the first symbol, then combines current C0/V0 before the second-symbol rotation.
Both trace audits report zero violations. Prototype construction is retained
in `work/v34_table11_prototype.py` (source base `05033eb`), with
`work/v34-table11{,-prototype}-audit.json`. Its target native hash is
`6ef73bf8d6dd4aff70197dffdbd4df21a41e7b4797f09776ffe0d61142db8428`.
It has not been promoted into the source implementation or deployed.

The change is not ready for adoption. The existing clean-decoder oracle test
fails with 72587 bit errors using the prototype; changing the diagnostic
V0 offset to zero still gives73519. These failed controls are retained in
`work/v34-table11-prototype-{startup,current-sync}.log`. The receiver contains
additional assumptions about previous-interval C0/V0 and must be audited with
the transmitter. The test that previously passed does not independently prove
the old transmitter's standards compliance.

On-server reference searches over 16 rate/trellis/shaping/tap configurations
still show no convincing captured B1 match. Baseline best-fit explained power
is0.17767 (noise0.16105); prototype is0.17268 (noise0.18159); positive generated
controls score1.0. Best-of-many ranking is exploratory, not proof of a causal
hardware fix. Evidence: `work/v34-shell-58408-reference-audit.json`,
`work/v34-table11-58408-reference-audit.json`, and the corresponding scripts
and detailed results retained under `/tmp` on CT105.

Production continues to use the qualified native binary and integrated
application. The next source work is to reconcile transmitter/receiver
current-interval ordering against Table11/equation9-32 and independent clean
reference vectors before another hardware candidate is justified.


## Table 11 prototype clean decoding and shifted-frame recovery (2026-09-12)

The earlier prototype failures were localized further without hardware changes.
With corrected TX ordering and current-interval V0, the zero-cost branch matched
the transmitter's used U0 on all16000 generated 4D intervals, and the selected
source-state parity matched the transmitter on all16000. The remaining high bit
error count came from automatic frame realignment: it locked at phase0, then
applied the old `(19-phase) mod20` rule and discarded19 correctly aligned
symbols. Disabling that realignment in the controlled clean test yielded
0 errors over156486 bits; the deliberately shifted synchronization control
still failed. This ablation identified the fault, but was not adopted as the
solution because automatic alignment is required.

The isolated prototype now uses current-interval V0 in the diagnostic oracle
and aligns to phase0 using the actual mapping-bit-count cycle
`4*P/gcd(r,P)`, instead of a fixed20 with target19. With automatic alignment
still enabled, the clean oracle test passes0/156486; an incorrect sync phase
fails. Figure9 exact traceback and settled-decoding regressions pass at
7200/16800/33600, retaining their separately reported startup errors/erasures.

A diagnostic-only prefix-drop control exercised24 combinations: rates
12000/16800/21600/33600 and drops of0/2/6/10/18/38 input2D symbols. All cases
recovered a stable bit alignment and had zero errors across3035776 checked
settled bits in total. Every nonzero requested drop produced a positive matched
bit lag, so an ignored test parameter would not satisfy the audit. This test
uses no sync oracle, but deliberately preserves4D pairing. It does not test
odd-symbol pairing recovery, channel noise, precoding, audio or PPP. The fixed
settled region starts at output bit40000; lag is selected with a2048-bit window
and verified over all subsequent saved bits.

The reproducible, unadopted patch is
`research/v34-rx/table11-prototype.patch` against native source at`05033eb`.
It includes the diagnostic prefix-drop control. Applying it directly to the
qualified deployment is not the tested procedure. Current prototype source
SHA256: `51da05a0538836257da7fa26e9bb6731110ab4b8c0d14e2d5e851af6e50cb6d4`;
local native SHA256: `c0f2e2e0bb388e0b92dc19b90d5123667ced1de86eb537cd1e6e18e95bfc2061`.
Local evidence: `work/v34-table11-branch-evidence/audit.json`,
`work/v34-table11-alignment-{startup,fig9}.log`,
`work/v34-table11-frame-shift-audit.json`,
`work/v34-table11-alignment-prototype-manifest.json`,
and `work/v34_table11_frame_shift_check.py`.

The source default remains unchanged. Precoded receive processing still uses
previous-interval C0 in several places and needs corresponding standards-based
validation before this prototype becomes a hardware candidate. The previous
on-server captured B1 mismatch also remains unresolved; clean unprecoded
symbol recovery is not a replacement for that hardware requirement.


## Table 11 and precoded receive corrections integrated in source (2026-09-12)

The Table11 transmitter and rate-aware alignment corrections have now moved
from the isolated prototype into `vendor/linmodem/v34.c`, with two additional
precoded-receive fixes. They have not been deployed to CT105.

The normal trellis input after a matching channel is Y=u+c. Its constraint is
Y0 xor V0; adding the experimental front-chain C0 again is incorrect. With the
corrected transmitter and a matching synthetic FIR channel, disabling that
extra bit gave exact decoding at12000/16800 for both a single real tap and the
captured call's three complex taps. Enabling it produced roughly half wrong
bits. `SIPFAX_FC` therefore defaults to0; value1 remains an explicit legacy
experiment, not a qualified receive mode.

At33600, a second defect clipped Y to the unprecoded constellation's radius45
before removing c. The retained trace shows the first failing symbol's correct
Y=(51,-13) becoming(45,-13); inverse output u=(35,-13) became(29,-13), then
subsequent precoder history diverged. Precoded Y now retains the transmitter's
coordinate range through255 until c is removed. The first400 traced symbols
then matched exactly, and both complete33600 cases decoded with zero errors.

New CI gates run `tools/audit-v34-table11.py` and
`tools/tests/v34-table11-link.py`. The integrated native build passes:

- Equation9-32 trace checks with zero and exact nonzero coefficients.
- Twelve exact precoded clean-channel cases: rates12000/16800/33600, both
  shaping modes, and two tap sets, with specification-derived synchronization.
- Negative controls for the extra-C0 constraint and a missing inverse. A missing
  inverse can suppress invalid frames rather than emit wrong bits; both are
  correctly treated as decoding failures by the control.
- Forty-eight automatic-alignment cases, with and without precoding, four rates
  and six even2D prefix drops: zero errors across6064690 settled bits. No sync
  oracle is used for these cases. Nonzero drops must produce a positive matched
  lag, guarding against an ignored test option.
- Existing known-phase startup test (0/156486; wrong-phase control fails),
  Figure9 exact-state/settled-bit checks,156-configuration B1 generation, and
  independent B1 scrambler/framing checks. Figure9 startup errors/erasures remain
  separately reported and are not hardware startup passes.

Before-fix evidence: `work/v34-table11-precode-before-range-audit.json` and
`work/v34-precode-range-before-audit.json`. After-fix evidence:
`work/v34-table11-precode-audit.json`,
`work/v34-precode-range-evidence/audit.json`, and
`work/v34-table11-integrated-{audit.json,link.log,startup.log,fig9.log,b1-reference.log,b1-framing.log}`.

These checks use generated symbols and ideal matching linear channels. They do
not qualify nonlinear encoding, odd2D pairing recovery, analog timing/noise,
the captured hardware acquisition problem, or V.34 PPP. The receive inverse
still requires deliberate enabling and a correctly prepared Y signal. The
alternative pre-trellis inverse remains experimental; it was not validated by
these post-traceback inverse tests. The qualified deployed V.90 baseline is
unchanged, and complete native CI/hardware qualification of this source change
remains pending.


## Low-rate B1 determinism fix and CT105 validation (2026-09-12)

CI run34717920914 at `5d73032` passed the application job but failed the
B1 reproducibility gate. Investigation found that low-rate mapping read a
bit beyond the short frame and flattened the I-bit array in an order that
did not follow clause9.3.2. Commit `411f88b` groups I1/I2 for every4D symbol
and includes I3 only in the first mp_size-8 groups, with matching receive
packing. A new regression checks emitted4800-bit/s B1 rotations against
independently derived GPC bits and verifies clean data roundtrips for both
shaping modes. The rotation check failed before the fix.

The exact `411f88b` source was built separately on CT105 at
`/tmp/v34-table11-411f88b`; it was not installed into the running service.
Source archive SHA256:
`1a56a530ed638f7547fc258f4c208069eecf61753e0046c7b975c7017451d58a`.
Native binary SHA256:
`5148293e0faa98fead8328a05307ea5d3ca5472a5b83a400abe48bf844a535ad`.
Loader preflight, low-rate framing,156-configuration deterministic B1,
Table11 trace audit,12 exact precoded cases and their negative controls,
48 alignment cases (0/6064690 settled bits), and known-phase startup
(0/156486; wrong-phase control fails) passed on CT105.
Local target evidence: `work/v34-table11-411f88b-target-validation.log`.

These are offline checks on the target host. Full CI and physical V.34
acquisition/PPP remain required. Production remains application `9e0f242`
with the qualified `9c493c3` native binary and persistent erasure guard.


## Corrected Table 11 hardware trial and restoration (2026-09-12)

Exact CT105 candidate `411f88b` (native SHA256 `5148293e0faa98fead8328a05307ea5d3ca5472a5b83a400abe48bf844a535ad`)
was tried with the previous V.34-only 12000-bit/s caps, minimum shaping,
CMA receiver and RX debug settings. Attempt
`a13ea357-dcb9-47ea-ae55-fdfb748242d1` failed with XP error678. MP-prime,
E and B1 were reached. The receive configuration was12000 bit/s,64-state
with K16/M4/L16. Lattice RMS remained0.564, near the diagnostic no-lock
reference0.577. This is not successful data acquisition or PPP.
The caller requested nonlinear encoding in this attempt; the current
clean-channel regression explicitly excludes that path. This observation
does not establish that nonlinear encoding caused the failure.

The exact qualified native was restored and the temporary trial settings
removed. Restoration call `1cec0a46-a1fc-49f7-94ec-59537b59d036` passed at
49296 bit/s, with a559-byte HTTP response matching the expected SHA256,
PPP source10.64.0.2, all six RAS error counters zero, ipcp-open lifecycle,
and complete cleanup. An independent final audit verified all25 managed
release files, the persistent erasure guard, and idle notebook/server state.
Production remains application `9e0f242` and native `9c493c3`.

Local evidence: `work/v34-table11-cma-hardware-1789246456.{json,call.log,native.log}`,
`work/v90-ppp-lifecycle-1cec0a46-a1fc-49f7-94ec-59537b59d036.json`, and
`work/v34-table11-hardware-final-audit.json`. Raw call audio remains on CT105.
CI run34718354507 at `39a21df` has passed the previously failing B1 gate,
low-rate framing, Table11/precoded-link and startup checks; the complete
native job was still running when this hardware record was written.


## Independent nonlinear transmit projection check (2026-09-12)

The latest caller requested nonlinear encoding. An audit against
[ITU-T V.34 (02/98), clause9.7](https://www.itu.int/rec/dologin_pub.asp?id=T-REC-V.34-199802-I%21%21PDF-E&lang=e&type=items)
found the current projection formula consistent with equations9-33 through
9-35. The B1 diagnostic now accepts `SIPFAX_B1_NL_MEAN`: a positive finite
reference energy in lattice units. It enables the existing encoder branch
without changing live modem defaults or substituting a new formula.

`tools/tests/v34-nonlinear.py` first obtains linear B1 output, independently
measures its energy, then compares actual nonlinear output against Decimal
calculations using two supplied reference energies. All3840 symbols passed:
rates4800/12000/16800/33600, both shaping modes, zero and nonzero precoder
taps, and two normalisers. The comparison also checks that nonlinear output
does not feed back into and change the precoder's history. Ignored-option
and constant-gain controls differ from the expected result; the latter is
applicable only to variable-energy constellations. Invalid mean values are
rejected. The unchanged B1-reference and Table11 trace checks also pass.
The new test is included in CI.

Evidence: `work/v34-nonlinear-projection.log`,
`work/v34-nonlinear-b1-regression.log`, and
`work/v34-nonlinear-table11-regression.json`.
This validates projection for supplied energy and generated symbols, not the
live power estimate, nonlinear receive inverse, analog waveform acquisition,
or hardware interoperability. It does not resolve the latest error678 or
establish its cause. The retained production release was not changed.


## Latest failed call: in-place negotiation and reference audit (2026-09-12)

PID59456 is the `411f88b` call `a13ea357-dcb9-47ea-ae55-fdfb748242d1`.
Its315392-byte RX/TX recordings remain on CT105. Echo replay reproduced
live delay1426 and lock sample80800. Receive replay again reached E with
12000-bit/s,64-state,K16/M4/L16 parameters and failed acquisition
(lattice RMS0.562 in replay,0.564 live).

Decoding the transmitted audio independently confirmed the advertised
12000/12000 rates and minimum shaping. ACK=0 windows initially requested
16-state trellis; the final ACK=1 window at16.0 seconds requested64-state.
All decoded MP windows used nonlinear=0 for the remote transmitter and
hQ14=`4415,2209,-3795,190,2700,-614`. The caller's separate nonlinear
request applies to our transmitter, not to the caller's upstream signal.

A16-configuration reference search used the corrected `411f88b` encoder,
exact advertised or zero taps,12000/16800 rates,16/64-state trellis and both
shaping modes. It varied nearby alignment, conjugation, and carrier rotation,
with a3-tap complex fit. The best captured score was0.18136 explained power;
the best unrelated-noise score was0.18057. A generated positive control scored
1.0 at its known alignment. Thus the search did not find convincing B1
agreement. Ranking uses held-out samples, so these best-of-many scores are
exploratory, not statistical significance or a decoded-bit acceptance test.
This preserves evidence against assuming that the symbol-level corrections
already fixed the physical receiver.

Metadata and aggregate results only were returned locally:
`work/v34-table11-59456-stream-audit.json`,
`work/v34-table11-59456-negotiation-audit.json`, and
`work/v34-table11-59456-reference-audit.json`.
The replay waveform, receiver trace, and search script remain on CT105 under
`/tmp/v34-table11-59456-*`. No service, modem or network settings changed.


## Live V.34 data descrambler role corrected (2026-09-12)

Generated audio exposed a role contract mismatch. Live receive initialization
passes the transmitting peer's role to select its carrier and training path,
but `put_bit` interpreted that field as the local role and selected the other
polynomial. Clause7 requires GPC for caller transmissions and GPA for answer
transmissions. The new peer initializer records the data polynomial explicitly;
both live initialization and the Phase2 handoff use it. Legacy local-role
symbol diagnostics retain their original selection semantics.

A clean-symbol reproduction using specification-derived synchronization had
0/111776 errors with the legacy local-role initializer but56194/111776 with
the previous live peer-role initializer. The same comparison now has zero
errors in both modes. The regression covers both transmission directions,
12000/16800, both shaping modes and both initializer contracts:2146096 payload
bits with zero errors. Transmitted symbol fingerprints must differ by role,
and the peer initializer reports its selected polynomial to confirm the test
exercises it. Existing precoded-link/alignment regressions also pass.

A16-second generated12000-bit/s waveform through the actual native modulator
and CMA receiver previously had46774 wrong bits among93681 bits after the
fixed40000-bit settling boundary. With only the role correction, the same
recorded generated waveform has0/93681 settled errors. It still has2988 errors
within the initial40000 emitted bits. `tools/tests/v34-generated-audio.py`
reproduces this result and is included in CI alongside the role regression.
The supplied4.2-second data-entry time deliberately bypasses automatic E
recognition; the unforced generated-audio replay produced no data bits.
Thus this is a real audio data-path regression, not automatic startup or
hardware fallback qualification. It does not establish that the captured
caller acquisition failure is fixed.

Evidence: `work/v34-peer-role-{before,after}-oracle.json`,
`work/v34-peer-role-regression.log`,
`work/v34-peer-role-table11-regression.log`,
`work/v34-peer-role-audio-regression.log`, and
`work/v34-generated-audio-6d31989/` (generated content only).
Production remains unchanged. CI run34718354507 at `39a21df` has now completed
with both jobs successful; that result predates this new role correction.


## Captured receive contrasts and clockwise mapper correction (2026-09-12)

Replaying PID59456 with the CT105-built `5bc4919` descrambler correction
left acquisition unchanged: lattice RMS0.562 and mean trellis metric241.7.
The receiver emitted7867 bits in both runs, but the observed ones fraction
remained near50 percent. Three existing receive configurations were tested
on the same retained recording: CMA plus post-traceback inverse, a delta
equalizer plus inverse, and the seeded block equalizer plus inverse.
All activated the expected options and loaded the exact advertised h.
None acquired a convincing signal; lattice RMS ranged0.562..0.566.
These contrasts do not isolate the remaining cause. Aggregate evidence:
`work/v34-peer-59456-replay-audit.json` and
`work/v34-peer-59456-equalizer-audit.json`. Audio remained on CT105.

A separate specification audit found that data mapping violated9.6.1:
Z rotations must be clockwise. The legacy `rotate_clockwise` helper actually
rotates counterclockwise for positive arguments. Phase4 already negated its
argument, while the data mapper and its inverse lookup still agreed on the
wrong direction. A mutually consistent loopback did not detect this error.

`tools/audit-v34-mapper.py` independently checks the emitted u-coordinate
residues against clockwise rotations of the Figure5 quarter-constellation.
The old mapper violated59/120 checks with zero precoder taps and54/120 with
the captured advertised taps. A prototype negated only the data-mapper
rotation and converted the receive lookup's CCW index to clockwise Z.
Both audits then had0/120 violations. The same correction is now integrated
in source, with the independent audit enabled in CI. The generic helper and
Phase4 rotation behavior are unchanged.

The prototype passed12 exact precoded cases and their negative controls,
48 alignment cases (0/6065920 settled bits), both peer-role contracts
(0/2146096 bits), and generated audio (0/93760 settled bits,3035 startup
errors retained). Integrated-source checks also passed known-phase startup
(0/156486, wrong-phase control fails), Figure9 state/settled decoding,
nonlinear projection, low-rate grouping and deterministic B1 generation.
Automatic startup and hardware interoperability remain unqualified.

The isolated CT105 prototype source SHA256 is
`2acd3c8f8d9bdb55b1f17f47445f4cd84286f0938f066778c3b83c53f4dba11b`;
native SHA256 is
`8e70d64839fb021813a78538b8a3fdce6d2d300d82958bef0f53fc5b008a2478`.
The corrected references still did not convincingly fit PID59456's received
symbols: best capture0.18160 versus unrelated-noise0.20677, positive control1.0.
This is another retained hardware-analysis failure, not a fallback pass.

Evidence: `work/v34-mapper-clockwise-{before,prototype}.json`,
`work/v34-clockwise-prototype-link.log`,
`work/v34-clockwise-59456-reference-audit.json`,
`work/v34-clockwise-target-build.json`, and
`work/v34-clockwise-integrated-*`. The prototype lives under
`/tmp/v34-clockwise-5bc4919` locally and on CT105. Production was not changed.


## Clockwise candidate target build, hardware failure, and restoration (2026-09-12)

Exact source `f1e4592` was built on CT105 under
`/tmp/v34-clockwise-f1e4592`. Source archive SHA256:
`3e82b92845f4dcced4fc429865e54e6d1acc6de1e8f9e78a96ab09465ca392c0`.
Native SHA256:
`b47e16ffadb877a81221b2cb5c9d98746784640f62e2b08ed267efe4e70fa6bd`.
Loader preflight, clockwise mapper audit, peer-role, generated-audio,
precoded-link/alignment, nonlinear, and low-rate checks passed on CT105.
Evidence: `work/v34-clockwise-f1e4592-target-validation.log`.

One bounded hardware attempt used the same V.34-only12000-bit/s caps,
minimum shaping, CMA receiver and RX-debug settings as the previous trial.
Attempt `eda444e9-de1b-4c03-95d2-adbb9d914ad5` failed with XP error678.
The native log includes repeated B1 entries and receive lattice RMS0.547
and0.567; these are not usable acquisition or PPP. The result is retained
in `work/v34-clockwise-cma-hardware-1789248463.{json,call.log,native.log}`.

The qualified native and original settings were restored. Verification call
`7d9e2491-e5d5-4e15-b733-b33059269539` connected at49296 bit/s and passed
the559-byte HTTP checksum with PPP source10.64.0.2, all six RAS counters
zero, ipcp-open lifecycle and complete cleanup. The independent audit verified
all25 managed release files, the persistent guard, removal of trial settings,
and idle notebook/server state. Evidence:
`work/v34-clockwise-hardware-final-audit.json` and
`work/v90-ppp-lifecycle-7d9e2491-e5d5-4e15-b733-b33059269539.json`.
Production remains application `9e0f242` and native `9c493c3`.

Full CI run34719420312 at the preceding role-fix commit `5bc4919` has now
completed successfully. The newer clockwise mapper still requires its own
complete CI result; the failed hardware fallback gate remains open.


## MP numeric field bit-order correction (2026-09-12)

Earlier V.34 MP rate and trellis reports in this log used the same incorrect
MSB-first decoder as the transmitter. They must not be treated as independent
proof of the on-wire numeric settings. Tables20/21 label these fields LSB:MSB.
The transmitter wrote them MSB-first, and both receive parsers read them that
way. For example, the test sender's requested rate code5 appeared on wire as10,
and trellis code2 appeared as1. This can make software and peer use different
rates and trellis codes while a self-loopback reports agreement.

The MP writer now emits rate and trellis fields LSB-first, and both the block
and fold parsers decode that order. The general bit writer and CRC serialization
are not reversed: `calc_crc` already reverses the register bits for that writer.
The independently checked mask and coefficient field handling is retained.

`tools/tests/v34-mp-fields.py` reads emitted frame bits independently, then
constructs its own Type0 and Type1 frames, CRC16 bits, GPC scrambling, and
clockwise differential QPSK symbols. The native block decoder must recover all
rate codes1..14 and trellis codes0..2, with asymmetric rates and both frame
formats. All84 cases pass after correction; corrupted frames are rejected.
Before correction, one generated frame requesting codes1/14 decoded as8/7,
and the independent emitted-bit audit also failed. Existing generated-audio
(0/93760 settled bits) and peer-role tests (0/2146096 bits) still pass.

Evidence: `work/v34-mp-field-order-before.json`,
`work/v34-mp-field-rx-before.log`, `work/v34-mp-field-fixed.log`,
`work/v34-mp-fields-audio.log`, and `work/v34-mp-fields-roles.log`.
This is not hardware qualification. The retained recordings need re-decoding
with the corrected parser. Two additional negotiation concerns remain open:
MP information changes during an exchange, and the receive trellis fallback
treats the valid code0 as absent. The peer's shaping request also needs tracing
through to the transmit configuration. Production has not changed.


### Corrected retained MP interpretation and zero-code receiver selection

The exact 0b0a159 target build passed the independent MP, peer-role, and
generated-audio tests on CT105 without deployment. Its corrected block decoder
read retained TX capture 59456 as 24000/24000, initially trellis code0 and then
code1 in MP-prime. Thus the previous 12000/64-state interpretation was wrong;
old reference searches at those parameters cannot exclude valid data at the
actual advertised parameters. Evidence: work/v34-mp-0b0a159-target-audit.log.
The recording stays on CT105.

The live receiver now uses the presence of an advertised rate to distinguish
an unset advertisement from trellis code0 (valid 16-state mode). Once an
advertisement exists, its trellis selection takes precedence over the peer's
request for our transmitter. Without an advertisement, the existing peer
fallback remains. The MP test exercises all nine advertised/peer combinations
and three cases without an advertisement, including disagreeing directions.
All 12 selections and 84 independent MP frame cases pass. Peer-role payloads
retain zero errors across 2146096 bits; generated audio retains zero errors
across 93760 settled bits, with 3035 startup errors still reported.

This fixes selection only. Stable MP information throughout the exchange,
peer shaping propagation, and hardware fallback qualification remain open.
Production is unchanged.


### Directional shaping propagation

Both live MP decoder paths now retain the peer's shaping request, and the
RX-to-TX bridge carries it into transmit data parameters. The data constellation
is rebuilt even when the negotiated rate equals the initial rate. Power
normalisation receives the actual transmit shaping configuration explicitly;
it no longer reads the global SIPFAX_SHAPE setting independently. Our MP's
shaping bit is stored and bridged to the receiver, where it overrides the
initial/global shaping choice when an advertisement exists. The global setting
remains available for generated signals and unnegotiated diagnostic input.

The MP test now checks both directions independently at 12000 and 24000 bit/s,
including conflicting global settings and unchanged transmit rates. Sixteen
cases match the separately initialised reference constellations. Estimated
power varies with transmit shaping and is invariant to receive/global shaping.
All 84 MP cases and 12 trellis cases still pass; peer-role tests retain zero
errors over 2146096 bits and generated audio retains zero over 93760 settled
bits (3035 startup errors retained). These are local generated tests, not a
hardware call result. Stable MP exchange parameters and V.34 startup remain
open, and production has not changed.


### Freeze MP information within one startup exchange

The first MP now snapshots the complete information field, format, and advertised
precoder coefficients. Later MP and MP-prime reuse it; only ACK and the resulting
CRC change. This prevents newly decoded peer requests or updated channel estimates
from changing an exchange already in progress. V34_init_low invalidates the
snapshot for a new negotiation. The receive bridge carries the frozen coefficient
values, and live receive rate selection uses the actual advertised cap. Offline
recording diagnostics retain their environment/global fallback when no advertisement
exists.

Independent raw-frame checks cover both MP formats, parameter/channel changes after
the first MP, ACK transitions with recalculated CRC, and a fresh negotiation after
initialisation. All pass, alongside 84 independent MP frames, 12 trellis selections,
16 directional shaping/power cases, peer-role payload checks and generated audio.
This does not establish hardware fallback. The corrected negotiation must still
be built on CT105 and exercised with the notebook; production is unchanged.

The preceding directional-shaping commit 9f46376 also passed the exact target build
and loader check on CT105: native SHA256
e701fc13c533c38d68529a391b2a31e796a29c80d51b34d669990cd7ecd918d0.
Evidence: work/v34-shaping-9f46376-target-validation.log.


### Corrected negotiation hardware trial 315e304

Exact CT105 build passed the MP, trellis, shaping, snapshot/reset, peer-role and
generated-audio gates. Native SHA256:
ac57c9954d1a5c223e0ffe63817ea8fde8949a153aebf845d9910558044bedce.
Evidence: work/v34-stable-315e304-target-validation.log.

V.34-only attempt 07bb1db8-1646-417c-88db-bb02816f9e88 still failed with Windows
error678. The caller's CRC-valid MP-prime requested ca33600/ac31200 and trellis
code1. The server applied the intended negotiated 12000 rate in each direction,
32-state TX trellis with the peer's expanded shaping (L20), and its own advertised
16-state/minimum-shaping RX (L16). The first MP froze zero precoder coefficients.
One TX E-to-B1 event occurred; no watchdog restart was logged. Acquisition lattice
RMS was0.232 (previous trials approximately0.56), clock seed+4.1ppm. However early
descrambled bits were only59.2% ones at600 and58.8% at2400, so correct B1/payload
decoding remains unproven. This is an improved diagnostic metric, not V.34 success.
The next analysis should inspect acquisition, timing and mapping-frame alignment
with this corrected negotiation. Raw audio and full receiver traces remain on CT105.

Qualified native e62a02b2f2868b096b61957b666cdabb8f25815b2bf47f56926431f6bfea55ea
was restored, with the temporary drop-in removed and erasure guard enabled.
Restoration call117e57f3-c860-4133-a7ce-c489b300e8f2 passed at49296 bit/s: PPP
ipcp-open,559-byte HTTP probe SHA256
ff67a9d764d6a2367a187734e697f6a53217db9a21c101d410a113ca871a299d,
source10.64.0.2, all six RAS error counters zero before/after, and clean disconnect.
All25 installer-managed files matched the qualified release and the notebook/server
were idle afterward.

Evidence: work/v34-stable-cma-hardware-1789250239.{json,call.log,native-audit.json},
work/v34-stable-hardware-diagnostics.json, work/v34-stable-hardware-final-audit.json,
work/v34-stable-restoration-call.log and
work/v90-ppp-lifecycle-117e57f3-c860-4133-a7ce-c489b300e8f2.json.
DialUpLab still reports1.1.1.0; actual bulk upload and multiple physical calls
remain unqualified. PR29 remains draft.


### Recorded advertisement replay and B1 match, capture61604

Offline replay previously lacked the live TX-to-RX advertisement bridge and could
select the peer's32-state request instead of the recorded local16-state request.
Commit deff8a7 adds SIPFAX_STREAM_MP=rate,trellis,shape to supply recorded receive
parameters explicitly. Generated audio with a contradictory peer trellis but the
correct recorded advertisement yields identical decoded bits to the baseline;
malformed/out-of-range inputs are rejected. These tests pass locally and on CT105.
No live modem behavior was changed by this diagnostic addition.

Retained capture61604 (RX SHA256
61f14aebaadea5bef1b2d715e9e5797fdd6b4ec7b15b4169fca19a4075e1b3a4)
was echo-cancelled and replayed entirely on CT105. The recorded12000/16-state/
minimum-shaping advertisement produced lattice RMS0.235 versus0.232 live, but only
56.5% ones over the first2400 decoded bits. Timing offsets minus/plus0.25 input
samples worsened RMS to0.279/0.290 and did not produce valid startup decoding.
Evidence: work/v34-recorded-61604-replay-audit.log. Target diagnostic binary SHA256
d40d7fc095c9c73d0dd217bb0a91c5fe1842987c00375edd7f3eff16f308387b.

A B1 reference comparison now shows a material match: recorded12000/minimum/zero-h
with a32-state reference explains0.7732 of held-out symbol power; the16-state
reference explains0.6412. Both best fits have zero symbol offset, zero carrier
rotation, and no conjugation. Unrelated noise scores approximately0.13; the
constructed positive control scores1.0. Four reference candidates cover16/32-state
and minimum/expanded shaping. The search fits three complex linear taps on
symbols2..79 and ranks symbols80..119; best-of-many ranking remains exploratory.
It does not establish that the caller ignored our trellis request or that either
reference encoder is independently correct. Next isolate the trellis-dependent
symbol differences and residual channel error before another hardware change.
Evidence: work/v34-recorded-61604-reference-audit.log. Audio and receiver traces
remain on CT105; production is unchanged and V.34 PPP remains unqualified.


### Isolate Figure 9 mapping mismatch in capture61604

Additional in-place discrimination compared all16/32/64-state references at the
same zero offset and carrier rotation, with1/3/7 complex channel taps. Generated
positive controls correctly identified each true trellis; noise explained at most
0.0343 in the legacy fixed-offset comparison. A common-channel fit on reference-
shared symbols showed all52 first symbols rounded exactly to the expected points,
but only28/52 second symbols matched16-state or32-state and26/52 matched64-state.
This localizes the discrepancy to the second-symbol redundant-bit mapping rather
than general timing or channel acquisition.

Generating references with the existing SIPFAX_FIG9=1 branch resolves the apparent
32-state preference: the advertised16-state reference explains0.99788 with one
complex gain and0.99867 with three taps, versus0.79444/0.78768 for32-state. Thus the
legacy reference mapping caused the earlier misleading trellis ranking; this call
now strongly agrees with the advertised16-state B1 sequence. This remains reference
comparison, not a successful payload decoder.

Full audio replay with SIPFAX_FIG9=1 still returns the same incorrect bit stream.
Inspection shows that the corrected-label receive branch table is selected only in
the64-state switch case. The16-state trellis_trans_4 and32-state trellis_trans_8
paths remain legacy regardless of that flag. Next extend the corrected branch-table
construction to these modes, independently verify membership and state traceback,
then replay before another hardware trial. Do not change production based only on
the reference correlation.

Evidence: work/v34-trellis-61604-discrimination.json,
work/v34-b1-61604-parity.json, work/v34-fig9-61604-discrimination.json and
work/v34-fig9-61604-replay.json. All captured audio and receiver traces remain on
CT105. No live service changes were made.


### Complete Figure 9 tables and controlled hardware trial d675cea

The branch generator now supports4/8/16 transitions and supplies corrected tables
for16/32/64-state receivers under SIPFAX_FIG9=1. All768 tuples are independently
classified using the fixed Figure9 grid; the same generator reproduces every
legacy table when given legacy labels. The dataloop harness accepts explicit
trellis selection. Nine clean-data cases (7200/16800/33600, allthree trellises)
have exact traceback states and no settled bit errors, retaining startup errors
and erasures in the reports. The qualified default remains unchanged.

Exact CT105 build d675cea, SHA256
05bdc59abc0903333db53bd13f2d8ad83f75812d8f7e6ecac44b264a6263478d,
passed table/state tests. Capture61604 replay with the corrected16-state table
improved first2400-bit ones fraction from56.5% to70.1% and mean trellis metric
from227.5 to208.4; overall61.5% ones over7568 bits. This is still not correct
payload/startup decoding. Evidence: work/v34-fig9-d675cea-target-replay.log.

Controlled Figure9-enabled hardware attempt a3429cd2-c75e-48c4-a46a-304105e9a961
failed with678 after four TX B1 entries. Two RX acquisition events had RMS0.546/
0.545 and clock seeds-291.4/+322.9ppm, unlike the earlier good capture. Preserve
this failure; the table correction alone does not provide repeatable acquisition.
Capture62290 remains on CT105 (880640-byte RX); full trace is
/tmp/v34-fig9-d675cea-hardware-native.log. Aggregate evidence:
work/v34-fig9-cma-hardware-1789251146.{json,call.log,native-audit.json} and
work/v34-fig9-hardware-diagnostics.json.

Restored qualified V.90 call b7c2a184-0b86-4a8e-ace4-425f0a173b93 passed at49296,
with the expected559-byte checksum, zero six-category RAS errors, ipcp-open and
clean cleanup. All25 release-managed files match, erasure guard is enabled, the
Figure9 trial flag is absent, and both endpoints are idle. Evidence:
work/v34-fig9-restoration-call.log, work/v34-fig9-hardware-final-audit.json and
work/v90-ppp-lifecycle-b7c2a184-0b86-4a8e-ace4-425f0a173b93.json.
V.34 fallback, actual bulk upload, and multiple physical calls remain unqualified.


### Acquisition discards B1 before bit decoding

Trace indices on capture61604 with corrected Figure9 tables show E at row29910
and the first decoder-fed symbol at31911:2001 symbols later, with2000 collected
for acquisition (about584ms). B1 at these parameters is120 symbols. The collected
acquisition samples are used for gain/phase fitting but are not replayed through
the bit decoder. Consequently previous first600/2400 emitted-bit ones percentages
are not B1 BER and must not be presented as such. They refer to later symbols,
after the entire known B1 sequence was skipped.

Existing SIPFAX_ACQ_N contrasts on the same retained call:
64 gives acquisition RMS0.153, first600/2400 emitted-bit ones92.3%/80.7%,
mean trellis metric160.1;120 gives0.215,96.5%/78.9%,161.2;400 gives0.123,
67.5%/70.0%,166.8. The2000-symbol baseline gives0.235,69.0%/70.1%,208.4.
These are receiver diagnostics, not measured BER or PPP qualification. In
particular a low acquisition RMS does not by itself prove correct alignment.
Evidence: work/v34-acq-window-61604-replay.json. Raw traces remain on CT105.

Next preserve the buffered startup symbols through acquisition and pass them to
the decoder in order, with matching gain/phase, rather than only shortening the
window and still dropping B1. Validate recovered known B1 bit positions on generated
input and the retained recording before hardware use. Also verify tracking after
acquisition and repeatability on the separate failed capture62290. Production is
unchanged by these offline experiments.


### Experimental acquisition-buffer handoff 0572a93

SIPFAX_ACQ_REPLAY=1 preserves raw equalizer samples and their SRX phase separately
from the acquisition workspace. After acceptance it processes the saved prefix
in order and then the current sample. Sequence tests caught and corrected an
initial off-by-one: acquisition runs on the sample after the buffer fills, so
that current sample must follow the buffer. The default remains off. This adds
per-instance storage and work to the acquisition callback; callback timing still
requires qualification before hardware use.

New CI test v34-acq-replay.py checks64/2000-symbol windows, explicit source-symbol
indices, no duplicate/out-of-order feed, complete accepted-prefix/current-sample
continuity, and zero settled generated-audio errors. Both pass locally and on
CT105. Baseline generated-audio and MP tests also pass. This test supplies data
entry time and does not claim automatic E or hardware B1/PPP qualification.

Exact target native SHA256:
db9f227e83928c4b7f4a2deab6e93a3c93f2320c2b8002a3d70d7a7e70a60d24.
Capture61604 replay with Figure9 and recorded MP now feeds8215 symbols for either
window. With64-symbol acquisition, first600/2400 emitted-bit ones are98.5%/84.2%,
mean metric158.5,14550 decoded bits. With2000 they are98.5%/83.8%,158.5,14601 bits.
The equal feed counts demonstrate retained samples; differing emitted bit counts
still reflect decoder alignment/erasure behavior. Ones percentages are not BER.
Next audit exact B1 output bit positions, tracking after acquisition and callback
latency; repeat on capture62290 before hardware use.

Evidence: work/v34-acq-0572a93-target-replay.log. Audio and complete receiver traces
remain on CT105. Production is unchanged; the option is not enabled there.


### Startup positions and callback deadlines

The retained61604 replay now feeds source symbols0..119 contiguously and emits
all bit positions0..419 without suppression for both64/2000 acquisition windows.
Each candidate B1 interval contains9 zero bits, all within positions0..55; the
remaining364 bits are ones. Both runs lock V0 phase420 after931 symbols and request
zero4D-symbol alignment drops. This narrows the initial-state/quadrant question,
but exact association of emitted frame zero with B1 still requires verification;
do not label these counts a fully established hardware BER measurement.
Evidence: work/v34-b1-61604-position-audit.json.

Commit78e0ecc adds opt-in SIPFAX_STREAM_TIMING instrumentation around each actual
V34_demod_cma call. The generated-audio sequence test verifies all800 callbacks at
160 samples, coherent timing fields, and unchanged settled correctness. Exact
CT105 build SHA256:
c781eb9294dff218c244a967960049756d691c7fccbf7bcbfc1c0a754ca3407b.
On retained call61604 with buffered handoff and receiver diagnostics enabled:
64-symbol acquisition:948 callbacks, mean0.287ms, peak17.861ms, zero20ms overruns;
2000-symbol acquisition:mean0.846ms, peak547.488ms, one overrun. State CSV export
was disabled during measurement, but diagnostic logging remained enabled.
Evidence: work/v34-callback-78e0ecc-target-replay.log.

The2000-symbol path is not suitable for live deployment as measured. The64-symbol
single-run result has limited headroom and is not a real-time qualification.
Next remove/gate purely diagnostic exhaustive acquisition scans, distinguish scan
cost from buffered decoder cost, repeat callback measurements, and verify initial
B1 state plus the separate failed capture62290. Production remains unchanged and
SIPFAX_ACQ_REPLAY is still off by default.


### Gate exhaustive acquisition audit, c39a0e4

The exhaustive two-dimensional gain/phase diagnostic is now opt-in via
SIPFAX_ACQ_AUDIT=1. It does not choose receive parameters; the actual acquisition
fit remains enabled. Generated tests compare audit-on/off bits and source sample
sequences byte-for-byte for64/2000 windows with replay both off/on. All match and
retain zero settled generated-audio errors. Baseline generated audio also passes.

Exact CT105 native SHA256:
02398ed7e7eace2d161f7c45e28e3282e6e2165136c92372b03f50f70bf1e450.
Three retained61604 runs per window preserve every decoded bit versus78e0ecc.
For64 symbols, peaks13.305/13.107/13.623ms, means approximately0.275ms, and zero
20ms deadline overruns over948 callbacks each. The worst callback moves to input
sample118560, earlier than the previous acquisition callback125600. For2000,
peaks185.967/185.424/184.859ms and one overrun per run remain at130080. The scan
was a material part of the547ms spike, but long buffered acquisition is still
unsuitable for a live callback. Use only the short-window candidate in subsequent
controlled hardware work; these offline timings do not prove live scheduling.

Evidence: work/v34-audit-c39a0e4-target-timing.log. The acquisition replay option
remains off by default; production has not changed. Next replay the separate
failed capture62290 with the short-window candidate and resolve/measure initial
B1 errors, then perform a controlled call with qualified V.90 restoration.


### Short-buffer hardware reaches DeviceConnected; PPP readiness gap

Separate failed capture62290 still acquires poorly with the short-buffer path
(RMS0.543), but replay timing peaks14.873ms with zero overruns; baseline long-window
replay overruns twice. This is not robust acquisition qualification.
Evidence: work/v34-second-62290-replay.json.

Controlled c39a0e4 hardware trial a772b4d6-d1a2-4005-99e2-b26a01758a91 reached
Windows DeviceConnected and Authenticate at22:33:32.420UTC on2026-09-12, then failed
at22:34:11 with721. Prior trials failed in ConnectDevice with678. One TX B1 entry;
RX acquisition RMS0.163, clock seed-141.7ppm,64 samples replayed, first600 emitted
bits97.0% ones. This is later-stage progress, not PPP success or proof of bad
credentials. Retained RX capture63164 is999424 bytes on CT105.

Code inspection identifies a PPP readiness gap: lm_get_state handles V90/V21/V23
but not SM_V34, so V34 always returns LM_STATE_CONNECTING. linpipe.c emits the
pty-opened event only for LM_STATE_CONNECTED. The native log had no CONNECTED
notification, and the unit journal interval had no pppd/LCP/IPCP events (journal
absence alone is not definitive). Next add and test a V34 readiness predicate
based on completed startup/data readiness, not merely selecting SM_V34. Also
inspect V34 serial versus error-control handling if PPP still fails afterward.

Qualified V90 restoration call89c594dd-0252-4c7e-97e0-09305691f2b6 passed49296,
559-byte expected checksum, zero six-category RAS errors and clean cleanup.
All25 managed files match; guard enabled, Figure9 and acquisition-replay flags
removed, endpoints idle. Evidence: work/v34-short-buffer-hardware-1789252372.*,
work/v34-short-buffer-hardware-diagnostics.json,
work/v34-short-buffer-ppp-journal-audit.json,
work/v34-short-buffer-hardware-final-audit.json and
work/v90-ppp-lifecycle-89c594dd-0252-4c7e-97e0-09305691f2b6.json.
Production is unchanged. Actual bulk upload and multiple physical calls remain open.


### V34 readiness notification 621c538

lm_get_state now reports V34 ready after TX reaches DATA with B1 queued, peer E
is received, receive acquisition completes, and at least8*P data symbols have
been processed. It checks these conditions on every query and returns connecting
after reset/retraining. This is a bridge-readiness gate, not a claim of valid
payload decoding or error-control establishment. The public readiness API test
covers64 startup combinations, unset P, retraining and idle; CI includes it.
Local and exact CT105 readiness/acquisition tests pass. Target SHA256:
54d13b160ab0f21e57d5fda9a05089c37b304dadca5eb1fd019447f91c8895bf.
Evidence: work/v34-ready-621c538-target-validation.log.

Hardware attempt ce7b9d80-ff11-4697-b261-8d50392a9410 emitted CONNECTED-to-PTY and
the application health monitor recorded PPP state starting. This confirms the
missing bridge notification is fixed. The call still failed with678 after four
TX B1 entries; RX acquisitions were poor (RMS0.523/0.500, seeded clock+388.7/
+128.3ppm), so PPP did not complete. Capture63643 remains on CT105. The earlier
DeviceConnected/721 result is preserved separately; this trial does not reproduce
it and must not be counted as V34 success.

Next examine the reliability of the Phase4 clock estimate and acquisition on poor
captures, and the serial/error-control path once modem connection is repeatable.
The readiness gate alone cannot repair incorrect received symbols. Evidence:
work/v34-ready-hardware-1789252868.{json,call.log,native-audit.json} and
work/v34-ready-hardware-diagnostics.json.

Qualified V90 restoration b9afb314-6805-4e2b-9568-f8574ba87ba4 passed49296, expected
559-byte checksum, all six RAS errors zero, ipcp-open and clean cleanup. All25
managed files match; guard enabled and trial flags removed. Evidence:
work/v34-ready-hardware-final-audit.json, work/v34-ready-restoration-call.log,
work/v90-ppp-lifecycle-b9afb314-6805-4e2b-9568-f8574ba87ba4.json. Production unchanged.


### Retry channel estimate persists across V34 initialization

Five clock-policy contrasts on retained63643 (automatic, unseeded, zero,-25,+25ppm)
all acquire at approximately0.520 RMS. The replay's automatic seed is-36.3ppm,
not the live retry's+388.7ppm, so it is not a faithful replay of every retraining
state. Full-trace B1 correlation finds a moderate match at detected E (index61758)
and a stronger later retry (index119173). These results do not justify changing
clock policy. Evidence: work/v34-clock-63643-contrast.json and
work/v34-boundary-63643-search.json.

Decoding the retained TX MP frames reveals the missing condition: first-attempt
MP at15.0..16.5s advertises zero coefficients, but every subsequent attempt at
27..28s,38.5..40s and50.5..52s advertises hQ14
4713,1865,-3920,449,2678,-610. Other fields remain12000/12000,16-state,minimum
shaping,nonlinear off. The log's static one-time advertisement message concealed
this retry change. Prior poor-retry replays assuming zero h therefore use wrong
negotiation parameters; preserve them with this limitation. Evidence:
work/v34-63643-tx-advertisements.json. Audio remains on CT105.

V34_init now invalidates the previous attempt's global channel estimate and clears
its coefficients. Phase4 can still establish a fresh current-attempt estimate.
A raw emitted-frame test exercises the real answer initializer three times with
stale values preloaded: zero advertisement, fresh1234 estimate advertisement,
then zero again on the next attempt. Existing MP, readiness and acquisition-buffer
tests pass locally. This addresses stale-state leakage; it does not qualify live
precoder inversion, force all advertisements to zero, or establish V34 PPP.
Next verify the exact target build and retry advertisements on hardware, and use
actual advertised coefficients for any further retained-retry reference analysis.
Production has not changed.


### V34 serial path after the channel-reset trial

The exact 5f7c5cd target build (native SHA a8d3fbfbbcb708a953c5abf7685760f91602a0f7d6e51e396f1899fa559244d8)
passed target MP/reset, readiness and acquisition tests. Hardware attempt
725b841a-bed0-410c-bfd1-b1571559bede had one B1 entry, opened the PTY,
and entered PPP starting, but ended with Windows error721 without IPCP.
It does not exercise retry advertisement reset. Evidence:
work/v34-reset-hardware-1789253515.native-audit.json and
work/v34-reset-hardware-diagnostics.json. Restoration attempt
d9e47daa-64eb-4f86-9ad1-6ddd05586649 passed at49296bit/s with the expected
559-byte public HTTP checksum, six zero RAS error counters, and clean teardown.
The subsequent work/v34-reset-hardware-final-audit.json verifies all25 deployed
files, the qualified native hash, erasure guard, removed trial flags and idle state.

Source tracing finds that live V34 assigned legacy serial_get_bit/serial_put_bit,
which transmit/recover MSB-first characters. The only serial_init callers were
in lmsim, leaving live V34's serial_wordsize at zero. The V90 production path
already has its own LSB-first framing and LAPM implementation.
V34 now initializes serial state on selection and uses dedicated LSB-first 8N1
callbacks. Independent fixed-wire and all256-octet tests check TX and RX separately,
idle gaps, invalid stop rejection and partial-character reset. The native build,
MP and readiness tests pass locally. Local sanitizer runtime libraries are missing;
the new CI test requests address/undefined sanitizers. This is a byte-framing fix,
not proof of correct V34 payload reception or V42 interoperability. Hardware
qualification of this change remains pending; the deployed V90 release is unchanged.
Wire framing reference: https://onlinedocs.microchip.com/oxy/GUID-173AD72D-41FE-4760-A93C-7078A02BD908-en-US-7.1.1/GUID-7F09657C-791A-43DC-9238-56BDE5EC97F7.html

The exact c5962e0 CT105 build hashes to
1f416c6f2820d05dd5a4c16353a633e0b5dbf5b50b3af4ddea82c1d95827c2c3.
Target ASan/UBSan serial vectors, MP/reset, readiness, both acquisition-buffer
windows and dynamic-loader checks pass (work/v34-serial-c5962e0-target-validation.log).
Hardware attempt ff104004-8baa-4c0b-83d6-390054bbc94e fails with error678 after
four B1 entries. The PTY opens and PPP enters starting, without IPCP. Evidence:
work/v34-serial-hardware-1789254171.{json,call.log,native-audit.json}.
This call does not isolate bit-framing effects from training variability.

Restoration attempt d117b5be-a8b3-44c7-a4ab-37804c184fd2 passes at49296bit/s,
expected public HTTP checksum and zero six-category RAS errors. The final audit
verifies25 managed files, qualified native hash, guard enabled, trial flags removed
and idle endpoints (work/v34-serial-hardware-final-audit.json).

The next protocol integration gap is explicit in source: v8.c unconditionally
advertises V8_DATA_LAPM, while live V34 assigns raw serial callbacks. V90 uses
v90lapmlink.c for error-control negotiation and framed DTE octets. V34 needs a
matching error-control path, with link readiness, negotiated bit rates, retraining
and bounded FIFO handling tested before promotion. This source mismatch is not
proof of the latest error678's cause; PHY acquisition remains variable. Notebook
health still reports DialUpLab1.1.1.0, so genuine bulk-upload acceptance is pending.
At head c5962e0, PR29 application CI passes and native CI remains in progress.

### Opt-in V34 LAPM integration

SIPFAX_V34_V42=1 selects an answerer LAPM DTE bridge. It reuses the existing
V90 LAPM protocol/selector with one V34 decoded stream, stores per-call protocol
state outside the retrained modulation union, and uses the negotiated TX rate.
V34 readiness now additionally requires an initialized, connected LAPM link that
is not reacquiring. PTY reads pause during retraining while queued LAPM frames
remain available for retransmission. Raw 8N1 remains available with the option off.
The qualified deployment has not enabled this experimental path.

The new v34-dte test exercises the actual bridge against an originating bundled
SpanDSP endpoint:16KiB in each direction, independent expected octets, a blocked
128-byte DTE FIFO, partial writes, busy recovery, and a one-second retraining gap
with TX rate changing12000 to9600. Both transfers finish exactly with no protocol
errors or queue overflow; the retrain preserves and resumes the link. These are
protocol-level tests using generated bits, not independent modem or waveform
interoperability evidence. Native build, MP, readiness (including LAPM negotiation
and reacquisition gates), and acquisition-buffer tests pass locally. Target
sanitizers and hardware qualification remain required before promotion.

The exact 9e1c135 CT105 build hashes to
f707990192b6960ea01ac0ca032427f909a5808bbcfc1ab4eb0e239c45eda287.
Serial and V34 DTE ASan/UBSan tests, MP, readiness, acquisition-buffer windows and
loader checks pass (work/v34-lapm-9e1c135-target-validation.log).
Hardware attempt22628e55-93cd-4a3c-972b-ca2fae86105c with SIPFAX_V34_V42=1
ends error678 after four B1 entries, without PTY readiness or a logged completed
ODP/LAPM establishment. The retained native log has zero [v42] messages; this
alone does not locate failure in the waveform versus detection input. Final
acquisition reports lattice RMS0.363. Evidence:
work/v34-lapm-hardware-1789254660.{json,call.log,native-audit.json} and
work/v34-lapm-hardware-diagnostics.json. Full native log remains on CT105 at
/tmp/v34-lapm-9e1c135-hardware-native.log.

Restoration attempt30bd3778-6bce-42ff-b860-ebf7487b69c3 passes49296bit/s,
expected559-byte public HTTP checksum, all six RAS error counters zero, and clean
teardown. work/v34-lapm-hardware-final-audit.json verifies all25 managed files,
qualified native hash, guard enabled, trial flags removed and idle endpoints.
Next inspect actual post-B1 receive bits and LAPM detection input in place,
including retry advertisements, before changing acquisition settings. The
protocol-level transfer tests do not establish hardware V34 fallback.

### Retained LAPM trial65423: negotiation and detector audit

In-place wire decoding of TX65423 confirms zero precoder coefficients in all
four attempts, with ca/ac12000,16-state receive trellis, minimum shaping,
nonlinear off and CRC-valid ACK transitions. This supplies hardware retry evidence
for the earlier channel-estimate reset fix; the previous reset trial had only one
attempt. Evidence: work/v34-lapm-65423-replay.json. Audio remains on CT105.

A replay with these recorded MP parameters and the64-symbol acquisition buffer
finds E at symbol62921, clock seed-46.0ppm and acquisition RMS0.135. The first600
bits are all ones; the first1200 contain1156 ones, then the next1200 contain555.
Across2816 live-size callbacks, worst runtime14.649728ms, zero20ms overruns.
The replay emits91688 bits but does not reinitialize at every live watchdog
retrain, so its later output must not be presented as an exact live bitstream.
The live log itself had one RMS0.144/all-ones early window and later retries at
RMS0.395 and0.363. A final-window-only summary obscures this variation.

The actual v42_detect_bit implementation, compiled and called in place, finds
no ODP in the first6000 replay bits (no accepted DC1 characters). Across the whole
replay it sees26 isolated accepted DC1 characters, maximum alternating run1,
no detection. A synthetic eight-character alternating DC1 control detects at
bit63;2000 idle ones never detect. This supports investigating the signal reaching
error-control detection, not weakening detection criteria. It does not prove the
caller transmitted an ODP, or distinguish transmitter handshake failure from
receive tracking failure. Evidence: work/v34-lapm-65423-odp-audit.json.
Production remains the restored qualified V90 release; these analyses make no
service changes.

Live DTE observability now uses the existing SIPFAX_V90_LAPM_DIAGNOSTICS=1
switch for a bounded one-line-per-second V34 summary: requested/initialized,
TX/RX bit counts, RX ones, retrain count, negotiated TX rate, detection, selection,
connection, pending bytes and protocol errors. Counters are per call, persist
across its retrains and reset for a new call. No payload bytes or raw bits are
logged. The generated 16-KiB bidirectional transfer/backpressure/retrain test
independently counts callback invocations and ones and checks these counters,
including reset. Both scenarios and native readiness tests pass locally; the
native build succeeds. The next hardware trial must verify requested=1 and
initialized=1, then compare cumulative counter deltas around each training
attempt. This closes an observation gap, not the underlying hardware failure.

### Live V34 ODP detection and post-startup frame failure

Exact15a266d target native SHA
45024a4703124f7129574ec6705d44186b80d87a50fd52b4fa409dc0aedbfc2e
passed the target build, serial/DTE sanitizers, readiness, MP and acquisition
checks. Hardware attempt7596e320-5d64-4b75-867d-2f341a1239de ends error777.
Unlike preceding trials, it logs completed ODP detection, ten transmitted ADPs,
and stream selection after continuous flags. All ten initially logged selected
HDLC frames are CRC-invalid; link connection never completes. Later retries
attempt to reacquire the negotiating LAPM stream. This is a detection milestone,
not PPP success or proof of valid payload reception.

The new live counters verify requested=1, initialized=1, and actual traffic:
at16s TX2688/RX560bits; at17s TX14672/RX9819, detected=1, selected=1,
connected=0, negotiated TX12000. RX delivery advances irregularly in the next
seconds, while TX remains near12000bit/s. By24s a retrain is counted. Final72s
summary has TX342048/RX278586, three retrains and no connected link. Initial
acquisition RMS0.185 and600bits98%ones establish a useful startup window;
later acquisition alone cannot establish data integrity. Do not infer zero
frame errors from the LAPM protocol errors counter, which remains zero even
while the logged HDLC frames fail CRC. Next inspect the post-ODP receiver and
mapping/erasure behavior in retained capture66046, with actual MP parameters.

Evidence: work/v34-dte-hardware-1789255382.{json,call.log,native-audit.json},
work/v34-dte-hardware-diagnostics.json and work/v34-dte-hardware-protocol.json.
Full native log and audio remain on CT105. Restoration attempt
24e13639-e1cc-4453-9732-a442363102d5 passes49296bit/s, expected559-byte
public HTTP checksum, six zero RAS error counters and cleanup. Final audit
work/v34-dte-hardware-final-audit.json verifies25 files, qualified native hash,
guard and idle endpoints. Separately, CI run34724716055 for9e1c135 completed
successfully in both application and native-modem jobs.

### Clock-seed contrast on the call that reached ODP

In-place replay of retained RX/TX66046 confirms zero advertised precoder
coefficients and reconstructs ODP at decoded bit1212. Its position trace exposes
ten early51-bit gaps, consistent with an invalid mapping frame plus the23-bit
descrambler flush. The first12000 positions contain11439 delivered bits; later
windows initially have no gaps. This quantifies discards rather than inferring
them from irregular callback totals. Full-call replay does not reproduce each
live reset; the controlled comparisons below use only the first24 seconds.

With the recorded negotiation and automatic replay seed+22.9ppm, the actual
LAPM bridge recovers four CRC-valid77-byte XID commands (address03, controlAF,
valid envelope). Thus the caller did transmit meaningful negotiation traffic.
The baseline has48 selected HDLC frames,4valid and10770 missing bit positions.
Disabling equalizer adaptation gives50/4 and10905 missing; disabling AGC gives
47/4 and9051 missing; disabling carrier correction gives101/0 and37013 missing.
These counts are replay-specific, not live connection results. The selected
frame logger limits early output, so its first-ten sample alone is not a count
of every valid frame. Evidence: work/v34-dte-66046-tracking-contrast.json and
work/v34-dte-66046-valid-frame-summary.json.

The live first-attempt seed was+175.5ppm. Replaying the same cropped waveform
with only SIPFAX_DATA_CLK_PPM=175.5 yields83frames,0valid and37203 missing bits;
SIPFAX_DATA_CLK_PPM=0 yields49frames,4valid and10622 missing. All contrasts stay
within the20ms callback deadline. This isolates a harmful live clock seed on
this retained recording and supports a bounded zero-seed hardware comparison;
it does not justify removing clock recovery or globally assuming zero drift.
Evidence: work/v34-dte-66046-live-clock.json,
work/v34-dte-66046-clock-contrast.json and work/v34-dte-66046-odp-audit.json.
No service or deployed binary was changed during these analyses.

### Zero-seed hardware comparison: not qualified

Using the same15a266d native binary, trial
7cc63670-0900-43e5-b90b-1c4bb4c7af82 adds only SIPFAX_DATA_CLK_PPM=0
to the diagnostic V34 configuration. The live log confirms manual0ppm on each
acquisition. The call ends error678 without ODP/stream selection or LAPM
connection. Acquisition RMS values0.123/0.148/0.378 coexist with initial600-bit
ones fractions61.5%/47.3%/47.3%. A small lattice residual therefore does not
establish correct mapping/bit alignment. The first active DTE snapshot verifies
requested=1, initialized=1; the final snapshot has56028TX/4456RX bits and three
retrains. The strong retained66046 seed contrast remains valid for that recording,
but this hardware result does not support zero seed as a general default.

Evidence: work/v34-zero-clock-hardware-1789255905.{json,call.log,native-audit.json}
and work/v34-zero-clock-hardware-diagnostics.json. Capture66406 and full native
log remain on CT105. Next compare the first B1/data transition and decoded frame
alignment in this failed recording against66046, preserving actual negotiation
and avoiding a lattice-RMS-only acquisition criterion. No seed default changed.

Restoration attemptf1a8caac-b1e2-41e3-ab07-3d0dc43d2c18 passes49296bit/s,
expected559-byte HTTP checksum, six zero RAS error counters and cleanup.
work/v34-zero-clock-hardware-final-audit.json verifies all25 managed files,
qualified native hash, guard enabled, trial flags removed and idle endpoints.

### Failed zero-seed call: B1 interrupted by an input signal gap

Replay66406 with matching zero seed reproduces poor initial bits despite
RMS0.152. All decoded TX MP frames advertise zero precoder coefficients.
On the first24-second crop, automatic alignment and all eight manual2D symbol
offsets (with automatic realignment disabled) fail to recover ODP or select a
stream; each loses more than36000 bit positions. This does not support a simple
mapping-frame entry-offset fix. Evidence:
work/v34-zero-66406-replay.json and work/v34-zero-66406-alignment-contrast.json.

A complex scalar fit of the120 received symbols at E to the generated12k,
16-state, minimum-shaping B1 reference explains99.852% of66046's energy but
only64.336% of66406's. Searching offsets-256..256 symbols finds the best fit
at offset0 for both;32/64-state references fit worse. The reference self-fit is
1.0. There is no CFO search in this comparison, so inspect its short segments:
all six20-symbol windows of66046 explain>99.8%; the first three windows of66406
explain99.72..99.85%, then the fit and amplitude collapse. Evidence:
work/v34-b1-66046-66406-boundary-comparison.json and
work/v34-b1-66046-66406-segments.json.

Direct10ms input-energy windows confirm a real gap in the captured receiver
input, rather than merely a phase-fit error. Raw RX RMS is1708.5 at17.120s,
1515.2 at17.130s,54.6 at17.140s,then21.7..24.2 through17.190s; it recovers
to850.8 at17.200s and1780.5 at17.210s. Post-echo input shows the same gap.
TX RMS stays above2100 throughout. The first B1 symbols are therefore received
correctly before an approximately60ms input interruption. These aggregate levels
do not identify whether the caller deliberately stopped/retrained or the ATA/RTP
path introduced the gap. Next correlate that boundary with transport evidence
and caller/server training events before changing the receiver's bit alignment.
Evidence: work/v34-zero-66406-b1-audio-levels.json. All raw audio and receiver
traces remain on CT105; these analyses made no production changes.

### Gap followed by Tone B: caller retrain evidence

No retained transport capture for this call was found in the inspected top-level
/tmp and /var/log/sipfax directories. The trial did not request one. The20-line
service journal interval contains no erasure/late/underrun/discontinuity text,
but that is weak negative evidence because timing events need not be logged.
Do not interpret it as proof of loss-free RTP.

The raw input's quiet window contains15 distinct sample levels (34 of320 samples
are zero), rather than the uniform silence generated by RtpContinuity's gap-fill
path. More decisively, the returning signal is a sustained1200Hz tone: at17.22,
17.32,17.42,17.62 and18.02 seconds, more than99.95% of windowed spectral energy
lies within40Hz of1200Hz. Before the gap, input is broadband modem data. This
supports caller-initiated retraining, rather than an isolated missing-data hole
followed by continuing B1. It does not identify what triggered the caller's retrain
or exclude an earlier ATA/RTP disruption. Evidence:
work/v34-zero-66406-gap-origin-audit.json.

ITU-T V.34(02/98),11.5.1.1 describes caller-initiated retrain as70±5ms silence,
then Tone B while awaiting the answering sequence. The observed coarse60ms
near-silence windows plus their transition intervals are consistent with that
sequence; no exact standards timing conformance is claimed from10ms bins.
Reference: https://www.itu.int/rec/dologin_pub.asp?id=T-REC-V.34-199802-I%21%21PDF-E&lang=e&type=items
The next useful check is the server's outgoing E/B1/initial-data waveform and
negotiated caller receive requirements, with transport timing captured during a
future hardware comparison. Receiver alignment sweeps cannot recover data after
the caller has returned to Tone B. Production was not modified.

### Answering-direction reference and caller receive requirements

The B1 reference diagnostic now accepts SIPFAX_B1_CALLING=0 for the answering
role; omission retains caller-role1. Invalid values fail explicitly. Tests verify
unchanged default output, distinct deterministic answering output and invalid-role
rejection alongside the existing parameter tests. Exact4f05cc7 target build
(native SHA7d6e0d98deb8fcc93e98e37a35078cbb2d951e8dbd04b3d33defaf3c576497e0)
passes B1 references, serial/DTE sanitizers, MP, readiness and acquisition checks.
Evidence: work/v34-b1-role-4f05cc7-target-validation.log. This is diagnostic-only;
production has not changed.

Wire-decoded caller MP from66406 at15.5..16.5s requests ca33600/ac31200,
32-state trellis, expanded shaping, nonlinear encoding and precoder hQ14
1469,2069,-309,-1235,1213,804. The server caps its outgoing rate at12000 as
advertised. Its first-attempt log confirms those coefficients, nonlinear ON,
L20 expanded constellation and32-state trellis. It estimates shaped mean energy
9.47, warped mean11.19, amplitude4369 and postgain143/128 before B1. These
observations rule out merely ignoring these MP requests; they do not validate the
emitted symbols, normalizer or physical waveform. The forthcoming TX reference
comparison must use answering role, shape1,trellis32, actual coefficients and
nonlinear normalization, rather than the caller-direction/unprecoded reference.
Evidence: work/v34-zero-66406-caller-mp.json and
work/v34-zero-66406-tx-parameters.json.

### Retained answering B1 matches the configured encoder waveform

An in-place TX66406 comparison synthesizes the answering B1 reference with
12000bit/s,32-state trellis, expanded shaping, hQ14
1469,2069,-309,-1235,1213,804 and nonlinear mean9.47. The waveform model
uses the actual7-phase pulse-shaping table, quantized carrier increment, symbol
amplitude4369 and index2 pre-emphasis coefficients. It searches seven baud
phases and a16.0..17.3s window, fitting two quadrature components to allow an
unknown carrier phase/gain. Only the125..127-sample interior with fully known
B1/filter support is scored; neighboring unknown training/data symbols are
excluded. Synthetic quadrature self-fit is1.0.

The negotiated model explains0.9999997447 of the selected125-sample window's
energy at16.9495s (baud phase6). A wrong caller-role reference peaks at0.15747.
Disabling nonlinear encoding still fits0.99816, so that control is weak and
should not alone be used to identify nonlinear compliance. The result verifies
consistency between the saved transmit waveform and this configured encoder
model on the inspected interior. It is not an independent audit of all V34
coding equations, the full E/B1 seam, or delivery through RTP/ATA. It does rule
out a gross role/parameter mismatch or output-buffer corruption in this window.
Evidence: work/v34-tx-b1-66406-wave-audit.json; model artifacts and all audio
remain in /tmp/v34-tx-b1-66406 on CT105. Next obtain scoped transport timing
across the server/PBX/ATA path during a hardware trial and compare the emitted
and delivered startup windows. No production change was made for this audit.

### Transport-instrumented V34 trial

Trial f5c1c230-c546-4b36-9a7b-4f83ee1c1286 uses the same15a266d diagnostic
binary with automatic clock seed and ends error678. CT105 retains the full
scoped UDP capture; FreePBX records only selected RTP/IP/UDP header fields,
without an audio payload file. Capture processes were verified alive before
dialing and stopped by their exact PIDs afterward. The controller restored the
qualified native release. Evidence prefix: work/v34-transport-1789257124;
call controller: work/v34-dte-hardware-1789257128.

FreePBX metadata contains13746 RTP packets across four legs. ATA->PBX has3442
packets with no sequence gaps, median20.000ms and maximum26.567ms spacing;
its first timestamp increment is120, then all3440 remaining increments are160.
PBX->CT105 has3431 packets, maximum26.535ms spacing and continuous160-sample
timestamps. CT105->PBX has3429 packets, maximum21.560ms spacing and continuous
160-sample timestamps. The CT105 pcap independently has those same two packet
counts,160-byte G711 payloads, and no sequence or timestamp anomalies.

PBX->ATA has3444 packets with no sequence gap. Its only long interval is
131.797ms at0.4115s after stream start, accompanied by a backward2400-sample
timestamp step. Payload type changes from0 to96; subsequent timestamps advance
160 samples and spacing stays below30ms. Dynamic payload96 UDP lengths are
181/345 bytes; do not infer audio sample count by subtracting20 bytes from UDP
length on that leg. Those larger packets are not timestamp gaps. This header-only
capture does not establish dynamic payload contents or successful ATA reception.

Sustained1200Hz Tone B is present in regularly sequenced incoming CT105 packets
at relative17.220..18.340s,29.140..30.260s,41.060..42.180s, and later intervals.
Thus the observed caller retraining in this call is not explained by missing
incoming RTP at CT105 or a packet-sequence gap observed at the PBX. The capture
does not exclude loss after PBX egress, ATA analog playout effects, or protocol
mismatch. Retain the early timestamp discontinuities for future startup review;
they precede the observed B1/retrain boundary and are not proven causal.
Evidence: work/v34-transport-1789257124-pbx-audit.json,
work/v34-transport-1789257124-pbx-timestamps.json,
work/v34-transport-1789257124-ct-audit.json. Raw audio remains on CT105.

Restoration attempt5901548f-c295-4402-b44b-de3b94e8b29b passes49296bit/s,
expected559-byte HTTP checksum, six zero RAS error counters and cleanup.
work/v34-transport-hardware-final-audit.json verifies25 managed files, qualified
native hash, guard and idle endpoints. No experimental build remains deployed.


### Startup handoff and RED clock preservation

Read-only inspection confirms the active PBX dialplan sends Progress(), waits
0.3 seconds, registers the ATA RTP destination with the RED service, and then
calls SIPfax. The deployed encoder matches deploy/sipfax_red.py (SHA-256
 e2183498aeeceadd219bfcf9c2a0be7de472fa6d105cde15df067714b73a90f0).
Its wrapper preserves sequence, timestamp, SSRC and marker; PT96 is the configured
RFC2198 encapsulation, not evidence of a different sample rate.

The retained header capture identifies the exact transition: sequence8831 /
timestamp2400 / PT0 is followed 131.797ms later by sequence8832 / timestamp0 /
PT96, with unchanged SSRC1602114569. The latter is primary-only (UDP181);
sequence8833 / timestamp160 resumes prior-plus-current redundancy (UDP345).
This is consistent with the early-media to bridged-media handoff. The encoder
cannot create the rewind through its current timestamp-preserving code, but
this inspection alone does not locate the upstream timestamp assignment or
establish that the ATA tolerates it. No timestamp rebasing was deployed.

A regression exercises this captured rewind both when registration starts at
the handoff and when the bridge was already active. It verifies preserved RTP
identity/marker, exclusion of stale early-media redundancy, and recovery of the
next valid redundant block. All9 RED tests pass with local Unix-socket access;
the sandbox-only run failed the existing control-socket bind test, not the wire
test. A fresh deployment audit verifies all25 qualified files, erasure guard,
idle endpoints and removed trial flags (work/v34-startup-current-deployment-audit.json).
The audit reuses the recorded restoration call evidence; no new call was made.


### Dedicated V34 answer retrain entry

Inspection against V.34 (02/98) 11.5.2.2 finds that the Tone-B watchdog previously
emitted one silent callback then restarted the initial INFO0a exchange. The
answer retrain response requires 70 +/-5ms silence followed by Tone A and the
11.2.1.2.3 ranging procedure. A dedicated entry now resets Phase2, emits exactly
560 silent samples across callback boundaries, then sends pure Tone A without
INFO0a. Its ranging gate requires Tone B and at least400 emitted Tone-A samples.
Normal initial startup retains its existing INFO0a path. The outer watchdog's
roughly600ms recognition window and three-restart cap are unchanged; this change
does not claim to correct the initial training failure or all retrain behavior.

The waveform regression starts with a dirty initial handshake and checks80-,160-
and257-sample callbacks, exactly70ms silence, pure Tone A for at least50ms,
subsequent phase reversal, and no premature ranging with absent Tone B. It also
checks initial INFO0a remains present. CT105 ASan/UBSan passes, as do the complete
native build, serial/DTE sanitizer tests, B1 reference, readiness, MP field and
acquisition replay regressions. The local sanitizer link was unavailable because
libasan.so.8.0.0 is missing; sanitizer evidence is from the target.
Candidate /tmp/v34-retrain-entry-candidate has native SHA-256
356931ce7976f3ead5bf016c0eb513427a9b1307dfc40386df18cb2929145b14.
Evidence: work/v34-retrain-entry-full-target-validation.log. Physical fallback
still requires a hardware trial and successful PPP traffic.


### Physical V34 PPP milestone with retrain response correction

Source0221dfb, native356931ce7976f3ead5bf016c0eb513427a9b1307dfc40386df18cb2929145b14,
was tested with the existing V34-only12k limits, FIG9,64-symbol acquisition replay,
CMA and opt-in LAPM settings. Attempt b43f1f65-8bdc-432e-9de8-719e0b41b933
CONNECTS at12000bit/s. The native trace records three B1 entries and two caller
retrain responses before ODP detection, stream selection and LAPM connection.
Each corrected restart enters OPEN at70ms and SEQ at120ms (50ms Tone A), followed
by ranging. The short call alone cannot prove the correction caused success.

Server state reaches ipcp-open on ppp0. Two public HTTP probes use PPP source
10.64.0.2, return200 and559bytes each, and match SHA-256
ff67a9d764d6a2367a187734e697f6a53217db9a21c101d410a113ca871a299d.
All six RAS error counters are zero before and after both probes, with the last
statistics at21050ms after connection. LAPM data counters show errors=0 while
connected. The call disconnects and frees sessions, addresses and media lines.
Evidence: work/v34-retrain-entry-hardware-1789258267.json,
work/v90-final-live-b43f1f65-8bdc-432e-9de8-719e0b41b933.json,
work/v34-retrain-entry-hardware-timeline.json and
work/v34-retrain-entry-hardware-acceptance-audit.json. Full receiver trace remains
on CT105 at /tmp/v34-retrain-entry-candidate-hardware-native.log.

The controller restores qualifiedV90. Restoration attempt
c085e4bd-53e9-4337-b148-70943c37ca46 passes49296bit/s, PPP/IPCP, the expected
559-byte checksum and zero RAS errors. work/v34-retrain-entry-hardware-final-audit.json
verifies all25 managed release files, enabled guard, removed trial flags and idle
endpoints. No experimental deployment remains active. DialUpLab still reports
1.1.1.0. Repeatability, longer V34 traffic, automatic V90-to-V34 negotiation and
the other release gates remain open; do not label this one short call a release.


### Unchanged-candidate V34 repeatability campaign

Three additional calls use source0221dfb and the same native356931ce...145b14,
V34-only12k limits and diagnostic settings. No tuning occurs between calls.
Each trial restores qualifiedV90 and confirms idle resources before the next.
Full traces now have unique per-trial paths on CT105; the first successful call's
/tmp/v34-retrain-entry-candidate-hardware-native.log is preserved.

| Attempt | Result | Observations |
| --- | --- | --- |
| f3535254-e3f4-4457-b3e4-4304b80b67ec | Failed777 | Four B1 entries; three retrains; no ODP detection. |
| 57d57a2f-239f-44b1-9806-5403bea5cc91 | Failed678 | Three B1 entries; ODP detected once, no LAPM connection. |
| 666be31e-447e-4660-9654-4ad9b0c34faf | Connected12000 | Two B1 entries; one retrain; LAPM and two valid public HTTP probes. |

The successful repeat's probes each return559bytes and the expected
ff67a9d764d6a2367a187734e697f6a53217db9a21c101d410a113ca871a299d checksum,
from PPP source10.64.0.2. Both before/after sets have all six RAS counters zero.
Including the initial milestone, the retained sample is two successes and two
failures: repeatability is not achieved. Do not infer a population success rate.
Evidence: work/v34-retrain-repeat-results.json and per-attempt reports; metadata
comparison work/v34-retrain-repeat-comparison.json. Raw audio stays on CT105.

All four calls measure line echo delay1428samples. The first success acquires
RMS0.143 with seed-42.6ppm; the second succeeds atRMS0.149 with seed+167.9ppm.
Failed attempts also acquire atRMS0.144..0.170 and include seeds-1.3,+213.7,
+226.4,+332.0ppm (one earlier acquisition is worse at0.373). This contradicts a
simple rule that every positive seed fails or every low acquisition RMS succeeds.
Compare actual subsequent receive integrity and caller retrain timing next;
these metadata alone do not identify the cause. No new parameter was deployed.

Restoration attempt7ba4b285-6670-473e-a693-a75ecab219f9 passes49296bit/s,
PPP/IPCP, expected559-byte public HTTP checksum and zero RAS counters.
work/v34-retrain-repeat-final-audit.json verifies all25 qualified files, guard,
removed trial flags and idle endpoints. PR29 stays draft. CI34727480442 on4076a56
has application tests completed successfully; native-modem was still running
at the recorded check (work/v34-retrain-repeat-ci.json), not a passed check.


### Receive-delivery comparison and in-place replay of failed call68966

The four-call DTE-counter comparison separates stable one-second intervals with
unchanged retrain index,12000-bit/s configured rate, and11900..12100 transmitted
bits. Both successful calls deliver11984..12012 receive bits per connected
interval (20 and21 intervals respectively). The failed678 call's initial epoch
instead delivers2842..7510 bits/s, median5056 across seven intervals ending at
seconds17..23. This is delivered-bit accounting, not a measured wire BER or RTP
loss count; suppressed decoder output and other receiver behavior can reduce it.
See work/audit_v34_receive_delivery.py and work/v34-retrain-receive-delivery.json.

First600 decoded B1 bits distinguish two failure classes: callf3535254's three
acquisitions have52.3%,48.5%,47.3% ones, with no ODP. Call57d57a2f initially has
100% ones and detects ODP, then loses receive integrity before LAPM connects.
The two successful calls have98.5% and100% ones. Low lattice RMS alone does not
validate decoded B1. The2400-bit percentages include post-B1 protocol data and
must not be treated as a B1 bit-error measurement. Evidence:
work/v34-retrain-bit-health-events.json.

Capture68966 corresponds by recording end time to failed attempt57d57a2f.
All echo reconstruction, PCM, decoded bits, bit positions and full replay logs
remain on CT105 under /tmp/v34-lapm-68966. Emitted MP decoding verifies12k caps,
16-state unshaped receive advertisement and zero precoder taps on each retry.
The in-place receiver reproduces the +226.4ppm clock seed and clean first B1
(98% ones in the replay); its full capture replay has no20ms callback overruns.
The replay does not reproduce every live retrain reset.

For the first24seconds of the same post-echo recording, only the clock seed is
changed between two receiver/scanner runs:

| Seed | Delivered bits | Missing decoded positions | HDLC frames / valid |
| --- | ---: | ---: | ---: |
| Actual live +226.4ppm | 37699 | 59713 | 72 / 0 |
| Zero | 76035 | 21405 | 44 / 4 |

Both detect ODP and select candidate0. The four valid frames reach the actual
LAPM parser as77-byte XID commands with valid envelopes. This is more than an
accidental short-frame CRC count, but neither offline run establishes a duplex
link. Worst callback times are13.29 and13.22ms. Evidence:
work/v34-repeat-68966-replay.json, work/v34-repeat-68966-clock-contrast.json and
work/v34-repeat-68966-valid-frame-audit.json. Zero still leaves substantial gaps;
previous zero-seed hardware failed, and +167.9ppm succeeded in another call.
Do not deploy a zero-seed rule from this contrast. Next examine whether the
Phase4 instantaneous Gardner rate estimate is a reliable initializer for the
separate live data timing loop; compare stable-window estimates against retained
successful and failed recordings before changing the default. No production
configuration or binary was changed during this analysis.


### Phase4 clock publication and averaging cross-check

A temporary instrumented native copy under /tmp/v34-clock-publication-audit
records each Phase4 equalizer pass and MP validation during capture68966 replay.
Data mode's +226.4ppm seed is the final pass of the four-point hypothesis that
successfully decodes one MP frame. It is not a stale rejected hypothesis in this
recording. Earlier pass estimates vary from about-566ppm to-136ppm before ending
at-226.417757ppm; the data loop negates this value. This audit does not establish
whether all other calls publish only validated estimates. Evidence:
work/audit_v34_clock_publication.py and work/v34-clock-publication-audit.json.

An isolated diagnostic build stores the final2048 symbol-rate integrator samples
per equalizer pass and, when requested, replaces only its published final estimate
with a trailing mean. The subsequent receiver and LAPM scanner are unchanged.
No running service uses this build. All raw and decoded data remain on CT105.
The two24-second recording comparisons are:

| Capture | Averaging symbols | Seed ppm | Delivered / missing bits | HDLC valid / total |
| --- | ---: | ---: | ---: | ---: |
| 68966 | 0 | +226.4 | 37689 / 59723 | 0 / 72 |
| 68966 | 256 | -44.6 | 76331 / 21109 | 4 / 44 |
| 68966 | 1024 | -56.1 | 76642 / 20798 | 4 / 45 |
| 68966 | 2048 | -49.9 | 76382 / 21058 | 4 / 46 |
| 66046 | 0 | +22.9 | 74994 / 10770 | 4 / 48 |
| 66046 | 256 | -130.5 | 58049 / 27715 | 0 / 104 |
| 66046 | 1024 | -85.6 | 75873 / 9891 | 4 / 48 |
| 66046 | 2048 | -62.7 | 75840 / 9924 | 4 / 53 |

The256-symbol window regresses the second recording, so success on68966 alone
would have been misleading. The longer windows preserve four valid frames on
both recordings and reduce missing positions relative to their respective
controls, but neither establishes duplex LAPM or eliminates corruption. Both
recordings came from failed physical calls; successful physical-call recordings
still need a comparable replay before considering a hardware candidate. Worst
callbacks remain below15ms with no20ms overruns. The newly compiled zero-window
68966 control differs from the earlier build by10 delivered bits; its seed and
zero-valid-frame outcome agree. Preserve this difference rather than claiming
bit-identical baselines across separately compiled instrumentation builds.

Evidence: work/contrast_v34_clock_average.py,
work/contrast_v34_clock_average_66046.py,
work/v34-clock-average-68966.json and work/v34-clock-average-66046.json.
The second experiment uses /tmp/v34-clock-average-66046 and preserves the first
experiment's outputs. Production source, deployment and clock defaults are
unchanged. Next replay a successful physical recording with explicit retrain
boundary handling, then decide whether a longer averaged seed warrants a bounded
hardware comparison. All five acceptance gates remain open.


### Successful-call replay and experimental clock-history option

Successful physical call666be31e uses capture69114. Echo cancellation is replayed
on the full capture before slicing30-second windows at17,18 and19seconds, around
the sole retrain. These are explicit alternative initializations, not an exact
reproduction of all live receiver state. Instantaneous-seed controls recover
0,55,0 valid frames respectively, despite all reporting+167.9ppm and clean B1.
Both1024- and2048-symbol averages recover54,55,56 valid frames at those offsets.
The windows cover different recording end times, so compare variants within an
offset rather than interpreting54/55/56 as differing rates over identical data.
Worst callback stays below13.6ms. The18-second variants deliver243925bits with
459 missing decoded positions. An independent bit-unstuffing/CRC16 decoder
confirms55 valid frames and the same ordered frame digest for all three variants:
05ee36e7cfc8e21bb921b54ed834b71fb50be01950c05c31a60ebb1217292f88.
No frame payloads leave CT105. Evidence: work/v34-success-69114-clock.json,
work/v34-success-69114-clock-offsets.json and
work/v34-success-69114-frame-identity.json.

The candidate now supports explicit SIPFAX_CLOCK_AVERAGE=1024 or2048. Unset,
0 and unsupported values retain instantaneous estimates; unsupported values log
a warning. History is local to one equalizer pass, bounded at2048 samples and
reset before each pass. A short pass falls back to its own last estimate rather
than stale history. Existing manual data-clock overrides still take precedence.
The option remains off by default pending physical qualification.

The rolling-history regression covers both windows, more than three buffer
wraps, insufficient history, invalid bounds, alternating positive/negative rates
and reset. CT105 ASan/UBSan, full native build, serial/DTE, retrain waveform,
B1 reference, readiness, MP and acquisition regressions pass. Packaged candidate
/tmp/v34-clock-average-candidate has SHA-256
b048fd0a367928518a695c6b525c0065100a094f23dcbfcd91d392d11f936bdd.
Its18-second successful-call replay also recovers55 valid frames at all three
settings. Evidence: work/v34-clock-average-target-validation.log and
work/v34-success-69114-final-clock.json. No averaged candidate has yet passed
physical PPP; production remains qualifiedV90.
