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

## Probing and INFO1 exchange hardware verified (53a45ac)

New stages receive160ms L1+200ms L2 after the10ms A tail; send ToneB, detect next A reversal, reply40ms later then10ms B; transmit21-tone L1 at+6dB for160ms, L2 until ToneA; transmit109-bit INFO1d and decode70-bit INFO1a with CRC. INFO1d offers only mandatory3200-baud high carrier, flat preemphasis, projected upstream4800bit/s for initial receiver development. This is a trial configuration, not a reduced final goal. Downstream remains V.90; INFO1a's downstream integer6 proves selection, not a completed data connection. Synthetic tests cover probe power ratio, INFO1d CRC and returned parameters, plus prior capability/ranging tests.

Hardware attempt7c9f98e8-7f44-4f5d-aa1a-267bfe2991e5, PID5983:
INFO0a at-0.585s (legacy V8 handoff still delayed). First A0.244375/B0.284375, second A0.406375, RTD82ms. Probe RMS2669.6. Turnaround A0.934375/B0.974375, server L1/L2 begins0.984375; INFO1d begins1.524875. CRC-valid INFO1a returns upstream4 (3200baud), downstream6(V.90), UINFO78. Hardware then sends Phase3 training but server remains silent, as expected with unimplemented Phase3. Call eventually failed. Baseline restored, correct ownership, service active. RX/TX in CT105 /var/log/sipfax/linmodem-{rx,tx}.s16.5983; RX copied to local work/v90-rx-5983.s16.

Next reference V.90§9.3.1: parse MD duration from INFO1a bits18:24, detect upstream S/Sbar, train usingPP then512T TRN; decode Ja(DIL descriptor), send Sd384T+Sbar48T thenTRN1d>=2040T andJd. Decoder must use GPA and negotiated3200 high carrier, not old hardcoded3429. Ja needed to build DIL; cannot fabricate training constellation/CP. Pending timeouts/recovery and probe-result rate selection are also required before production. Current ranging_state9 is silence awaiting implementation, not connected.

## Phase3 PP/Ja decoded offline

Analyzed actual PID5983 upstream (local work/v90-rx-5983.s16). INFO1a at7.2883s has MD=0. S/Sbar at~7.475s, PP after~7.515s. Rate(4,high)=3200baud/1920Hz. Existing research/v34-rx/v34_front.py matched filter beta0.1,2x upsample,5samples/symbol yields PP correlation0.9887516 at7.5369375 (sampling phase1). No new live call needed: this recording contains the real notebook's Phase3 training.

New research/v90/analyze_training.py demodulates differential four-point Ja and GPA descrambles (feedback delays5,23). Seven repeated Ja frames CRC-match0x8299, separated1736bits (0.27125s). First validated descriptor at9.8619375s. N147,LSP126,LTP126,H=[20,20,20,20,20,20,11,11], REF all78. Ucodes0..117 with extra78 after each group of4; actual requested DIL, not guessed. The seemingly odd long descriptor was valid once its variable length was parsed. Raw bit fixture test/fixtures/v90-ja-5983.bits carries no PPP credentials.

New v90dil.c/h parse variable-length Ja with framing, CRC, bounds and even padding. tools/tests/v90-dil.py verifies actual parameters, all1736 truncations and every single-bit corruption rejection. Included in native Makefile, but not yet used in live receiver. Offline analyzer can export descriptor via --descriptor path. No server changes this turn; baseline remains previously restored.

Next: port the successful 3200/1920 frontend and incremental Ja decoding to C and wire into ranging_state9. Need live receipt of Ja before Sd/Sbar,TRN1d,Jd; then implement S return detection,Jd-prime andDIL. V.90§8.4 defines digital waveforms: Sd64 repetitions(+W,+0,+W,-W,-0,-W), Sbar8 inverse repetitions, W=UINFO+16. GPC for digital scrambled TRN1d/Jd, GPA for received Ja. Maintain sample/frame alignment. Do not use old fabricated CP or claim a data connection from CRC-valid training messages.

