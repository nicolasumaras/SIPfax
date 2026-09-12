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
A new captured one-hour run, `8b43d200-3179-43d7-b82a-1cb20f331a88`, is in progress;
no final endurance result is claimed here. The later RTP-readiness application
change `10b8228` remains undeployed during this run.

During the new hardware run, read-only connection tracking confirmed the actual
PPP client using the new NAT mapping: source ports 1189 and 1200 were translated
to 51381 and 58200 for completed public HTTP connections. An interim independent
audit at 377 seconds verified 47 download/upload pairs with zero reported modem
errors. This confirms live rule use; it is not the final endurance result.
