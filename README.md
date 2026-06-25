# SIPfax

SIPfax is a direct SIP/RTP service skeleton for carrying dial-up modem traffic
over a tightly constrained G.711 pass-through path.

The first supported baseline follows the LKMA-168 decision:

- SIPfax owns SIP dialog state and RTP once a call is routed to it
- `PCMU` or `PCMA` only
- one inbound call session at a time
- no transcoding, T.38, VAD, comfort noise, conferencing, recording, or Asterisk
  media side-channel in the live modem path
- Cisco ATA 191/192-class ATA routed through FreePBX to the SIPfax VM
- in-process dial-up protocol termination on the SIPfax VM; no physical modem is
  required on the server

## Run

```bash
npm ci
npm start
```

Default listeners:

- SIP UDP: `0.0.0.0:5060`
- RTP UDP: `0.0.0.0:40000`

Configuration is environment-driven:

| Variable | Default | Purpose |
| --- | --- | --- |
| `SIPFAX_HOST` | `0.0.0.0` | SIP and RTP bind host |
| `SIPFAX_PUBLIC_HOST` | `127.0.0.1` | Address advertised in SIP/SDP |
| `SIPFAX_SIP_PORT` | `5060` | UDP SIP port |
| `SIPFAX_RTP_PORT` | `40000` | UDP RTP port |
| `SIPFAX_OPERATOR_HOST` | `127.0.0.1` | HTTP bind host for health, metrics, and FreePBX snippets |
| `SIPFAX_OPERATOR_PORT` | `8080` | HTTP port for operator endpoints |
| `SIPFAX_FREEPBX_EXTENSION` | `faxmodem` | FreePBX route/extension label shown in the generated PJSIP snippet |
| `SIPFAX_MODEM_COMMAND` | unset | Optional external modem backend executable for lab/debug adapters; when unset SIPfax uses the in-process dial-up terminator |
| `SIPFAX_MODEM_ARGS` | unset | Comma-separated arguments passed to `SIPFAX_MODEM_COMMAND` when configured |
| `SIPFAX_PPP_USERS` | unset | Comma-separated `username:password` entries accepted by the PPP control path |
| `SIPFAX_PPP_POOL` | `10.64.0.0/24` | Client address pool; `.1` is reserved as the local peer by default |
| `SIPFAX_PPP_LOCAL_ADDRESS` | first host in pool | Local peer address advertised to authenticated clients |
| `SIPFAX_PPP_DNS` | `1.1.1.1,9.9.9.9` | DNS servers assigned to authenticated PPP clients |
| `SIPFAX_EGRESS_INTERFACE` | `wan0` | Outbound interface used when rendering NAT/firewall rules |
| `SIPFAX_EGRESS_ENABLED` | `true` | Set to `false` to disable internet forwarding |
| `SIPFAX_EGRESS_DNS` | `true` | Set to `false` to block client DNS egress |
| `SIPFAX_EGRESS_ALLOW` | `0.0.0.0/0` | Comma-separated destination CIDRs eligible for forwarding after default private/reserved blocks |

## Call Flow

1. Inbound `INVITE` is parsed from UDP SIP.
2. The service accepts only SDP offers with payload type `0` (`PCMU`) or `8`
   (`PCMA`) at 8 kHz.
3. If no other call is active, SIPfax sends `100 Trying`, `180 Ringing`, and a
   `200 OK` answer with the selected codec and local RTP port.
4. `ACK` marks the session established.
5. The PPP control path starts in `awaiting-auth`, accepts configured
   credentials, assigns a client address plus DNS, and records egress policy
   diagnostics for the active call.
6. RTP packets with the negotiated payload type are passed to the in-process
   dial-up terminator, which emits G.711 ANSam answer frames, detects inbound
   modem energy, advances into carrier training and PPP LCP probe audio, and
   records protocol state transitions in diagnostics/logs.
7. `BYE` tears down the PPP lease, RTP codec filter, and single-session slot.

## In-Process Dial-Up Terminator

SIPfax defaults to a server-local dial-up protocol terminator. It keeps the
media path constrained to negotiated G.711 payload bytes, emits an ANSam-style
answer signal with phase reversals, detects inbound modem energy from the remote
caller, and advances visible negotiation state from `answer-tone` to
`v8-training`, `carrier-training`, and `ppp-lcp-probe`. The current state,
inbound energy, frame counters, PPP probe counters, and thresholds are exposed
under `media.modem` in operator diagnostics.

The in-process terminator now gives live Windows dial-up testing a concrete
target beyond the previous `v8-training` plateau: sustained inbound carrier
energy should move diagnostics and logs to `carrier-training`, then outbound
G.711 PPP LCP probe frames should move the state to `ppp-lcp-probe`. It does not
require a modem binary or physical modem attached to the SIPfax VM.

## Optional External Modem Backend

`SIPFAX_MODEM_COMMAND` remains available for lab adapters. When configured, the
command is started when a call codec is selected and is stopped when the call is
torn down.

The process contract is intentionally narrow:

- stdin receives one G.711 RTP payload at a time, prefixed by a two-byte
  big-endian payload length.
- stdout must write outbound G.711 payloads using the same two-byte length
  prefix.
