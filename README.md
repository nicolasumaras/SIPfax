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
- spandsp soft-modem worker on the SIPfax VM; no physical modem is required on
  the server

## Experimental V.90 backend

The native C backend in `vendor/linmodem` answers Windows XP hardware modems
through a Cisco ATA187 and FreePBX and provides authenticated PPP internet access.
CT105 retains the verified `9be7827` baseline while later receiver changes are
qualified. This hardware path has passed sustained PPP transfers at 49.333 kbit/s
downstream and 26.4/28.8 kbit/s upstream on earlier profiles.

The development receiver implements 3000-symbol/s upstream profiles from 4.8 to
28.8 kbit/s and 3200-symbol/s profiles through 31.2 kbit/s. Startup follows the
caller's carrier capabilities and preserves the selected profile through training,
E replay and rate renegotiation. Causal ODP-assisted training with index-2
pre-emphasis started 3000/28.8 without fallback in two trials, but a later repeat
needed recovery to 26.4. The sustained hardware trial
passed all 129 transfer checks on one connection, but recorded 19 CRC/two alignment
errors and 66 retransmitted TCP segments over 672.717 seconds. The initial short
trial was error-free; sustained quality still needs improvement. The optional
3200/31.2 profile remains unqualified. Maximum rates are not guaranteed.

Startup now retries one 2.4 kbit/s rate step lower if B1 is detected but no valid
LCP Configure packet arrives within ten seconds plus two round-trip delays. It preserves the
reduced ceiling through retraining, stops at 4.8 kbit/s, and does not apply after
LCP startup has been recognized on the call. FCS matches alone do not disable recovery. One hardware call successfully recovered from
3000/28.8 to 3000/26.4 on the same call. A sustained recovery call also passed 129 transfer checks; a post-deployment call
verified recovery with normal symbol-rate negotiation.
See [receiver qualification](research/v90/upstream-rates.md) and the
[live development record](research/v90-live-status.md) for evidence and remaining
work. These results do not imply full V.90 conformance or concurrent-call support.

The native backend passively monitors PPP echo traffic. After a matching reply
has demonstrated peer support, repeated unanswered requests can trigger one
full retrain before PPP times out. It waits at least 40 seconds from the first
unanswered request and 10 seconds from the second counted request. Training
pauses this observer; another recovery requires a matching reply to rearm it.
This recovery mechanism does not diagnose or eliminate the underlying impairment.

Build on Linux with GCC and make:

```bash
make -C vendor/linmodem CFLAGS='-O2 -Wall -g -D_GNU_SOURCE -fcommon'
```

For a newly created configuration, set `SIPFAX_MODEM_ENGINE=linmodem`.
For an existing configuration, set `modem.engine` to `linmodem`,
`modem.modulation` to `v90`, and `modem.command` to `null` to select its launcher.
The default launcher is `/opt/sipfax/bin/sipfax-linmodem`;
`SIPFAX_LINMODEM_BINARY` can override that location. An explicit stored
`modem.command` still takes precedence, including existing capture wrappers.
The launcher uses the built `vendor/linmodem/lm` alongside the repository;
it does not enable private audio capture. The normal SIPfax PPP configuration,
G.711 codec negotiation and per-call backend lifecycle still apply.

Hardware qualification uses these service environment settings:

| Variable | Default | Current test setting |
| --- | --- | --- |
| `SIPFAX_V90_MAX_BPS` | `56000` | `49334` downstream ceiling |
| `SIPFAX_V90_INITIAL_TRN2D_MS` | `255` | `1500` initial final-training interval |
| `SIPFAX_V90_RENEG_TRN2D_MS` | `255` | `1500` rate-renegotiation training interval |
| `SIPFAX_V90_UPSTREAM_RATE` | `4800` | `28800` initial upstream ceiling |
| `SIPFAX_V90_UPSTREAM_SYMBOL_RATE` | both supported rates | unset in deployment; `3000` or `3200` restricts qualification offers |
| `SIPFAX_V90_PREEMPHASIS_3000` | `0` (flat) | `2` in the successful 3000/28800 trial |
| `SIPFAX_V90_ODP_TRAINING` | off | `1` enables experimental detection-pattern equalizer training |
| `SIPFAX_V90_LINE_ECHO` | off | `auto` initial echo-delay acquisition |
| `SIPFAX_V90_SOFT_RX` | `0` | `1` soft-decision 4.8 kbit/s path |
| `SIPFAX_RTP_PLAYOUT_MS` | `0` | `60` |
| `SIPFAX_PPP_UPSTREAM_TCP_MSS` | disabled | `536` |

