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