- SIPfax sets `SIPFAX_MODEM_CODEC`, `SIPFAX_MODEM_PAYLOAD_TYPE`, and
  `SIPFAX_MODEM_CLOCK_RATE` in the child environment for the active call.
- The backend must emit already-encoded `PCMU` or `PCMA`; SIPfax does not
  transcode or resample external backend frames.

## PPP and Egress Notes

SIPfax keeps PPP authentication, address assignment, and egress policy as
explicit session state in the service. The current modem bridge can call
`authenticatePpp(callId, { username, password })` once the downstream PPP stack
extracts PAP/CHAP credentials. Successful authentication returns the local peer
address, assigned client address, DNS servers, and egress diagnostics.

Controlled egress defaults to NAT on `SIPFAX_EGRESS_INTERFACE`, allows DNS, and
rejects private, loopback, link-local, documentation, multicast, and reserved
destinations before permitting internet forwarding. Operators can inspect
`EgressPolicy.firewallRules()` for the iptables/sysctl commands that match the
configured policy.

## FreePBX Integration

SIPfax should be connected to FreePBX as a narrow SIP trunk/extension target.
FreePBX routes the selected extension to SIPfax, but SIPfax remains the owner of
the modem media path after the call reaches it. Do not enable T.38, transcoding,
call recording, conferencing, VAD, comfort noise insertion, or other Asterisk
media features for this route.

For the first dedicated-VM deployment, follow
[deploy/README.md](deploy/README.md). That runbook covers the Debian VM
bootstrap, Node `24.x`, `/opt/sipfax`, `/etc/sipfax/sipfax.env`,
`sipfax.service`, firewall expectations, and verification commands. The LKMA-179
deployment decision is a new SIPfax VM on Proxmox `vmbr0` with FreePBX dial
string `12345678` routed to the SIPfax endpoint.

Recommended FreePBX shape:

1. Create or edit a PJSIP trunk that points at `SIPFAX_PUBLIC_HOST:SIPFAX_SIP_PORT`.
2. Allow only `ulaw` and `alaw`; keep all other codecs disabled.
3. Route the modem DID or internal extension directly to that trunk. For the
   first dedicated VM deployment, reserve internal dial string `12345678`.
4. Keep SIPfax reachable only from the PBX signaling network and the selected
   ATA/modem segment.
5. Leave SIPfax at one concurrent call. A second `INVITE` receives `486 Busy Here`.

The operator surface provides a generated PJSIP snippet:

```bash
curl http://127.0.0.1:8080/freepbx/pjsip.conf
```

Treat the snippet as the SIPfax-side baseline, then apply equivalent settings
through the FreePBX UI or include-file mechanism according to local policy.

## Operator Surface

The HTTP operator listener binds to `127.0.0.1:8080` by default. Keep it on
loopback or behind an authenticated internal proxy; it exposes live call and PPP
diagnostics intended for operators, not internet clients.

Endpoints:

- `GET /healthz` returns JSON readiness checks for SIP, RTP, configured PPP
  users, and single-session capacity.
- `GET /metrics` returns Prometheus text metrics for process health, active
  sessions, session limit, SIP `INVITE` outcomes, RTP accepted/dropped counts,
  configured PPP users, and active PPP leases.
- `GET /freepbx/pjsip.conf` returns the FreePBX/Asterisk PJSIP integration
  snippet for the configured SIP address and extension label.

Minimum alerting expectations:

- `sipfax_up == 0` for more than one scrape interval
- `sipfax_ppp_configured_users == 0`
- sustained growth in `sipfax_invites_total{outcome="rejected"}`
- sustained growth in `sipfax_rtp_dropped_total`
- `sipfax_active_sessions == sipfax_session_limit` outside expected fax windows

## Compatibility and Hardening

Supported baseline:

- Cisco ATA 191/192-class analog telephone adapter
- in-process dial-up terminator on the SIPfax VM
- optional external backend process only when explicitly configured with
  `SIPFAX_MODEM_COMMAND`
- G.711 `PCMU`/`PCMA` at 8 kHz only
- one live modem call at a time

Operator hardening checklist:

- Review `media.modem` diagnostics during live test calls; the dial-up state
  should leave `answer-tone` when inbound modem energy is detected.
- Set `SIPFAX_PPP_USERS`; an empty user list intentionally degrades health.
- Restrict UDP SIP and RTP ingress to the FreePBX/ATA network.
- Keep `SIPFAX_OPERATOR_HOST=127.0.0.1` unless an authenticated management
  network is in front of the service.
- Review `EgressPolicy.firewallRules()` before enabling internet forwarding on
  the production host.
- Disable FreePBX media features on the SIPfax route: no T.38, transcoding,
  recording, conferencing, VAD, or comfort noise.
- Pin ATA modem lines to G.711, disable echo cancellation and jitter-buffer
  features where the ATA permits, and verify the exact firmware in lab before
  production use.

## Verify

```bash
npm test
```

The current tests cover strict codec negotiation, single-session busy rejection,
ACK establishment, PPP auth/address/DNS assignment, controlled egress defaults,
BYE teardown, RTP payload filtering, operator health/metrics rendering, and the
FreePBX PJSIP snippet.