The initial training setting accepts 255–2000 ms, rounded down to a complete
six-sample frame; invalid values use the default. The independent renegotiation
setting accepts 0–2000 ms with the same rounding; it defaults to 255 ms.
The symbol-rate restriction accepts `3000` or `3200`; other values allow both
implemented rates. Offers still respect the caller’s carrier capabilities.
The 3000-symbol pre-emphasis option accepts the exact decimal strings `0` through
`10`; invalid values select flat pre-emphasis. It affects only offered
3000-symbol profiles and is retained through retrains. ODP training is enabled
only by the exact value `1`; it predicts equalizer targets from validated V.42
detection traffic and does not implement modem error correction. Automatic
echo mode acquires an initial delay; continuous delay tracking remains unfinished.
Service environment changes require a restart
when no call is active. A stored `modem.command` takes precedence over its
environment seed. The native code retains its GPL-2.0 licensing.

`SIPFAX_V90_SOFT_RX=1` selects soft decisions for the legacy 4.8 kbit/s,
3200-symbol/s path. The other implemented profiles use the streaming trellis/shell
receiver, with B1 acquisition, carrier/equalizer fitting and adaptive symbol timing.
Profile and waveform regressions include fractional timing, clock drift, mu-law,
interference and delayed-E replay. Hardware qualification remains narrower than
synthetic profile coverage.

For the experimental 4.8 kbit/s upstream, `SIPFAX_PPP_UPSTREAM_TCP_MSS=536`
optionally limits TCP segment-size advertisements sent to IPv4 PPP clients.
This reduced a 4 KB request from 16.7 to about 10.2 seconds in controlled tests.
It does not change advertisements sent by the client or increase smaller MSS
values. The setting accepts 256–1460 and is disabled when unset. It requires
the installed PPP egress helper and takes effect on new calls after restart;
per-lease cleanup removes the rules. It does not fix modem training failures.

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
| `SIPFAX_SOFTMODEM_BINARY` | `/opt/sipfax/bin/sipfax-softmodem` | Default spandsp soft-modem worker executable |
| `SIPFAX_MODEM_COMMAND` | unset | Optional external modem backend executable; when set it overrides `SIPFAX_SOFTMODEM_BINARY` |
| `SIPFAX_MODEM_ARGS` | unset | Comma-separated arguments passed to the selected modem command |
| `SIPFAX_PPP_USERS` | unset | Comma-separated `username:password` entries accepted by the PPP control path |
| `SIPFAX_PPP_POOL` | `10.64.0.0/24` | Client address pool; `.1` is reserved as the local peer by default |
| `SIPFAX_PPP_LOCAL_ADDRESS` | first host in pool | Local peer address advertised to authenticated clients |
| `SIPFAX_PPP_DNS` | `1.1.1.1,9.9.9.9` | DNS servers assigned to authenticated PPP clients |
| `SIPFAX_PPPD_COMMAND` | `/usr/sbin/pppd` | `pppd` binary from the Debian `ppp` package |
| `SIPFAX_PPP_AUTH` | `chap` | `chap` by default; set `pap` only for legacy clients |
| `SIPFAX_PPP_NOTIFY_SCRIPT` | unset | Optional pppd ip-up/ip-down notifier that emits JSON IPCP events |
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
6. RTP packets with the negotiated payload type are passed to the spandsp
   soft-modem worker, which emits already-encoded G.711 modem audio and reports
   modulation state through the control fd.
7. `BYE` tears down the PPP lease, RTP codec filter, and single-session slot.

## Soft-Modem Worker

SIPfax defaults to an external spandsp soft-modem worker at
`/opt/sipfax/bin/sipfax-softmodem`. Override the default path with
`SIPFAX_SOFTMODEM_BINARY`, or set `SIPFAX_MODEM_COMMAND` to replace it with a
different external adapter. The worker is started when a call codec is selected
and is stopped when the call is torn down. If the binary is unavailable, SIPfax
logs the backend error and emits no synthetic modem audio.

The old in-process dial-up terminator remains available to tests as a fixture,
but it is no longer wired into live server startup.

The process contract is intentionally narrow:

- stdin receives one G.711 RTP payload at a time, prefixed by a two-byte
  big-endian payload length.
- stdout must write outbound G.711 payloads using the same two-byte length
  prefix.
- SIPfax sets `SIPFAX_MODEM_CODEC`, `SIPFAX_MODEM_PAYLOAD_TYPE`, and
  `SIPFAX_MODEM_CLOCK_RATE` in the child environment for the active call.
- fd 3 may emit JSON-line control snapshots. Operator diagnostics expose
  `media.modem.modulation`, `baud`, `state`, `ber`, `framesIn`, `framesOut`,
  `lastEvent`, and `lastEventAt`.
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
- spandsp soft-modem worker on the SIPfax VM
- optional external backend process override with `SIPFAX_MODEM_COMMAND`
- G.711 `PCMU`/`PCMA` at 8 kHz only
- one live modem call at a time

Operator hardening checklist:

- Review `media.modem` diagnostics during live test calls; modulation should
  report the worker-selected mode, for example `V.21`, not synthetic
  `answer-tone` or `v8-training` states.
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
