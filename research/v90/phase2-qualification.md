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
