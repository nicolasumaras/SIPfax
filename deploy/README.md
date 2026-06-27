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

Install base packages, Node `24.x`, PPP, nftables, and the SpanDSP worker
runtime/build dependencies on Debian Bookworm:

```bash
sudo apt-get update
sudo apt-get install -y ca-certificates curl gnupg git jq ufw \
  build-essential pkg-config python3 libspandsp-dev libspandsp2 \
  ppp nftables iptables
curl -fsSL https://deb.nodesource.com/setup_24.x | sudo -E bash -
sudo apt-get install -y nodejs
node --version
npm --version
pppd --version
```

`libspandsp2` is the runtime shared library for the soft-modem worker.
`libspandsp-dev`, `build-essential`, `pkg-config`, and `python3` are needed
when building the vendored worker on the VM. `iptables` provides the
`iptables-nft` fallback used only when `nft` is unavailable.

### Optional: higher-speed modem engine (V.32bis / V.34, slmodem datapump)

The default engine is the spandsp worker (V.21 / V.22bis). To enable the
higher-speed engine (`SIPFAX_MODEM_ENGINE=slmodem`), the SmartLink `slmodemd`
datapump is required. It is a **32-bit x86** binary object (`dsplibs.o`), so the
build host needs multilib:

```bash
sudo apt-get install -y gcc-multilib libc6-dev-i386
./vendor/slmodem/fetch.sh          # stages slmodemd/ + dsplibs.o, verifies sha256
make -C vendor/slmodem/slmodemd    # 32-bit slmodemd
make -C vendor/slmodem-bridge      # the SIPfax<->slmodemd audio bridge
```

`dsplibs.o` is **gratis and redistributable but closed-source** (Debian "non-free"
= not open-licensed, not paid). Provenance and the pinned SHA-256 are recorded in
`vendor/slmodem/README.md`. The engine runs as a 32-bit process; the rest of
SIPfax stays 64-bit. The default `spandsp` engine needs none of this.

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

Build the bundled SpanDSP worker and install it into SIPfax's default handoff
path:

```bash
sudo -u sipfax make -C vendor/sipfax-softmodem
sudo install -d -m 0755 -o root -g sipfax /opt/sipfax/bin
sudo install -m 0755 -o root -g sipfax \
  vendor/sipfax-softmodem/sipfax-softmodem \
  /opt/sipfax/bin/sipfax-softmodem
test -x /opt/sipfax/bin/sipfax-softmodem
```

The service user only needs execute access to
`/opt/sipfax/bin/sipfax-softmodem`; keep the installed binary owned by root so
normal service runtime cannot replace it.

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
`startMode`, `v8Status`, `v8Modulations`, `lastEvent`, and `lastEventAt`. The
live path should report real worker state, not synthetic `answer-tone` or
`v8-training` diagnostics.

For Phase 5a, the default worker advertises both `V.21` and `V.22bis` in V.8.
Windows callers that offer V.22/V.22bis should drive
`media.modem.modulation: "V.22bis"` and `media.modem.baud: 2400`. If V.8 is not
offered or fails, the worker falls back to `V.21` at 300 bit/s.

Leave `SIPFAX_MODEM_START_MODE` unset or set to `v8` for normal calls. If a live
Windows call repeatedly ends at `media.modem.lastEvent:
"v8-failed-v21-fallback"` with `media.modem.v8Status: "failed"`, use a controlled
fallback run to bypass V.8 and start the worker directly in V.22bis answer mode:

```bash
sudo sed -i '/^SIPFAX_MODEM_START_MODE=/d' /etc/sipfax/sipfax.env
echo 'SIPFAX_MODEM_START_MODE=v22bis' | sudo tee -a /etc/sipfax/sipfax.env
sudo systemctl restart sipfax.service
curl -fsS http://127.0.0.1:8080/healthz | jq '.media.modem.startMode'
```

After the fallback run, restore `SIPFAX_MODEM_START_MODE=v8` or remove the line
so the next validation uses standards negotiation again. If `v22bis` progresses
to `v22bis-carrier-up` or `IPCP-open`, the remaining stop-point is V.8
negotiation. If it still cannot train, the next owner should inspect the live
analog/RTP audio path before PPP.

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

