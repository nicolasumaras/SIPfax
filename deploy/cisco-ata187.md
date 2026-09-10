# Cisco ATA187 modem pass-through

Verified on ATA187 firmware `ATA187.9-2-3-1` with FreePBX/Asterisk PJSIP.
G.711 codec selection alone does not put this ATA into modem data mode.
Its default fax relay setting left the DSP in voice mode on the V.90 answer
tone. Add this to the root `device` element of each port's provisioning XML:

```xml
<vendorConfig>
  <faxMode>1</faxMode>
</vendorConfig>
```

Merge into an existing `vendorConfig` if present. Back up the XML first and
change its `versionStamp` so the ATA applies the update. Each port fetches its
own file: for MAC `5C5015A8DBAB` these were `ATA5C5015A8DBAB.cnf.xml` and
`ATA5015A8DBAB01.cnf.xml`. Preserve the existing SIP accounts and dial plan.

For a remote reload, the tested Asterisk PJSIP NOTIFY definition is:

```ini
[sipfax-ata-service-restart]
Event=>service-control
Subscription-State=>active
Content-Type=>text/plain
Content=>action=restart
```

Place it in `sip_notify_custom.conf` (included by FreePBX's generated
`pjsip_notify.conf`), reload `res_pjsip_notify.so`, then send this NOTIFY to
the particular ATA endpoint while both ports are idle. This restarts the ATA's
services and re-fetches both XML files. The older `Event: check-sync` request
did not apply the configuration in this test. Asterisk accepting a NOTIFY
command is not sufficient proof that the ATA reloaded.

Verify using the ATA web UI's **Console Logs → FoxCliLog**, or its read-only
`/FoxCliLog` endpoint. After provisioning and a new call, the tested device
reported `faxMode: PASSTHRU` and, on `2100-PR-Net`, `[Voice->VBD]` with
`Calling PR EC VAD DTMF DC` flags. Before this setting it reported
`faxMode: FAX_RELAY` and remained `[Voice->Voice]`.

Keep G.711 μ-law end-to-end for the current experimental SIPfax V.90 server.
This ATA setting fixes the observed media-mode configuration; it does not
establish that SIPfax's V.90 training, sustained transfers or internet access
are reliable. Those require separate hardware acceptance tests.

