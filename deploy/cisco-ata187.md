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