For each PPP session, SIPfax renders the configured `SIPFAX_PPP_USERS` into a
private temporary `chap-secrets` file, or `pap-secrets` when
`SIPFAX_PPP_AUTH=pap`. The file is created with `0600` permissions, passed to
`pppd` with the matching `chap-secrets`/`pap-secrets` option, and removed when
the pppd session exits. Do not create persistent entries in `/etc/ppp/chap-secrets`
for SIPfax users unless an Operator explicitly chooses to replace this per-call
secret lifecycle.

## systemd Install

Install the unit and start the service:

```bash
sudo deploy/install-systemd.sh
sudo systemctl enable --now sipfax.service
sudo systemctl status sipfax.service
```

`deploy/install-systemd.sh` also installs:

- `/opt/sipfax/bin/sipfax-softmodem`
- `/usr/lib/sipfax/sipfax-egress-apply`
- `/etc/ppp/ip-up.d/sipfax`
- `/etc/ppp/ip-down.d/sipfax`

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

## PPP Runtime Requirements

PPP only works once the host can actually run `pppd` and create a `ppp0`
interface. Three things the base VM does not provide by default:

1. **The `ppp` package and a kernel with PPP support.** `apt-get install -y ppp`
   provides `/usr/sbin/pppd` (setuid-root, group `dip`). The Debian **cloud**
   kernel ships **no PPP modules** — `/dev/ppp` will be missing. Install the
   generic kernel and boot it:

   ```bash
   sudo apt-get install -y ppp linux-image-amd64
   echo ppp_generic | sudo tee /etc/modules-load.d/ppp.conf
   # ensure GRUB boots the generic (non-cloud) kernel, then reboot
   uname -r            # expect e.g. 6.1.0-NN-amd64 (not -cloud-amd64)
   ls -l /dev/ppp      # must exist
   ```

2. **A systemd drop-in so the unprivileged service can run setuid pppd.** The
   shipped unit's hardening blocks it. Install
   [`sipfax.service.d/ppp.conf`](sipfax.service.d/ppp.conf):

   ```bash
   sudo install -D -m 0644 deploy/sipfax.service.d/ppp.conf \
     /etc/systemd/system/sipfax.service.d/ppp.conf
   sudo systemctl daemon-reload && sudo systemctl restart sipfax.service
   # verify: NoNewPrivs must be 0 on the running process
   grep NoNewPrivs /proc/$(systemctl show -p MainPID --value sipfax.service)/status
   ```

3. **Secrets files the service can rewrite.** `pppd` always reads
   `/etc/ppp/chap-secrets`; the service renders per-call credentials there and
   clears them on teardown, so pre-create them owned by `sipfax`:

   ```bash
   sudo touch /etc/ppp/chap-secrets /etc/ppp/pap-secrets
   sudo chown sipfax:sipfax /etc/ppp/chap-secrets /etc/ppp/pap-secrets
   sudo chmod 600 /etc/ppp/chap-secrets /etc/ppp/pap-secrets
   ```

## Modulation Note (V.8 vs forced V.22bis)

Set `SIPFAX_MODEM_START_MODE=v22bis` for the live service. With real Windows
dial-up modems, standards **V.8 negotiation selects V.22bis but the modem then
fails to complete V.22bis training** (it sits on an unscrambled carrier while
the answerer trains) — a timing/interop quirk that does not reproduce in a
spandsp-to-spandsp loopback (see
[`tests/v8handoff_test.c`](../vendor/sipfax-softmodem/tests/v8handoff_test.c)).
**Forcing V.22bis** presents a continuous answer carrier the modem locks onto
and trains reliably. The V.8 handoff code itself is correct (the loopback test
passes); fixing the real-modem path needs lab tuning against the physical modem.

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
systemctl status sipfax.service
curl -fsS http://127.0.0.1:8080/healthz
pppd --version
sudo nft list ruleset | head
curl -fsS http://127.0.0.1:8080/metrics
curl -fsS http://127.0.0.1:8080/freepbx/pjsip.conf
sudo ss -lunp | grep -E ':(5060|40000) '
```

Expected `/healthz` result is `status: ok`. If it is `degraded`, confirm
`SIPFAX_PPP_USERS` is set and restart the service.

Expected modem diagnostics for the soft-modem worker path, including the
control-fd snapshot exposed under `media.modem`:

```bash
curl -fsS http://127.0.0.1:8080/healthz | jq '.media.modem'
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
- `media.modem.v8Status` remains `null` only for a deliberate
  `SIPFAX_MODEM_START_MODE=v22bis` fallback run; otherwise it records the V.8
  result that selected or rejected V.22bis.
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