Cisco documents the distinction between fax pass-through (including modem
traffic) and T.38 relay in the [ATA187 configuration guide](https://www.cisco.com/c/en/us/td/docs/voice_ip_comm/cata/187/1_0/english/administration/guide/sip/187adm80/a187_ag3conf.pdf).
The XML value and reload procedure above were verified on the deployed device.

## Preparing each development call

Firmware 9.2.3.1 has been observed to discard diagnostic coding-profile changes
between calls/while idle. XML `faxMode` alone does not retain the tested playout
settings. `ata187-prepare.py` provides an optional lab-specific call-setup helper:

- G.711 μ-law profile 2: fixed 80 ms nominal/minimum playout, VAD/EC/tone detection
  off, and packet-loss concealment `NONE`.
- Read the current profile first; write only if it differs, require an `OK` for
  every setting, and check all seven values afterward.
- Issue `activate` before answering, even when the pending profile matches.
  `show coding` does not prove the DSP uses that profile: an explicitly activated
  40 ms baseline stayed at 45 ms actual playout after an unactivated 80 ms update.
  Activation rebuilds both FXS channels and must not overlap another ATA call.
- Serialize preparations with a file lock; cap the whole exchange at 18 seconds.
  Log only status/timing and exception type, never credentials or console output.

This is an experimental workaround for the identified ATA/firmware, not a
persistent firmware configuration mechanism. A 64-file, 2 MiB PPP download test
completed in 12 minutes 46 seconds with concealment disabled, despite 34 ATA
reported losses. The line renegotiated from 53.333 to 49.333 kbps; sustained
maximum-rate performance and general multi-call operation remain unproven.

The tested deployment runs the helper under a dedicated `sipfax-ata` account
on CT105; it does not need root. Install Python 3 and a separate virtual
environment with `paramiko==3.5.1` (this firmware uses an old DSA SSH host key).
The root-owned script is `/opt/sipfax/deploy/ata187-prepare.py`; the environment
is `/opt/sipfax-ata-venv`. Provision these files outside Git:

- `/etc/sipfax/ata187.json`: `host`, `username`, `password`; owned by root, readable
  only by the dedicated account/group (mode 0640 in a restricted directory).
- `/etc/sipfax/ata187_known_hosts`: the independently verified ATA host key.
  Unknown or changed host keys are rejected.
- `/var/lib/sipfax-ata/`: writable only by the dedicated account, including the
  `prepare.lock` file and its SSH `authorized_keys`.

Give FreePBX's `asterisk` user a dedicated SSH key. On CT105 restrict that key to
FreePBX's source address and the fixed command below; disable forwarding and PTY:

```text
restrict,from="192.168.1.29",command="/opt/sipfax-ata-venv/bin/python /opt/sipfax/deploy/ata187-prepare.py" ssh-ed25519 PUBLIC_KEY
```

Install `ata187-prepare-freepbx.sh` as root-owned, executable
`/usr/local/bin/sipfax-ata-prepare` on FreePBX. Adjust its fixed addresses and
paths for other labs; pin CT105's verified host key in the indicated known-hosts
file. Test it as `asterisk` while the ATA is idle before enabling the route.
The wrapper rejects multiple ATA channels and any already-answered ATA channel.
The wrapper accepts no caller-controlled command parameters.

Merge `ata187-freepbx-route.conf.example` into `extensions_custom.conf`, replacing
only the existing SIPfax extension. Preserve unrelated routes and back up the
file first. Reload the dialplan and inspect the resulting extension. The sample
selects the two lab endpoints by channel name, keeps an Asterisk `TRYLOCK` for the
entire caller-channel lifetime, and returns congestion if preparation fails.
Other callers skip the ATA-specific preparation. This locks project calls from
these endpoints; it is not a general policy for every possible call on the ATA.
Use the ATA exclusively for this single-call development route while this hook
is enabled: unrelated inbound/outbound routes do not share its lock and could
start a call after the wrapper's channel check. Future multi-call support needs
call admission covering both ports or a configuration path that avoids a global
DSP rebuild.

Inspect `journalctl -t sipfax-ata-prepare` on CT105 for preparation outcomes.
To roll back, restore the previous SIPfax extension and reload the dialplan;
then remove the dedicated key/helper account if no longer needed. This does
not itself restore ATA firmware settings; a service restart resets those.

Run helper regression tests with:

```sh
python3 -m unittest discover -s tools/tests -p test_ata187_prepare.py
```

Hardware verification of the installed hook: from an activated 40 ms baseline,
profile updates without activation left actual playout at 45 ms. After enabling
activation in the hook, a normal call used 85 ms actual playout, completed eight
checksum-verified 32 KiB downloads, and disconnected cleanly. A second wrapper
invocation during that answered call returned failure before SSH; no additional
preparation appeared in the helper journal. The call still had 13 CRC errors and
six ATA-reported losses, so this verifies automatic configuration and the busy
guard, not elimination of the remaining audio impairments.

### Experimental redundant audio bridge

The optional `sipfax_red.py` service wraps PBX-to-ATA PCMU packets in RFC 2198
redundancy with one previous 20 ms audio block (payload type 96). This is specific
to the lab ATA187 diagnostic receive configuration; RED is not negotiated in the
SIP SDP. Do not enable this route for arbitrary SIP devices. The native V.90
modem and PPP server remain in CT105; this bridge runs on FreePBX.

Install `sipfax_red.py` as root-owned mode 0644 at
`/usr/local/lib/sipfax/sipfax_red.py` on FreePBX, and `sipfax-red.service` in
`/etc/systemd/system/`. It uses the existing Python 2.7 or Python 3 standard
library, Linux NFQUEUE number 105, and iptables. The service runs as root to own
the packet rule. Its local control socket is root:asterisk mode 0660 inside a
root-owned directory. Default source/ATA addresses are the development lab;
change both ExecStart and ExecStopPost arguments for another deployment.

Set `redundantAudio` to the JSON boolean `true` in CT105's protected
`/etc/sipfax/ata187.json`, deploy the updated preparation helper, then merge
`ata187-freepbx-red-route.conf.example` while preserving existing contexts.
Run `systemctl daemon-reload` and `systemctl start sipfax-red` before dialing.
Enable the service at boot only after validating the installation.

The route prepares and activates the ATA before `Progress()`, reads its actual
RTP destination, and registers that call through a fixed local command. A 300 ms
wait after Progress separates the ATA early-media codec UPDATE from the answer.
A captured immediate-hangup failure overlapped those exchanges; a successful
delayed call completed the UPDATE first. This mitigates the observed timing
pattern but does not establish universal startup reliability. Database
access occurs in a separate control thread, never in the RTP loop. An absent
service or invalid destination rejects setup. The hangup handler removes the
registration. Unregistered traffic passes unchanged. A new call clears audio
history, even if it reuses the previous call's port.

The bridge accepts only the configured source/destination, a registered port,
IPv4 without options/fragments, and 160-byte PCMU with a 12-byte RTP header.
Other packets pass unchanged, preserving kernel checksum state. Redundant audio
is included only for consecutive sequence numbers and 160-sample timestamps.
No loss injection interface is installed in this service.

Stopping removes the exact owned packet rule and queue. ExecStopPost and startup
also clear an orphaned rule/socket under a process lock. Queue bypass and the
kernel queue-full fail-open flag allow ordinary packets if the bridge is absent
or overloaded. Restart begins without an active registration: existing calls
may lose their modem connection, and seamless mid-call recovery is unverified.
A subsequent call registers normally. This remains a single-call development
route; the shared ATA activation and global registration are not a multicall
implementation.

For rollback, set `redundantAudio` to `false`, restore the ordinary route example,
and invoke the normal preparation wrapper while idle to disable RED/FEC and
activate the basic profile. Stop/disable `sipfax-red` after the call ends. With
the new helper, omitted `redundantAudio` also means explicitly disabled.
