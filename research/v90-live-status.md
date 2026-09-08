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

## Digital capability exchange implemented (6cbccdf)

The old JM explicitly advertised standard analogue access and omitted PCM availability. Despite the server's SM_V90 label the notebook actually sent V.34 INFO on 1200Hz. Corrected JM digital PSTN (wire8d/codeb1) and digital PCM availability (wire47/codee2), keeping the existing V.34 menu unchanged. After correction the notebook switched to 2400Hz INFO0a, independently CRC-verified in recording PID5452 (local work/v90-rx-5452.s16).

New v90startup.c builds real 62-bit INFO0d, CRC, negotiated PCM law, nominal -12dBm0, then ToneB. It receives CRC-valid INFO0a across parallel symbol phases. It deliberately does not emit placeholder data. lm.c retains 200ms of V.8 input so the early INFO response survives the handoff. Receiver validates payload CRC without requiring all four unprotected trailing fill bits (hardware capture yields 1000 at the tone transition).

Tests: tools/tests/v90-startup.py covers both PCM laws, emitted DPSK bits, independent polynomial CRC, 14 symbol phases with guard/noise, corrupt payload rejection, handoff history. Optional recording argument verifies hardware INFO0a. tools/tests/v8-role-menu.py checks digital/analogue V.90 and V.34 menus. Framing smoke also passes.

Live verification: attempt e0ef6d20-d87b-4a2f-9f8a-1ba4d9a9ff88, PID5564, log `[v90p2] CRC-valid INFO0a at 0.061s: ack=0 3429=1`. This proves the first capability-exchange stage only. Ended via API and restored V.22bis service, verified active. **After any config restore explicitly chown sipfax:sipfax and chmod600**: shutil.copy2 backup was root-owned and cp-p initially caused EACCES; corrected.

Next: implement V.90 §9.2.1 timed ToneA reversal detection -> ToneB reversal after40±1ms ->10ms ToneB then silence -> second reversal/ranging -> receive L1/L2 -> second probing exchange -> INFO1d/INFO1a. V90Startup currently holds ToneB forever after INFO0d; no complete Phase2, DIL, Phase4 or upstream decoder yet. Do not use ordinary V.34 answer roles; V.90 digital side sends1200Hz and receives2400Hz regardless of answering the call. Spec remains available at the ITU reference above.

## First ranging exchange verified (8b5b5c4)

Implemented raw-sample phase-reversal boundary estimation, scheduled ToneB response40ms later, 10ms tail then silence, and second ToneA reversal/RTD measurement. Synthetic tests sweep40 reversal offsets with equal-amplitude1800Hz guard and noise, assert40±1ms reply, second reversal timestamp, and silence after80samples. Existing INFO and recording tests pass. Hardware guard power is substantial: RMS2424, Fourier amplitudes~1268 at1800 and1153 at2400; pure-tone coherence threshold0.8 rejected it. Threshold0.30 plus phase lock fixes that. One mixed window must not erase the prior phase reference.

Variable legacy V.8 CJ detection delay exceeded200ms: PID5717 contained valid INFO0a at4.889s while CJ was detected5.4s. Expanded V.8 history to1s. This retains the message but does not fix CJ timing itself. Local recordings work/v90-rx-{5717,5812}.s16 expose this. Beware interpreting second reversal from a NONinteractive recorded stream as a response: its2.04s repeats are recovery, not actual measured RTT; a timeout check remains necessary.

Successful live attempt66e594f0-12da-4ee8-b2bc-499f23cf0ab6, PID5829:
- INFO0a CRC valid at -0.011s relative to startup, captured via history.
- A reversal0.250625s -> B scheduled and transmitted0.290625s.
- Second A reversal0.413125s -> RTD82.500ms.
- Capture CT105 /var/log/sipfax/linmodem-{rx,tx}.s16.5829.
Ended with DialUpLab disconnect; restored baseline with correct config ownership and verified service active.

Next implement probe receive after second reversal (L1 starts10ms later,160ms L1, up to500ms L2), then ToneB to request analogue turnaround, next reversal response40ms plus10ms tail, server L1/L2, and INFO1d/INFO1a. Current ranging_state3 holds silence; this is not Phase2 completion. Upstream decoder and phases3/4 still pending.
