# Phase-two hardware qualification

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
