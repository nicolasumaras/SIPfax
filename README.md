# SIPfax

SIPfax is a direct SIP/RTP service skeleton for carrying dial-up modem traffic
over a tightly constrained G.711 pass-through path.

The first supported baseline follows the LKMA-168 decision:

- SIPfax owns SIP dialog state and RTP once a call is routed to it
- `PCMU` or `PCMA` only
- one inbound call session at a time
- no transcoding, T.38, VAD, comfort noise, conferencing, recording, or Asterisk
  media side-channel in the live modem path
- Cisco ATA 191/192-class ATA and external hardware serial modem baseline

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

## Call Flow

1. Inbound `INVITE` is parsed from UDP SIP.
2. The service accepts only SDP offers with payload type `0` (`PCMU`) or `8`
   (`PCMA`) at 8 kHz.
3. If no other call is active, SIPfax sends `100 Trying`, `180 Ringing`, and a
   `200 OK` answer with the selected codec and local RTP port.
4. `ACK` marks the session established.
5. RTP packets with the negotiated payload type are passed to the modem bridge
   placeholder without decoding or transcoding.
6. `BYE` tears down the session and frees the single-session slot.

## Verify

```bash
npm test
```

The current tests cover strict codec negotiation, single-session busy rejection,
ACK establishment, BYE teardown, and RTP payload filtering.