## Streaming Ja and first downstream training trial (df32656)

The C receiver now validates live Ja using a streaming 3200-baud/1920-Hz frontend and the DIL parser. Trial18ae68ad-9c08-469c-a540-8fe59540dd48 decoded N147,LSP126,LTP126. Trial98b7c1b6-37fe-442f-b440-12b9e082b509 (PID6245) repeated the result and transmitted Sd/Sbar, minimum-length TRN1d and repeating Jd at startup-relative4.659625s, UINFO78. The notebook stopped Ja and became silent after Sd/Sbar, but did not demonstrably return S before retraining. Jd acceptance is unverified. Call finished Failed; baseline restored with sipfax ownership/mode600, service active. Captures /var/log/sipfax/linmodem-{rx,tx}.s16.6245; local RX work/v90-rx-6245.s16.

Tests cover streaming replay of the hardware Ja, both-law Sd/Sbar levels and durations, independent descrambling of TRN1d and Jd, and Jd CRC. These are training tests, not evidence of a V.90 data connection. Next inspect the actual transmitted capture and Jd timing/encoding, then implement S detection, Jd-prime, requested DIL and Phase4 before upstream data and PPP can work in V.90.

User-reported2.4kbps matches the established V.22bis baseline. Journal independently confirms CHAP authentication and notebook address10.64.0.2 on2026-09-08 at03:31UTC. Repeated IPCP link transitions and no demonstrated internet transfer mean stability and routed traffic still require acceptance testing.

## Phase3 DIL accepted and Phase4 CPt decoded (2026-09-08 04:28 UTC)

Correction to the preceding trial interpretation: full RX6245 analysis found S at14.58s (coherent320/1920/3520Hz) and Sbar at14.71s. The minimum TRN1d and Jd were accepted; the missing server S detector was the immediate obstacle. Do not lengthen TRN1d on the assumption Jd failed.

Added 3200/high-carrier S/Sbar detector using three coherent spectral lines, noise/single-tone rejection, and phase reversal detection. It drives completion of the current72-bitJd,12-bitJd-prime, then actual requested DIL. DIL uses each Ucode's Uchord H/reference, restarts sign/training patterns per segment, repeats the whole sequence, and terminates on a segment boundary after the second Sbar. Tests cover offset/noise S/Sbar, pure-tone rejection, actual6245 S detection, Jd-prime descrambling, DIL pattern/segment repetition and boundary termination. Existing startup and framing tests pass; native builds local and CT105 pass.

Live attempt a11e4ff7-03c6-4f74-8822-bc35bad73a58, PID6417:
- INFO0a via retained history; RTD97.25ms; INFO1a upstream4/downstream6/UINFO78.
- Live Ja N147/LSP126/LTP126; Sd begins startup-relative4.732500s.
- S detected8.875375s; DIL begins8.877000s.
- S/Sbar1 at8.987875s; second at11.220375s; DIL ends on segment boundary11.232625s.
- Subsequent upstream is actual Phase4 CPt. Offline research/v90/analyze_cp.py validates75 frames across timing phases in raw17..20s, CRC0x38c5,428bits. Training drn9,Sr1,lookahead1,mu-law,gainQ13=8180,filter=[63,0,0,0],all six frames use constellation0 with4 positive levels, corresponding codec constellation present. This training rate is not an established data connection. Fixture test/fixtures/v90-cpt-6417.bits saved.
- Call explicitly disconnected through API, later verified Failed; restored V22bis baseline with correct config owner/mode, service active. Raw RX/TX /var/log/sipfax/linmodem-{rx,tx}.s16.6417 copied to local work/v90-{rx,tx}-6417.s16.

Next implement bounded CP parser and live CRC-validated CPt reception, digital Phase4 R/TRN2d/MP/Ed using those real constellation and shaping parameters, and then upstream data decoding. Current stage2 intentionally emits silence; notebook eventually retrains because no Phase4 answer exists yet. Phase3 detector is explicitly specific to offered3200/high-carrier mode. Full V90 data + authenticated PPP internet remains unachieved.
