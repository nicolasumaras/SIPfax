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
| `sipfax` | Dedicated service user and group |

Default listeners:

- SIP UDP `0.0.0.0:5060`
- RTP UDP `0.0.0.0:40000`
- Operator HTTP `127.0.0.1:8080`

Keep operator HTTP bound to loopback for the first deploy. Reach it over SSH
port forwarding when needed.

## Prerequisites

Start from a Debian 12 VM on `vmbr0` with a fixed LAN address reserved for
SIPfax. The FreePBX VM should be able to reach that IP on UDP `5060` and
`40000`.

Install base packages and Node `24.x`:

```bash
sudo apt-get update
sudo apt-get install -y ca-certificates curl gnupg git ufw
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
- `SIPFAX_MODEM_COMMAND`: executable for the external modem backend that
  bridges length-prefixed G.711 frames to the attached hardware modem service
- `SIPFAX_PPP_USERS`: one or more `username:password` entries
- `SIPFAX_EGRESS_INTERFACE`: the VM network interface used for outbound traffic

Keep `SIPFAX_OPERATOR_HOST=127.0.0.1` unless an authenticated management network
or proxy is added.

The modem backend command is part of the live call path. It must read
two-byte-length-prefixed G.711 payloads from stdin and write the same framed
format to stdout after driving the real modem negotiation and PPP data path. A
deployment that leaves `SIPFAX_MODEM_COMMAND` unset will fail fast at service
startup instead of answering calls with a synthetic tone loop.

## systemd Install

Install the unit and start the service:

```bash
sudo deploy/install-systemd.sh
sudo systemctl enable --now sipfax.service
sudo systemctl status sipfax.service
```

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
