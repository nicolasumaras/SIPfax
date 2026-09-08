# Live V.90 development — 2026-09-08

Goal: actual V.90 answering service and PPP internet, initially one notebook, future concurrent calls. Do not treat V.22bis success or SM_V90 as completion.

## Verified baseline
CT105 on Proxmox 192.168.1.20, service 192.168.1.25. FreePBX .29 routes 12345678. ATA187 .235 ports 63416874/23416874 registered. Notebook .217:4782 DialUpLab reports SoftV90 Data Fax Modem with SmartCP. API supports automated calls; no user intervention required. API secret is outside this document.

PPP CHAP authenticated numaras and assigned 10.64.0.2 at 03:31 UTC. Internet transfer still unproven. Host ppp_async loaded and persisted in /etc/modules-load.d/sipfax-ppp.conf. pppd still warns about read-only /run PID/database files.

## Changes and tests
645b35e corrects μ-law clipping and INT16_MIN handling in linpipe and slmodem bridge. Both exhaustively match installed SpanDSP for all 65536 inputs. linpipe now preserves framed stdout on fd >=4 and redirects legacy printf diagnostics to stderr. Framing test verifies 100 complete frames under verbose DSP logging. Native linmodem builds in CT105 with CFLAGS='-O2 -Wall -g -D_GNU_SOURCE -fcommon'. These source changes and rebuilt linmodem are deployed, but the running service was returned to its V.22bis configuration after the trials.

## Recorded hardware trials
- V.90: DialUpLab attempt 834c904b-b7bc-42e6-8255-9c66eed7df77, engine PID3908. V.8 CM/JM/CJ completed, mods=0x1c03. Server entered SM_V90, generated a **local placeholder** CP (38666, A-law), then emitted PCM without required phases 2–4. No real connection. Explicitly disconnected through API.
- V.34: attempt abada22c-bc6c-4823-9a6f-c0bb70b10658, PID3913. SIPFAX_LINMODEM_MAX=v34. Phase2 saw upstream probing and INFO1c activity but timed out without decoding INFO1c and assumed rate5. Phase3 waited for S; remote failed with error678. This is weaker than the older research's successful Phase4 claims and requires fresh investigation.

Captures in CT105 /var/log/sipfax/linmodem-{rx,tx}.s16.{3908,3913}, signed 16-bit PCM 8kHz. /var/log/sipfax/linmodem-trial.log contains full diagnostics (6.3MB). Trial wrapper /opt/sipfax/bin/linmodem-trial sets capture paths and maximum v34. Config backup /var/backups/sipfax/pre-linmodem-trial.json. Restored that config and restarted sipfax after trials. To resume trials set config.modem.command to wrapper and restart; changes to the config file are not watched automatically.

## Required next work
V90_init fabricates CP, V90_process explicitly lacks upstream receive. Implement real V.90 phase2 INFO0d/INFO0a probing/ranging, phase3 training/DIL, phase4 CP exchange and working V.34 upstream decode. Digital/analogue roles are different from ordinary call/answer roles; do not splice ordinary V34 answering startup blindly.
Reference: https://www.itu.int/rec/T-REC-V.90-199809-I — sections8/9 define startup. Existing research/v34-rx/FINDINGS.md and bundled linmodem history contain substantial experiments but must be checked against current hardware. Most remaining branches are already incorporated or superseded.
