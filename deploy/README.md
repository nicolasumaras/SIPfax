# SIPfax Dedicated VM First Deploy

This runbook bootstraps SIPfax on a new dedicated Debian VM attached to the
Proxmox `vmbr0` bridge. It follows the LKMA-179 deployment decision: SIPfax runs
outside the FreePBX VM, owns SIP/RTP after FreePBX routes the modem call, and
uses FreePBX dial string `12345678`.

## Target Layout

| Path | Purpose |
| --- | --- |
| `/opt/sipfax` | Git checkout of this repository |
| `/etc/sipfax/sipfax.env` | Runtime environment file |
| `/etc/systemd/system/sipfax.service` | systemd unit |
| `/var/log/sipfax` | Writable service log/artifact directory for modem captures |
| `sipfax` | Dedicated service user and group |

Default listeners:

- SIP UDP `0.0.0.0:5060`
- RTP UDP `0.0.0.0:40000`
- Operator HTTP `127.0.0.1:8080`

Keep operator HTTP bound to loopback for the first deploy. Reach it over SSH
port forwarding when needed.

## Lab Access

Manage the live SIPfax VM through Proxmox host `root@192.168.1.20` using the
shared `PROXMOX` runtime secret. Do not store the secret value in this repo.
The SIPfax guest is Proxmox VM `133` and has service IP `192.168.1.31`.
Direct SSH to `192.168.1.31` may reject the Paperclip environment's keys; use
the Proxmox host and guest agent when direct guest SSH is unavailable.

## Prerequisites

Start from a Debian 12 VM on `vmbr0` with a fixed LAN address reserved for
SIPfax. The FreePBX VM should be able to reach that IP on UDP `5060` and
`40000`.

Install base packages, Node `24.x`, PPP, and nftables:

```bash
sudo apt-get update
sudo apt-get install -y ca-certificates curl gnupg git ufw ppp nftables
curl -fsSL https://deb.nodesource.com/setup_24.x | sudo -E bash -
sudo apt-get install -y nodejs
node --version
npm --version
```

Create the service user and checkout path:

```bash
sudo useradd --system --home-dir /opt/sipfax --shell /usr/sbin/nologin sipfax
sudo git clone <repo-url> /opt/sipfax
sudo chown -R sipfax:sipfax /opt/sipfax
cd /opt/sipfax
sudo -u sipfax npm ci --omit=dev
```

For the first live deploy, use the commit or release branch selected by the CTO
rather than an unreviewed local working tree.

## Runtime Environment

Install the environment template and edit every placeholder:

```bash
sudo install -d -m 0750 -o root -g sipfax /etc/sipfax
sudo install -m 0640 -o root -g sipfax deploy/sipfax.env.example /etc/sipfax/sipfax.env
sudo editor /etc/sipfax/sipfax.env
```

Required first-deploy values:

- `SIPFAX_PUBLIC_HOST`: the dedicated SIPfax VM IP on `vmbr0`
- `SIPFAX_FREEPBX_EXTENSION`: `12345678`
- `SIPFAX_PPP_USERS`: one or more `username:password` entries
- `SIPFAX_PPPD_COMMAND`: path to `pppd` from the Debian `ppp` package
- `SIPFAX_EGRESS_INTERFACE`: the VM network interface used for outbound traffic

Keep `SIPFAX_OPERATOR_HOST=127.0.0.1` unless an authenticated management network
or proxy is added.

The default live call path uses the spandsp soft-modem worker at
`/opt/sipfax/bin/sipfax-softmodem`. Set `SIPFAX_SOFTMODEM_BINARY` only when the
worker is installed somewhere else. Set `SIPFAX_MODEM_COMMAND` only when
intentionally replacing the default worker with another external adapter.

The selected worker must read two-byte-length-prefixed G.711 payloads from stdin
and write the same framed format to stdout. fd 3 may write JSON-line control
snapshots. During a live Windows dial-up attempt, `/healthz` and operator
diagnostics should show `media.modem.type` as `external-process` and expose
`media.modem.modulation`, `baud`, `state`, `ber`, `framesIn`, `framesOut`,
`lastEvent`, and `lastEventAt`. The live path should report real worker state,
not synthetic `answer-tone` or `v8-training` diagnostics.

For Phase 5a, the default worker advertises both `V.21` and `V.22bis` in V.8.
Windows callers that offer V.22/V.22bis should drive
`media.modem.modulation: "V.22bis"` and `media.modem.baud: 2400`. If V.8 is not
offered or fails, the worker falls back to `V.21` at 300 bit/s.

If the worker writes decoded data or capture artifacts under `/var/log/sipfax`,
keep the shipped unit's `ReadWritePaths=/var/cache/sipfax /var/log/sipfax`
entry intact so `ProtectSystem=strict` does not make the artifact path
read-only.

Install `ppp` and `nftables` on the SIPfax VM. When the modem worker emits a
`pty-opened` control event with `slavePath`, SIPfax starts `pppd` on that pty with
`nodetach`, `nodefaultroute`, `noccp`, `require-chap` by default, the leased
local/client address pair, configured `ms-dns` values, MTU 1500, and high-latency
LCP/IPCP retry settings. `SIPFAX_PPP_AUTH=pap` switches the required auth mode
for legacy clients. If `SIPFAX_PPP_NOTIFY_SCRIPT` is set, the script is used for
pppd `ip-up-script` and `ip-down-script`; emit JSON lines such as
`{"state":"IPCP-open","interfaceName":"ppp0"}` so operator diagnostics can show
`ppp.state`, peer addresses, DNS servers, interface, and session duration.

SIPfax writes a per-call PPP egress descriptor under
`/run/sipfax/ppp-leases/<call-id>.json` before `pppd` starts. The descriptor
contains the rendered nftables and iptables-nft fallback rules from
`EgressPolicy`. SIPfax itself runs as the unprivileged `sipfax` user and does
not write firewall state. The root-side pppd hooks installed by
`deploy/install-systemd.sh` are the only path that enables forwarding, applies
NAT/MASQUERADE, rolls rules back on `ip-down`, and posts best-effort loopback
diagnostics to operator HTTP.

## systemd Install

Install the unit and start the service:

```bash
sudo deploy/install-systemd.sh
sudo systemctl enable --now sipfax.service
sudo systemctl status sipfax.service
```

`deploy/install-systemd.sh` also installs:

- `/usr/lib/sipfax/sipfax-egress-apply`
- `/etc/ppp/ip-up.d/sipfax-egress`
- `/etc/ppp/ip-down.d/sipfax-egress`

Operator review is required before enabling the hooks on a production VM,
because they write nftables/iptables state and toggle IPv4 forwarding while a
PPP lease is active.

The shipped unit leaves `MemoryDenyWriteExecute=false` because Node `24.x`/V8
requires executable anonymous memory during runtime startup. Do not add a live
drop-in override for this setting; keep the repository unit as the source of
truth so fresh Debian 12 deployments start cleanly.

Useful service commands:

```bash
sudo systemctl restart sipfax.service
sudo journalctl -u sipfax.service -f
sudo deploy/sipfaxctl status
sudo deploy/sipfaxctl logs
```

## Firewall Expectations

Restrict SIP and RTP ingress to the FreePBX IP. Replace `<freepbx-ip>` with the
LAN address of VM `107` and keep SSH limited to the normal admin network.

With `ufw`:

```bash
sudo ufw default deny incoming
sudo ufw allow from <admin-cidr> to any port 22 proto tcp
sudo ufw allow from <freepbx-ip> to any port 5060 proto udp
sudo ufw allow from <freepbx-ip> to any port 40000 proto udp
sudo ufw enable
sudo ufw status verbose
```

Do not expose operator HTTP port `8080` to the LAN during the first deploy. Use:

```bash
ssh -L 8080:127.0.0.1:8080 <admin>@<sipfax-vm-ip>
```

Then query `http://127.0.0.1:8080` locally.

PPP egress is applied by nftables when `nft` is available. The helper falls back
to `iptables-nft` for systems where nftables is not present. On `ip-up`, the
helper enables `net.ipv4.ip_forward=1` and
`net.ipv4.conf.<SIPFAX_EGRESS_INTERFACE>.forwarding=1`, then applies the
per-call ruleset. On `ip-down`, it removes the per-call ruleset and disables
forwarding after the last active SIPfax PPP lease is gone.

## Verification

On the SIPfax VM:

```bash
systemctl is-active sipfax.service
curl -fsS http://127.0.0.1:8080/healthz
curl -fsS http://127.0.0.1:8080/metrics
curl -fsS http://127.0.0.1:8080/freepbx/pjsip.conf
sudo ss -lunp | grep -E ':(5060|40000) '
```

Expected `/healthz` result is `status: ok`. If it is `degraded`, confirm
`SIPFAX_PPP_USERS` is set and restart the service.

Expected modem diagnostics for the soft-modem worker path:

```bash
curl -fsS http://127.0.0.1:8080/healthz | jq '.media.modem // .ppp'
journalctl -u sipfax.service -f | grep 'modem backend'
```

For the LKMA-196 path, a live Windows dial-up attempt should show real worker
modulation, for example `media.modem.modulation` as `V.21`, and frame counters
increasing from the worker control stream.

## Real Windows Lab Run

Use this path for the Phase 5a lab validation. The target is a real Windows
computer using its Windows dial-up networking stack, not a Windows VM demo.

Prerequisites:

- SIPfax VM `192.168.1.31` has this repository deployed, `npm ci --omit=dev`
  completed, and `vendor/sipfax-softmodem/sipfax-softmodem` built with
  `libspandsp-dev`.
- `/etc/sipfax/sipfax.env` sets `SIPFAX_PUBLIC_HOST=192.168.1.31`,
  `SIPFAX_MODEM_COMMAND=/opt/sipfax/vendor/sipfax-softmodem/sipfax-softmodem`,
  `SIPFAX_PPPD_COMMAND=/usr/sbin/pppd`, `SIPFAX_PPP_USERS`, and
  `SIPFAX_EGRESS_INTERFACE`.
- FreePBX routes dial string `12345678` directly to `192.168.1.31:5060` with
  only PCMU/PCMA enabled and media features such as T.38, VAD, recording, and
  transcoding disabled.
- The real Windows computer is connected through the lab analog path into the
  ATA/FreePBX route and has a dial-up profile that dials `12345678` with the
  SIPfax PPP username and password.

Wiring:

```text
Windows computer modem -> analog line/ATA -> FreePBX route 12345678
FreePBX SIP/RTP -> SIPfax VM 192.168.1.31 UDP 5060/40000
SIPfax soft-modem pty -> pppd -> ppp0 -> egress interface
```

Before dialing, watch the service and operator state:

```bash
sudo journalctl -u sipfax.service -f
watch -n1 "curl -fsS http://127.0.0.1:8080/healthz | jq '{modem: .media.modem, ppp: .ppp}'"
```

Start the Windows dial-up connection. A successful modem and PPP attach should
show all of the following on `192.168.1.31`:

```bash
curl -fsS http://127.0.0.1:8080/healthz | jq '.media.modem.modulation, .media.modem.state, .ppp.state, .ppp.interfaceName'
journalctl -u sipfax.service --since "5 minutes ago" | grep -E 'pty-opened|IPCP-open|ppp0'
ip addr show ppp0
```

Expected results:

- `media.modem.state` becomes `data-mode`.
- `media.modem.modulation` is `V.22bis` when Windows offers V.22/V.22bis, or
  `V.21` only when the call falls back.
- `ppp.state` becomes `ipcp-open`.
- `ppp.interfaceName` is `ppp0` and `ip addr show ppp0` shows the leased local
  and Windows peer addresses.

Verify IP traffic from the Windows side by opening Command Prompt after the
dial-up connection reports connected:

```cmd
ipconfig
ping <sipfax-ppp-local-address>
ping 8.8.8.8
```

If public ping is disabled by the remote network, verify egress from the SIPfax
VM while the call is connected:

```bash
sudo tcpdump -ni ppp0 icmp
sudo nft list ruleset | grep sipfax_
```

After disconnect, confirm cleanup:

```bash
ip link show ppp0
sudo nft list ruleset | grep sipfax_ || true
curl -fsS http://127.0.0.1:8080/healthz | jq '.ppp'
```

For PPP egress, an authenticated Linux client should be able to reach a public
HTTP destination with `curl --interface ppp0 <url>`. After disconnect, confirm
that `sudo nft list ruleset | grep sipfax_` no longer shows the call-specific
table and forwarding is disabled when no other SIPfax PPP lease is active.

From the FreePBX side:

1. Create a dedicated PJSIP trunk or endpoint that targets
   `<sipfax-vm-ip>:5060`.
2. Allow only `ulaw` and `alaw`.
3. Disable T.38, transcoding, recording, conferencing, VAD, comfort noise, and
   other media features for this route.
4. Route dial string `12345678` directly to the SIPfax endpoint.
5. Place a call to `12345678` and verify SIP/RTP arrive at the SIPfax VM.

SIPfax remains intentionally single-call. A second simultaneous call should
receive `486 Busy Here`.
