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

## Phase4 exchanged; notebook reaches DeviceConnected / PPP authentication (2026-09-08 04:42 UTC)

Implemented v90cp.c/h bounded variable-length CP/CPt parser (up to1788bits) and continuous CP mode in the existing3200/1920 streaming receiver. Parses distinct transmit/codec masks, signed shaping coefficients, capability mask, type/rate/ack. No fabricated CP. Tests replay6417 with71 CRC-valid frames and reject every truncation and single-bit corruption of real CPt.

New v90pcm.c/h encodes six-symbol frames from real CP parameters, uses64-bit modulus mapping, GPC, all Sr0..3, and exhaustive bounded lookahead0..3. It prefetches complete input frames so lookahead cannot drop the first mapping frame. The old v90.c shaping filter incorrectly subtracts the a feedback terms; new implementation follows positive a1/a2 feedback in V90§5.4.5.6. Tests independently recover every source bit across32 law/Sr/lookahead combinations. Phase4 replay independently decodes TRN2d, repeated MP and CRC, MP-prime, two Ed frames, and48 B1d frames using both53.333k and56k actual CP masks. The bit-roundtrip tests prove coding invertibility; hardware evidence additionally verifies the notebook accepts the implementation on this line.

New v90phase4.c/h sends Ri>=192samples, Ri-bar24samples after live CPt, TRN2d2040samples, MP/MP-prime, Ed, and48B1d frames. It then sends scrambled idle ones using the real data constellation. MP currently offers4800 upstream,16-state trellis, no nonlinear encoding, minimum shaping for receiver development (final scope remains fullV90). Receive E is detected as20descrambled ones after a valid acknowledgedCP; this is not yet an upstream data decoder. SM_CONNECTED is deliberately not asserted because no data receiver/PPP transport exists in this engine.

Hardware trial52136df6-bbe7-484e-b3b4-03f8390ffba1 (PID6597) received CP drn20 (53,333.333bit/s), then CP-prime ack1. This proves TRN2d/MP acceptance. Initial implementation still lackedEd/B1d. RX/TX downloaded to work/v90-{rx,tx}-6597.s16. Offline CP at20.143875/20.25325,700bits, CRCab4e/5bc7,indices[0,1,1,1,1,1],masks58/57levels. Fixture test/fixtures/v90-cp-6597.bits.

After Ed/B1d implementation: trialdc1cbacc-a7c9-41eb-a063-bf75bec7d3c3, PID6711. Live CPt drn9,Sr1,ld1,gain8205. CP drn22 selects56000bit/s, thenCP-prime ack1. Ed completes at Phase4-relative3.599250s, B1d usesK37/S5. Received E at3.730625s. Data-mode CP has one72-level constellation for all six intervals, CRCfb41/3f6a,428bits; fixture test/fixtures/v90-cp-6711.bits. Server remains sending idle ones.

**Independent Windows evidence:** DialUpLab at04:42:44.308UTC reportsDeviceConnected, thenAuthenticating/Authenticate and repeatedAuthNotify. This is the first confirmed hardware carrier connection through the actual V90 startup. Server CP proves56000 selection; API does not expose a completed connection-speed object because PPP never succeeds. Call later fails with APIerror31; do not claimPPP/internet success. Full attempt saved work/v90-b1-attempt-final.json. RX/TX work/v90-{rx,tx}-6711.s16 contain upstream B1/data (around21s onward), suitable for the missing4800bit/s3200-baudV34receiver. CT captures /var/log/sipfax/linmodem-{rx,tx}.s16.6711.

Final state: call terminalFailed verified; V22bis config restored with correct owner/mode, serviceactive. Current experimental native build deployed but not selected. No goal completion: implement upstream B1/data decoding, byte/framing/error-control negotiation and bidirectional PPP, verify internet transfer and stability, then broaden upstream rate support/recovery/concurrency. Existing research/v34-rx/v34_rx.py explicitly warns its DSP front end is not yet a complete data decoder; reuse validated pieces, not completion claims.

## Native upstream decoder and bidirectional PPP exchange (2026-09-08 05:06 UTC)

Implemented research/v90/analyze_upstream.py: at4800bit/s,3200baud the V34 mapper is b12/K0/four-point. Each4D pair yields I1 from half the modulo4 odd-even phase difference and I2/I3 from the differential rotation of consecutive even symbols. V34§9.3.2 specifies interleaved triplets; legacy v34.c uses a different array layout for b<=12 and should not be copied blindly. GPA delays5/23, LSB-first8N1, async PPP unescaping and FCS verification recover a real29-byte LCP Configure-Request from RX6711 (initial offline29..34s window).

New v90upstream.c/h implements this in C with81tap RRC, five half-sample timing hypotheses and both4D pair alignments. It accepts only FCS-valid PPP frames, deduplicates hypotheses within5ms, bounds frame buffers, and discards corrupt/oversized frames. Native replay21..36s ofRX6711 recovers three29-byte LCP requests. This is currently a hard-sliced receiver WITHOUT trellis error correction or adaptive timing; longer/noisier packets need additional work.

Connected verified incoming frames to modem RX FIFO as canonical escaped PPP frames. New downstream serial adapter is LSB-first8N1 (legacy serial.c is MSB-first); after48B1d frames the PCM encoder consumes real PPP TX FIFO bits. lm_get_state declares the V90 link ready only afterB1d stage and at least one CRC-valid incoming PPP frame. linpipe reportspty-opened, allowing the existing pppd supervisor to start. Also set PTY raw/noecho at creation to prevent initial receive bytes being echoed beforepppd opens the slave. Existing startup/framing/Phase4 tests and nativeupstream replay pass.

Trials:
- e1a59292-b17e-4af2-bb00-ced270754206 PID6889 retrained duringPhase4 beforedataCP. RX/TX saved work/v90-{rx,tx}-6889.s16. First3seconds of TRN2d/MP are sample-identical to successfulPID6597, so no evidence this is a changed transmitter bitstream.
- a5456985-4121-4acf-b0bf-5348d3254faa PID6897 succeeded through53.333kB1d and bidirectionalPPP. Nine FCS-valid incoming frames, pppd6898 at04:54:14UTC, thenCHAP rejected an EMPTY peer name at04:54:17. APIerror691. Thus data path works; Saved mode on the running olderXP app sent blank credentials. Capturework/v90-rx-6897.s16 contains authentication exchange: do not publish payloads/fixtures from it indiscriminately.
- Explicit credentials (read in memory from CT config, passed as userName/password, credentialModeSupplied; no secrets logged) attemptfccaa8a6-184a-4198-8749-f3a82e036063 PID6904 failed678 duringDIL. Native/independent inspection shows no secondS at all; it retransmitsToneA by16.5s. Do not loosen S detection on an assumption it missed a real signal. SavedRXwork/v90-rx-6904.s16.
- Explicit-credential attemptb47b2add-a767-4e81-bad7-64747352da59 PID6913 reachedPPP again (pppd6915 at04:59:37), but onlyLCP repetitions followed; API eventually reported756 (alreadydialing). Not authenticated. RXsavedwork/v90-rx-6913.s16. RunningAPIhealth isDialUpLab-XP1.0.0.0 and responses omitcredentialMode despite currentrepo having it. Current/oldsource both parseuserName. Connectionslist laterempty. Investigate timing/nativeRASstate before claiming credentials resolved.
- AfterPTYraw fix, attempt61f67bd1-530b-4b75-8081-9722254c7d84 PID6932 failed678 afterCPt/TRN2d, beforedataCP. NoPPPdaemon launched.

Temporarypppddebug was enabled through/etc/ppp/options with backup/var/backups/sipfax/pre-v90-debug-ppp-options; latestcall never reachedpppd. Restored original options and V22bis config, chown/chmod600 verified, sipfaxactive. No activecall/exec handles. Experimentalbinary remainsdeployed but notselected. Configbackup pre-linmodem-trial isrootowned: continuecorrectingownerafterrestore.

Next: stabilize media/startup acrosscalls (captureRTP atPBX to compare exact payload continuity/timing; existingwavTX matches do not prove codec output delivery). Authenticated PPP with explicitexistingcredentials stillunproven. Addtiming recovery/trellis correction for robust upstream packets, then verifyPPPIPassignment, ping/HTTP traffic throughmodem, publicinternet/NAT, stability. CurrentCTforwarding=1 andnftableshaspool10.64.0.0/24 masqueradeoneth0, permitspublicegress andblocksprivatedestinations. Baselineaddresses10.64.0.1/.2. Goalremainsactive; no internet successclaimed.

## Authenticated V.90 and first HTTP through PPP (2026-09-08 05:21 UTC)

Trial d11f0a1c-27bf-4da6-a0a2-a22b0fc04312, modem7019/pppd7020: negotiated CP 160000/3 bit/s downstream, authenticated numaras, assigned10.64.0.2. Windows API independently reports Connected and both PPP addresses. No pacing yet. Link ended after about62s total; late ping found no PPP interface. Client/server clocks differ, so use capture-relative timing for correlations.

New research/v90/audit_rtp.py reads Ethernet IPv4 G711 RTP pcaps, validates headers and omits partial capture records, reports sequence/timestamp continuity, timing intervals and payload hashes without dumping payloads. FreePBX capture work/v90-rtp.pcap (outside repo) has3111 packets in each direction on each leg. All upstream payloads match exactly across PBX; all downstream payloads match exactly. No sequence gaps. ATA's first timestamp increment is120 for160samples; later increments160. Outgoing intervals0.65..56.10ms show immediate return of incoming scheduling bursts. No proof of what ATA jitter buffer actually played.

Trial58839510-3542-420d-80c4-2c69fbf61ac2, PID7075, explicitcredentials: failed678 beforePPP. This was an unpaced retry with immediate network probing prepared.

Added optional RtpPacer output queue, selected by SIPFAX_RTP_PLAYOUT_MS (default0, development-only). It buffers then emits by8kHz RTP duration using monotonic deadlines, retains exact payloads, reports underrun/late/overflow, bounds200packets, clears timers on call teardown, and avoids catch-up bursts after a late event loop. No resampling or inserted silence. Actual media clock drift adaptation remains absent. Also fixed linpipe PTY transmit reads to respect available FIFO capacity; previously sm_put_bit silently discarded excess bytes when full.

Trial8a52395e-0873-491a-ad13-59a6013ca3e5, modem7257/pppd7277,60ms pacing: Windows Connected/Authenticated,10.64.0.2. Immediately ran curl in CT105 to http://10.64.0.2:4782/api/v1/health with API auth. **HTTP succeeded, exit0, complete valid DialUpLab-XP health JSON.** This is actual bidirectional TCP/IP over the modem, not a LAN request. Ping0/4 (could be Windows firewall; HTTP disproves total IP failure). Larger /attempts HTTP request was started too late and timed out after link loss; not a bulk-transfer pass.

Paced capture work/v90-paced.pcap: FreePBX preserves all3173 outgoing packets byte-for-byte; all3180 forwarded upstream packets match (one final ATA packet not forwarded at hangup). Normal outgoing intervals about20ms, minimum18.87ms. Incoming88.89ms gap at60.168s coincides with output107.24ms gap/pacer-underrun. RX changes to retrain-like signal around62s and call ends shortly after. This supports buffer exhaustion as a contributor, not proof it explains every failure. No successful internet request or sustained transfer yet.

Trial4103a581-fe1d-42fc-81f1-841598c1a023, PID7387,120ms pacing: RTD202.25ms, Ja/DIL/CPt valid, MP sent, remote retrains beforedataCP. APIterminalFailed678 verified. Increasing buffer alone has not solved startup. Next separate Phase4 interoperability from audio delivery and verify longer transfers with immediate probing on each successful call.

Tests:42 Node tests passed on CT (pacer, session and multiline), nativebuild local/CT passed, linmodem-framing100frames passed. Local shell lacks node. Native RX/TX7019,7257,7387 copied to work/ outside repo; may contain authentication material. Do not publish those payloads. Saved API responses work/v90-{rtp,network,paced,paced120}-*.json. All calls and capture tool processes terminal. Restored original PPP options (debugoff), baseline modem config with sipfax ownership/mode600, removed temporary systemd v90-pacing.conf, daemon-reload/restart, serviceactive. New JS/C deployed but pacing disabled and baseline selected. Goal remains active: reliable V90 startup, sustained data, real public internet traffic, then rate/retrain robustness and future concurrency.

## ATA187 pass-through configuration verified (2026-09-08 05:38 UTC)

Compared successful7257 and failed7387 Phase4 captures: identical training masks [53,78,88,96], Sr1/lookahead1/filter[63,0,0,0]; only gain differs8178 vs8175. First24000 TRN2d/MP samples of both calls are identical to6597 (start17.091625 and17.643500 respectively). Failed7387 has onlyCPt, no hidden CRC-validdataCP in17..22s. Do not infer a changed training bitstream as cause.

Additional trials:7611d83e-6a95-4d87-95e4-05ab677524b8 at120ms pacing and ca296e08-1fe4-4bf5-aad5-6ecd445ab457 at80ms pacing bothFailed678. No new PPP success. Reusable private work/trial_v90.py reads PPP credentials in memory, API key from DIALUPLAB_API_KEY, saves responses using V90_TRIAL_TAG and immediately requests /api/v1/attempts through10.64.0.2 afterConnected. Does not disconnect active calls automatically. Long-request acceptance remains unverified.

ATA read-only HTTP pages discovered: /Device_Information.htm, /RTP1.htm, /RTP2.htm, /Console_Logs.htm and /FoxCliLog. Log is a circular multi-section file, not chronological overall; inspect timestamps. Existing calls reported faxModeFAX_RELAY, and2100-PR-Net remainedVoice->Voice. Cisco ATA187 configuration documentation distinguishes fax/modem pass-through fromT38relay. Current XML lacked faxMode. Checked configuration key against deployed9-2-3-1 firmware; no firmware was modified or executed.

Set device/vendorConfig/faxMode=1 and bumpedversionStamp in /tftpboot/ATA5C5015A8DBAB.cnf.xml andATA5015A8DBAB01.cnf.xml onFreePBX. Backups /var/backups/sipfax-ata-v90-passthrough/. Existing Eventcheck-sync NOTIFY did notreload. Added sipfax-ata-service-restart to /etc/asterisk/sip_notify_custom.conf (backedup), withEventservice-control,Subscription-Stateactive,Content-Typetext/plain,Contentaction=restart. Reloadedres_pjsip_notify.so and sent onlytoendpoint63416874 whilebothportsidle. ATAloggedregister-call-idmismatch warning but proceeded CEVT_SW_RESTART, fetchedbothXML05:32:39/44, andbothcontactsregisteredagain.

**Actual verification:** trialfc9a6a07-1e14-454a-b019-55aa01feb85f at05:33:42 loggedfaxModePASSTHRU; at05:33:45 answer-tone detection changedVoice->VBD withCalling PR EC VAD DTMF DC flags. Thus numericvalue1 andvendorConfigplacement verified onATA187 (do not substituteATA190/191value0). This80mspacedcall failed duringDIL beforesecondS. Then unpacedtrial2812a3fe-11f2-4ca1-a7f6-e3ba1768a6b7 hadJa/Sd/TRN1d/Jd but noS, retrainedToneAabout16s, failed678. CorrectATAmediamode has notmade trainingreliable. No new successfulHTTP/internettransfer.

Added deploy/cisco-ata187.md withverifiedconfiguration/reload/verification procedure. ATApass-throughchange remainsapplied, SIPaccountsunchanged. Installedtcpdump,squashfs-tools(+liblzo2-2) inCT105 fordiagnostics; legacyfirmwareSquashFS2/LZMAnotreadablebystockunsquashfs. Firmwaremetadata analyzedonlyinwork/, do notcommitfirmwareassets. A CTany-interface PPPcapture /tmp/v90-ppp-network.pcap had0packets because latestcallneverreachedPPP; captureexplicitlystopped, toolterminalverified.

Finalstate: allfournewcalls terminalFailed; baselineconfigrestored withowner/modecorrect, temporaryv90-pacing.confremoved, daemon-reload/restart andserviceactive. PPPdebugremainedoffthisturn. NewATApass-throughsettingverifiedandretained. Nextresolve intermittentSd/Jd/DIL/Phase4 interoperability, rather than repeatingcallswithoutnewDSPevidence; pendingCJtimingfallback stilloccasionallylate. FullV90internetgoal remainsactive.

## Coherent CJ, startup retrain response, and a stalled bulk transfer (2026-09-08 05:55 UTC)

New v8cj.c/h performs answer-side coherent1180/980Hz300-baud acquisition across27integrate-and-dump phases. Requires threecomplete8N1zerooctets (30bits), continuoussignalcoherence/energy; enabledonlyduringJM afterCMselection, existing400msJMfloorretained. Fixes demonstrableV21PLLmiss: RX7716 hadactualCJ4.73625s butlegacyfallback5.42s. Added hardware-only4.4..4.8s fixture test/fixtures/v8-cj-7716.s16 (noPPPcredentials), tools/tests/v8-cj.py with81syntheticphase/frequencycases, two-octet/tone/noise rejection, hardwaretimestampassert. Build, V8role-menu and100-frameframingchecks pass.

Trial489bbc9f-1301-46a4-9de8-3bdd3017b293 PID7925: newcoherentCJ acquired0.931375safterJM, transition4.720s; INFO0a nowreceivedliveatstartup0.071s. LaterJa/Sd/Jd butnoS; Failed678. Thushandoffdelayfixedbutnotfulltraining.

Added V90startup response to caller retraining inPhase3/4 beforedata-mode: require>50mscoherentToneA, silenceexactly560samples70ms, resumeToneB and40msphase-reversalreply, retaincapabilityexchange andresettrainingDSP. NoINFO0 retransmission onthisresponse, noV8restart. Counterrecordsretrains. **Data-mode retrains/DTEclamping andlocallyinitiatedtimeouts remainunimplemented.** Tests independentlycheckduration, ToneBcoherence, reversalresponse andoffbandrejection. ExistingPhase2test erroneouslycontinuedToneAafterINFO1; changedthatPhase2-onlystimulus tosilence, sincecontinuingToneAnowcorrectlyrequestsretrain. FullstartupandPhase4replay6711tests pass.

Trial0bc61bac-5649-4900-8862-c55710d3dd3b PID8042/pppd8044: firststartupreachedV90CP53.333k, authenticatednumaras05:48:34, assigned10.64.0.2. Unpaced, ATAalreadyPASSTHRU. ImmediateHTTPGET /api/v1/attempts overPPP receivedHTTP200 and4368bodybytes of68033 beforestall. Curl endedmax100s exit28; itsdeadlinewouldnotfitentire68KB at4800upstream evenwithoutstall, but4.3KB/100s isclearlynotasustainedpass. ServerPPPduration1.1min, sent904/received12305IPbytesincludingretransmissions; remote ToneAretrainaroundraw88s, disconnected96s. Nointernetacceptance.

CT /tmp/v90-transfer.pcap copiedtowork/v90-transfer.pcap:16IPpacketsstarting05:48:53. Multiple1448-byteTCPsegments, LinuxACKsadvance4368; XP repeatedlyresendsolder1448-byteblocksandTS echo lags, eventuallytrafficstops. Capturedoesnotcontainstartoftransfer. RawRX/TX8042 copiedtowork/. Privatework/decode_v90_tx.py independentlyinvertsactualCPmasks/Sr1signshaping/GPC/UART/PPPFCS overTXraw24..85s: alignment2,81333mappingframes,26CRC-validPPPframes including6LCP,2CHAP,4IPCP,1IPv6CP,1IP,11VJ-uncompressed0x2f,1VJ-compressed0x2d. Thisprovespre-RTPencoderemitsvalidPPPframes; notproofATAplayedthemcorrectly. Donotpublishauthpayloads.

Diagnosticnovjtemporarilyaddedto/etc/ppp/options (backup/var/backups/sipfax/pre-v90-novj-options). Trialb942907d-45c7-462c-9124-c3ab953c2935 PID8110: failed678beforePPP, soheadercompressionhypothesisunresolved. **Hardwareverifiednewretrainresponse:**11callerretrainrequestsprocessed; eachre-enteredranging, measured~57msRTD,exchangedCRC-validINFO1a. Afterfallbackrequestsupstream4/downstream4/UINFO69,currentV90-onlytraininggate requiresdownstream6,sonotrainingemittedandcallerretries. Futureworkmusthandle/refuseunsupportedfallbackcoherently; cannotclaimthisisV90datarecovery. CapturesRX/TX7925and8110copiedtowork/.

Finalstate: all3newcalls terminal, captureterminal, noactivehandles. RestoredPPPoptionsfrompre-v90-novjbackup, baselineconfigwithcorrectowner/mode, serviceactive. ATApass-throughremains. Noextrartppacingdropin. NativeCJ/retraincodeisdeployedbutbaselineselected. NexttargetssustainedIPtraffic/downstreamACKdelivery, novjtrialthatactuallyreachesPPP, adaptiveupstreamtiming/FEC, androbusttraining/fallback. Goalactiveandunachieved.


## Additional bounded transfer diagnostic (2026-09-08 05:58 UTC)

Trial 0c573bda-a3eb-4879-b9ef-50f110ab3776 enabled novj and prepared a small current-attempt HTTP response instead of the full attempt history. It ended Failed678 before PPP, so it provides no evidence for or against TCP header compression as the transfer-stall cause. The startup retrain responder again completed repeated ranging/INFO1 exchanges, but the caller selected unsupported downstream4. No new successful transfer. Original PPP options and baseline modem configuration restored with sipfax ownership/mode600; service active.

A comparison of saved TX8042 RMS with advertised maximum power is inconclusive until ITU Table1 linear-value scaling and the actual measurement point are checked. No amplitude or negotiated constellation changes were made on that hypothesis.


## Fractional matched filtering recovers lost upstream packets (2026-09-08)

Resolved the power suspicion against ITU-T V.90 Table1/Table15: Table1 linear values match signed16 PCM (e.g. mu-law U78=3772); the -6dBm0 maximum corresponds to RMS8028, not RMS4024. TX8042 data RMS~5800 is below that maximum (approximately -8.8dBm0). The previous -2.8dBm0 estimate used an incorrect reference. No transmitter amplitude change is justified by that measurement.

Independent offline replay of RX8042 raw24..85s estimates only~0.48ppm clock drift and recovers40 CRC-valid PPP frames, including9 frames of1503bytes. Native receiver before this change recovers36 total, only7 full-size. Its half-sample outputs were averages of adjacent RRC outputs, causing baseband interpolation distortion. Evaluating actual half-sample RRC taps recovers38 total; evaluating quarter-sample taps across10 symbol timing phases recoversall40, includingall9 full-size. No adaptive clock or trellis FEC is claimed: this improves the existing fixed-phase receiver. Native CPU replay remains substantially faster than real time.

Changed v90upstream.c/h to four fractional RRC filters and ten timing phases, preserving CRC-gated delivery and duplicate suppression. Extended tools/tests/v90-upstream.py with configurable interval/minimum total/minimum full-size frame counts. Regression command: python3 tools/tests/v90-upstream.py ../v90-rx-8042.s16 --start 24 --end 85 --min-frames 40 --min-long-frames 9. Captures contain private authentication/traffic, remain outside the repository. Existing RX6711 three-LCP test and complete Phase4 replay pass. Local and CT native builds pass. Hardware transfer verification remains necessary.

Hardware trial230c3361-3e0f-4f00-9238-d71d050f44ea (quarter-small) endedFailed678 beforePPP. It cannot establish live throughput improvement. Caller retrains eventuallyselectedunsupporteddownstream4. OriginalPPPoptions remainedselected (no novj for this trial), baseline modem configurationrestored withcorrectowner/mode, serviceactive. Newnativefilterbinary remainsdeployed butnotselected. Noactivecall/toolhandles. Nextpriority remainsintermittentPhase3startup, thenliveverificationofrecoveredlongpacketsandpublicinternet.


## Fractional handshake receiver and another authenticated hardware call (2026-09-08)

Latest failed8373 and successful8042 TX have identical first8000 DIL samples after aligning DIL start (8373 first alternate level13.640s,8042 14.2585s). ATA last-call statistics for8373 reported2 network-lost packets, but their timing is unknown and may involve teardown. This is not proof of a startup DSP defect or RTP cause. Captures8373 copied privately to work/.

Applied the same fractional matched-filter correction to v90training.c/h (Ja and CP): four direct fractional filters, ten symbol timing hypotheses. Existing Ja5983 and8373 replay pass, completePhase4 RX6711 remains sample-consistent and passes, fullPhase2/startup/retrain test passes; CT build passed. No new adaptive clock/FEC is claimed.

Trial2a79e219-d6c9-4411-aa5c-87ed0bf28a66, modem8565/pppd8567: live53.333k CP, authenticated PPP10.64.0.2. First small HTTP TCP connection timed out after5seconds; a second health TCP connection timed out after20seconds whileppp0 stillpresent. RX/TX captures8565 savedprivately. Link laterretrained/dropped around88s. Native receive replayraw24..85recovers26 PPPframes, IPtraffic exclusivelyIGMP224.0.0.22 andUDPbroadcast255.255.255.255. NoTCPresponse observed. Independentdownstreaminverse raw24..75recovers25validPPPframes,including12uncompressedIP0x21TCPpackets10.64.0.1->10.64.0.2. ThusVJcompressiondoesnotexplainthiscall'sSYNtimeout. Theseobservationsdonotdistinguishundelivered/corruptedanalogdownstreamfromclientfiltering; inspectclientRASstatistics/firewall/activeIPprobeinfuture ratherthanassumingallfailureisDSP.

PBX tcpdump bounded150seconds, terminalexit124 after17748packets/0kernel-dropped. Snapshotwork/v90-training-quarter.pcap has4423packetsperleg, allpayloadsidenticalacrossPBX, nosequencegaps. IncomingATA initialtimestampstep120for160samples (knownquirk), remainder160. Maximuminput50.401ms/output50.514msinterarrival. ItprovesPBXpreservationonly, notATAanalogplayback. HardwareAPItrial69dfa74f-2918-48e8-974c-d6f39975b0b3 withnovjendedFailed678beforePPP; noadditionalcompressionresult.

Allcalls/capturehandles terminal. RestoredPPPoptionsfrompre-v90-novjbackup andbaselinemodemconfig, ownershipsipfax/mode600, serviceactive. Fractionaltraining/databinarydeployedbutbaselineselected. No sustainedinternetorbulktransferpass. Goalactive; nextdiagnosticmustseparateclientIPacceptance/downstreamanalogdeliveryandhandshakeintermittency.


## V22bis control confirms notebook accepts PPP HTTP (2026-09-08)

Baseline control attempt5421017b-b182-483d-a99c-b1e08baafa71 authenticated and reached10.64.0.2. First HTTPconnect timeout5seconds was insufficient; secondhealthrequest with20secondconnect/30secondoverall succeeded HTTP200,151bytes,5.535seconds. ppp0capture showed actualTCPrequest/responses overPPP. Thus the laptop listener/firewall canacceptPPPtraffic; do nottreatV90SYNtimeoutsasprovenWindowsfiltering. Baseline remainedup5.3minutes until explicitAPIdisconnect; thatPOSTobservationtimedoutbutserverauthoritativelyconfirmedpeerLCPterminationandpppdexit06:25:08UTC. Serviceactive/baselineunchanged.

CurrentDialUpLabAPI lacksRASstatistics andoutboundHTTPprobes, whichlimitsdiagnosisoftheWindowsreceivepathandpublicinternetverification. PreparedDialUpLabbranchcodex/v90-network-diagnostics,commit9ab56b6,withXP1.1connectioncounters andauthenticatedPOSTconnections/{id}/probe boundtoPPPsourceIPv4. Bounded30seconds/64KiB, no proxies/redirects/commands/routemutations. SystemDNSandWindowsroutingstillrequire server-sidecapturetoprovePPPpublicegress. WindowsCI34194543424 dispatched forfullbuild andloopbackdiagnostictests. Notyetinstalledonlaptop; runtimeversionremains1.0.0.0.


DialUpLab CI first run34194543424 failed because the new testproject inherited nullable-enable incompatiblewithC#4. Fixed explicitnullable/implicit-usingsdisable andbuildfailureguard in85e0269. Rerun34194672806 **fullypassed**: existingsolutiontests,newXPdiagnosticloopbacktests,XPbuild,portable/offline/installerartifacts. Downloadedportableartifact tooutputs/dialuplab-xp-1.1 andzippedoutputs/DialUpLab-XP-1.1.zip(35664bytes). EXESHA25665b9f597cfc205adeef6949cfe32cfeace13d657431c647f7a57aeb85b888d74. HostedonCT105 /opt/dialuplab-downloads via unprivilegednobody transientunitdialuplab-download,PythonHTTPbound192.168.1.25:8081,RuntimeMaxSec86400. Verified http://192.168.1.25:8081/DialUpLab-XP-1.1.zip HTTP200 andexactSHA256matchtolocalpackage. Containsnoprivatekeys/settings. ItmustbeextractedandruntoreplacetheoldXPexecutable; existingAppDatasettingspersist. NeeduserinstallationbecausecurrentAPIhasnoselfupdateorremoteexecutioncapability. Runtimehealth1.1.0.0willconfirmwheninstalled. Diagnosticbranchpushed,notmerged; goalremainsV90publicinternetunachieved. SIPfaxbaselineactive,noactivecallsorCItoolhandles.


## Respond to data-mode retrains without restarting PPP (2026-09-08)

RuntimeDialUpLabstill1.0.0.0at06:31,connectionsnone; requestedXPdiagnosticupdateisnotyetinstalled. Continuedindependentserverwork. Removedstartup-onlyrestrictiononV90ToneAretrainresponder. Resettingthephase4datapumpimmediatelystopsDTEbitconsumptionandreceivedPPPdeliveryduringretrain. linpipeleavespendingDTEbytesinPTYwhiletraining(afterinitialconnection), maintainingboundedkernelbackpressure,andemitspty-openedonlyoncepercalltokeeponePPPprocess. ExistingFIFOandserialstateoutsideStartupremainintact; thisisnotLAPM/error-correctingdelivery,andbytesalreadyinflightmayneedPPP/TCP retransmission.

Extendedstartupregressiontocoverphase3anddatamode,checkDTEcallbackstopsduringretrain,70mssilence/ToneB/40msreversalresponse,offbandrejection. Optional --data-retrain withprivateRX8565 checksraw24..69normaldatadoesnotfalse-triggerandlateToneAtriggers. Fullhardwareopen-loopreplaydetectsToneAat79.5885s. Nativebuild,fullPhase4RX6711,and100-frameframingchecks pass. DeployednewCbuildtoCT.

Hardwaretrialf3262dd1-650d-4c4a-840c-c69086eb274d,modem8889/pppd8890: firststartup53.333k,CHAPauthenticated06:37:41,IP10.64.0.2. SmallHTTPconnectiontimeout20seconds,0bytes. **Liveverifieddataretrainresponse:**firstToneAresponseatstartup29.059s (~raw33.9),77.25msRTD,CRC-validINFO1a stilldownstream6,andnewJa/Sd/Jd/DIL. ThisretrainfailedbeforesecondS; subsequentrequestsselecteddownstream4(unsupported). PPPprocess8890remaineduntilactualmodemhangup06:38:13,0.6min afterIPup,notrestartedbyretrain. No data-mode recovery or successfulinternet claimed. RX/TX8889savedprivately. Private trialhelperconnecttimeoutnow20secondsbasedonV22control.

Hardwarecallandtoolsended;baselineconfigrestoredwithcorrectowner/mode,serviceactive. PPPoptionsunchanged(debug/novjoff). XPdiagnosticdownloadremainsavailableviaCT8081transient24hserver. Goalactive; needslaptopdiagnosticupdateforreceivecounters/outboundinternetprobe,plusreliabletraininganddatarecovery.


## XP diagnostics installed; public internet verified in control mode (2026-09-09)

User installed the update. Health at15:14:53UTC reportsDialUpLab-XP1.1.0.0; connectionstatisticsandHTTPprobe endpoints work. Newprivatehelperwork/trial_v90_diagnostics.py readsPPPcredentialsinmemory, dialsSupplied, andonConnected requestshttp://example.com/ boundtoPPPsource, savingcounter/proberesultswithoutcredentials.

V90attemptsc195e7e7-fa61-42b0-a48d-b47b37d5c8fc and7c9868a4-751d-4e1f-8533-e31460125b69 bothendedFailedbeforePPP. NoV90internetorcounterresultfromthese. ServerdirectegresscontrolreturnedHTTP200/559bytes.

**First actual notebook-originated public internet control:** baselineV22bisattempt e720f2cc-122e-4eae-9b35-35b31ee0e312. WindowsRAS reports2400bps. Probe source10.64.0.2 ->http://example.com/ returnedHTTP200,complete559bytes,16.969seconds. SHA256ff67a9d764d6a2367a187734e697f6a53217db9a21c101d410a113ca871a299d exactlymatchesindependentserverfetch. CTtcpdumpverifiedPPPinterface traffic10.64.0.2:1755 <->172.66.147.243:80 includingrequest56bytesandpublicresponse/retransmissions. ThusrequestactuallycrossedPPP/publicegress,notjustLAN. RASbefore:886sent/322receivedbytes,22sent/16receivedframes,CRC18/alignment1. After:2504sent/2300received,42sent/25receivedframes,CRC24/alignment1. SixnewreceiveCRCerrorsduringprobe; networkingworksdespiteerrors.

Comparedbaselinewith100msoutgoingRTPpacing: attempt739b0670-7034-43a9-9b79-535486dc45d5,2400bps. HTTP200,complete559bytes,sameSHA256,15.550seconds. CRC18->23(fivenew),goodreceivedframes16->25,alignment1unchanged. Onecallperconditionisnotastatisticallymeaningfulimprovement;donotclaimpacingfixesBERorV90. NoextrapolationofV22CRCcauseontoV90withoutdata. Bothcontrols explicitlyAPI-disconnected. Capturesbounded100/120secondsfinishedwith0kernel-drops. Allhelper/toolhandlescompleted.

Finalstate: baselineconfigselected,temporaryv90-pacing.confremoved,daemon-reload/restart,serviceactive. Nopppoptionchanges. NewAPIremainsinstalledonXP. Goalstillunachieved: V90trainingreliabilityandactualV90data/internetverificationremain. End-to-endcontrolnowprovesPPP/NAT/publicendpointandprovidesvalidlaptopreceivecounterinstrumentationfornextV90success.


## First V.90 public internet and complete bulk download (2026-09-09)

Added optional SIPFAX_V90_MAX_BPS for the experimental native backend. Jd advertises standard downstream rates at or below the requested ceiling, comparing exact rates in thirds of bit/s. Default and invalid settings retain 56000; valid ceilings are 28000..56000. Reserved bits and CRC remain valid. Independent training tests cover both laws, waveform/framing/CRC and several ceilings including nonstandard 35000 and invalid values. Local and CT builds pass. CT uses /etc/systemd/system/sipfax.service.d/v90-rate.conf with ceiling48000, no RTP pacing or PPP option changes.

Attempt 5bca30a6-19a0-4c29-9845-275c61a40476, native9969/pppd9972: Windows RAS negotiated48000bps, CHAP/IPCP succeeded. Notebook-originated http://example.com/ returned HTTP200, complete559bytes in1.905seconds, SHA256 ff67a9d764d6a2367a187734e697f6a53217db9a21c101d410a113ca871a299d matching the independently fetched body. CT tcpdump confirmed actual PPP traffic10.64.0.2:1775 <->172.66.147.243:80. Capture terminated normally at its timeout,12packets,0kernel drops. Windows CRC/framing/error counters were zero during this initial public probe.

A controlled32768-byte file (bytes0..255 repeated128times), served only on PPP gateway10.64.0.1:8082, downloaded completely in7.700seconds (~4256body bytes/s). SHA256 e11360251d1173650cdcd20f111d8f1ca2e412f572e8b36a4dc067121c1799b8 matched exactly. Windows received bytes1384->35429, CRC2->2 (no new CRC errors during the transfer), other error counters zero. This is a real V.90 downstream data milestone, not just a reported carrier rate; implemented upstream remains4800bps.

Reliability did NOT pass. Last initial-data CRC-valid upstream PPP frame at receiver128.530750s; caller requested retrain at startup169.778500s. Repeated retrains did not recover data. pppd logged four unanswered echo requests at15:30:08UTC, connect time4.5minutes, then exited15:30:19. Windows continued reporting an open48000bps connection at~320seconds, so its connection status alone is insufficient evidence of a functioning link. Later repeated probes failed ConnectFailure with no new received bytes. Explicit API disconnect completed15:32:06. Private capture9969 and JSON probe evidence remain outside the repository. Neither sustained operation nor successful retrain recovery is claimed.

Repeat attempt4bf33264-a581-4028-aa2e-85c2c6683975 failed beforePPP and fell back to unsupported downstream4. Third48k attempt e207e65c-178b-4898-a84e-c053ef90a856, native10057/pppd10058, authenticated at15:34:59UTC. Public example.com probe again completed559bytes with matching hash in1.387seconds, zero reported receive errors.

**Sustained bulk pass on this third call:** 30 sequential32768-byte downloads all completed HTTP200 with the exact expected hash,983040bodybytes total (960KiB), sum of probe durations257.493seconds, median7.787seconds. Final Windows connection duration264.520seconds, received1061515bytes/860frames, CRC14 and all other error counters0. Bulk body throughput across all probes~3.82kB/s; typical error-free transfers~4.2kB/s. Some probes slowed with CRC errors but all completed intact. This demonstrates a multi-minute V.90 data run beyond the prior failure point, while one failed startup out of these three attempts and the first call's failed retrain remain unresolved. Native process10057 used~1.5% of one CPU core at107seconds, RSS2896KiB; this observation does not validate multi-call scaling.

Post-bulk idle test failed to sustain the link. Observation started at Windows duration291.750seconds; keepalives continued for at least150seconds (duration441.785seconds), while CRC rose14->17 and alignment0->2. At duration471.789seconds counters had stopped advancing except CRC18. The subsequent public-page request timed out after30.017seconds with no received bytes. Server first caller retrain at startup469.014625seconds, followed by unsupported downstream4; recovery failed again. Thus continuous transfers passed for4.3minutes, but the call did not survive the subsequent idle interval. This single run does not establish idle as the cause rather than channel degradation over time. Native RX/TX10057 saved privately for comparison. Explicit API disconnect confirmed Disconnected; no active test calls remain. CT retains the experimental48k backend for further development; do not describe it as a reliable production service. Goal remains active: repeated startup success, prolonged data/idle stability and retrain recovery are still required, along with higher upstream rates and eventual multi-call validation.


## 48k pacing comparison and late-call timing audit (2026-09-09)

Previous turn was progress: authoritative live transfers passed and post-idle failure was captured. Revalidated current CT service and48k ceiling with no PPP calls before this experiment.

RX10057 offline Oerder-Meyr estimates over20-second windows starting raw40,100,200,300,400,440seconds are approximately -0.190,+0.011,-0.500,-0.164,+0.379,+0.267ppm. These limited measurements do not demonstrate a major upstream clock drift and do not justify an untested receiver timing change. They do not exclude short disturbances or downstream impairment.

Enabled the existing60ms RTP playout buffer while retaining48k ceiling. Attempts b9676eb6-dd10-46eb-9181-e2194c7b2719 (native10222) and b38a7f4b-6c3e-4d53-be6c-ef7ade83271e (native10235) both failed beforePPP. Both decoded Ja and the firstS/Sbar, began DIL, then received caller retrain at startup~14.55seconds without the secondS/Sbar. Retrain repeated Ja/firstS/DIL and eventually selected unsupported downstream4. This narrows these failures to completion of digital impairment learning, rather than absence of the initial S detector event.

Bounded CT Ethernet capture completed120seconds,9359captured/0kernel-dropped. First call's captured portion has2526incoming/2528outgoing RTP packets with no sequence or timestamp anomalies; outgoing interarrival18.622..21.487ms, incoming15.028..25.117ms. Second call's captured portion has2145incoming/2142outgoing, no sequence anomalies, only known initial120timestamp step for160sample packet; outgoing18.867..21.137ms, incoming11.338..28.130ms. The capture excludes some call boundaries and does not prove ATA analog playback. Pacing worked at CT egress in these intervals but neither call established PPP. No stability benefit can be claimed from this comparison.

Saved RX/TX10222 and final transport capture privately. Both API trial helpers and capture process terminal. Removed temporary v90-pacing.conf, daemon-reloaded/restarted service;48k ceiling and experimental native backend remain selected. No calls active. Next work should inspect DIL completion interoperability and retrain behavior using the saved successful/failed waveforms; do not loosen signal thresholds or change PCM gain based solely on these failures.


## Complete requested DIL waveform audit (2026-09-09)

Rechecked ITU-T V.90 clauses8.4.1 and9.3.1.5/6 against native DIL generation: segment lengths(Hc+1)*6, separate sign/training patterns restarted per segment, full-sequence repetition, and segment-boundary termination agree on inspection. Source: https://www.itu.int/rec/dologin_pub.asp?id=T-REC-V.90-199809-I!!PDF-E&lang=e&type=items .

Added research/v90/audit_dil.py. It obtains the first CRC-validated Ja from a private caller capture, reconstructs the requested PCM cycle independently of v90train_tx.c using G.711 codeword decoding, and compares full candidate cycles against the transmitted capture. Output is training metadata only. It does not validate analog delivery, start timing, or a changed descriptor in a later retrain; candidate suffix differences may be normal termination rather than corruption.

Actual requested cycle is147segments/17334samples/2.16675seconds. Failed paced capture10222 has exact complete cycles at raw15.154875 and17.321625seconds, and again during retrain at29.547375/31.714125. Successful10057 has an exact complete cycle at13.673125seconds before its normal termination, and exact cycles in its later failed retrain. No sample mismatch in any of those complete cycles. Independently decoded outgoing G.711 from the final CT RTP capture also matches both complete cycles of failed10222 exactly; encoding/packetization did not alter those samples at CT egress. This strengthens the evidence against a DIL segment/pattern generator fault for these calls, without proving the ATA received or reproduced the samples correctly.

Diagnostic checks: reference G.711 values for both laws verified; isolated17334-sample known-good cycle passes; flipping sample1000 produces exactly the expected rejection and mismatch position. Captures remain private outside git. Service revalidated active; no live call or runtime changes in this audit. Next useful evidence is delivery at the ATA-facing PBX leg and the relationship between channel changes and the caller's failed DIL/retrain, rather than arbitrary PCM scaling or detector threshold changes.


## ATA-facing packet audit and live receive-loss counters (2026-09-09)

Previous DIL audit was progress; current worktree clean at start. New unpaced48k attempts ae8ed4eb-667a-44bf-be16-156c5ccbd0f6 and9dc2841d-8666-4d38-8028-073a67017964 both failed beforePPP. FreePBX eth0 capture bounded120seconds completed16243packets/0kernel-dropped, covering the entire first call and initial20seconds of the second. Captures remain private.

First call:3040RTP packets per leg, exact ordered payload equality acrossPBX in both directions, no sequence gaps. Upstream retained known initial120timestamp step; downstream timestamps advance160 consistently. PBX relay delay downstream0.052/0.104/1.123ms min/median/max, upstream0.058/0.110/1.669ms. ATA-facing interarrival9.385..30.456ms. Afterhangup ATA Stream1 reported6lost, but those losses were not sampled during the first call and cannot be located in time.

Second call:1007packets per leg in the captured20second prefix, exact concatenated payload hashes acrossPBX, no sequence gaps, ATA-facing interarrival13.847..26.291ms. Sampled ATA RTP1.htm55times over~136seconds through training and teardown, storing only timestamp/counters. Counters reset to0 on the new call. At epoch1788969277.4516, Tx735/Rx733, PacketLostByNet became1; at1788969279.9720, Tx861/Rx858, it became2. It remained2 through the final post-call observation(Tx3044/Rx3038). Therefore these two increments occurred during the active training interval, not solely at teardown. Web-page response timing is approximate and counters alone do not distinguish physical network loss from internal late-packet/playout discards. PBX egress capture proves transmission, not delivery at ATA Ethernet ingress or analog output. This makes ATA receive behavior a concrete next investigation, without exonerating all DSP behavior.

Updated audit_rtp.py to skip cross-call comparisons when packet time ranges do not overlap. Previously repeated silence/training payloads produced irrelevant comparisons between successive calls on the same endpoint pair. Replayed the two-call capture: only the four appropriate bidirectional comparisons remain (3040 and1007packet pairs). No runtime setting changes. API helpers, counter sampler and packet capture all terminal; service remains in unpaced48k experimental configuration. Goal active, reliable startup/data recovery not achieved.


## Sparse ATA observation with paced RTP (2026-09-09)

Revalidated current firmware logs: recent calls still enter Voice->VBD on2100-PR-Net; no playout/starvation diagnostic appears at current log verbosity. ATA SSH22/Telnet23 reject TCP connections. Read-only examination of the previously saved firmware metadata finds internal coding-profile controls(nom_delay/max_delay/min_delay/adaptive_playout), but no verified exposed provisioning setting for them. The published ATA187 configuration guide does not supply such a control. Do not copy settings for ATA190/191/SPA models or execute unverified internal firmware commands. No firmware/device configuration modified.

Frequent HTTP counter polling might itself load this older ATA, so repeated60ms-paced48k trial fac3dc47-b60d-40a7-88f6-02ee34bd5e8f with just two counter reads. First read epoch1788969581.0917:Tx865/Rx861,loss0,jitter0. Second at1788969607.6115:Tx2191/Rx2186,loss1,jitter0. Both were during the active training interval; later API statusFailed, downstream4fallback. This does not establish HTTP polling as a cause, nor eliminate observer effects entirely, but loss reporting persists with sparse reads and paced egress. The ATA counter's exact distinction between on-wire loss and internal discard remains unverified. Saved metadata in private work/v90-ata-paced60-sparse.json.

Trial/helper and sparse sampler both terminal. Removed temporary60ms pacing drop-in, daemon-reloaded/restarted CTservice; unpaced48k experimental backend remains, serviceactive, noPPPcalls. Last turn's packet/counter evidence was progress; this turn narrows the observer/pacing explanations but does not fix reliability. Goal remains active. Further changes must be tied to a supported ATA control, authoritative packet-arrival evidence, or a demonstrated modem protocol defect.


## Previously ignored rate renegotiation found in stalled calls (2026-09-09)

Previous sparse-ATA turn was progress but did not repair reliability. Revalidated code and audited recovery against V.90 clauses8.6/9.6. **Concrete protocol defect:** data-mode phase4 continued decoding CP messages but never listened for S/Sbar or sent the required rate-renegotiation response. It could also overwrite the stored active CP while leaving the running encoder unchanged.

Private RX10057 contains S/Sbar at raw471.249875/471.264875seconds, followed by CRC-valid CP-prime(type1,drn16/48000bps,ack1,silence0) by472.94seconds, then sustained ToneA starting473.795seconds. RX9969 similarly has S/Sbar171.999875/172.009875, CP-prime by173.68, ToneA174.535. Thus a rate renegotiation precedes the full retrain in BOTH48k stalls; these are not merely late full-retrain events. The callback-count CP replay finds multiple CRC-valid messages, not an inference from a single tone. Prior full-retrain-only recovery ignored this earlier opportunity. ATA loss counters may describe an impairment but do not remove this software defect.

Added ordinary rate-renegotiation response in v90phase4.c: detect S/Sbar in data mode, clamp DTE consumption and upstream delivery, retain preceding dataCP, reset the CP/E receiver, and start Rd on the existing6-symbol frame boundary. Emit384Rd symbols using each preceding data interval's largest magnitude, then24 reversed symbols. Renegotiation TRN2d/MP/Ed use preceding data constellation/shaping with K retained from CPt; subsequentB1/data use the newCP. Reset upstream demapper while preserving callbacks; existing outer PTY/PPP process remains. Stop silently applying unsolicited CP metadata in data mode. Parse CP's silence bit; CPs silence procedure remains explicitly unsupported and awaits caller retrain rather than accidentally treating CPs as ordinary dataCP. Local initiation and renegotiation timeout handling remain incomplete.

New v90_pcm_renegotiate validates both constellations/capacity, resets encoder state, then combines preceding data parameters with trainingK. New private-capture regression tools/tests/v90-renegotiation.py replays initial training and the entire late event from RX10057: exactly one renegotiation, aligned384+24Rd waveform with correct magnitudes/signs, K12, no DTE consumption while clamped, and emitted TRN2d/MP/Ed/B1. This is open-loop replay; it does NOT prove the caller accepts the response. Existing full RX6711 initial Phase4 wire/MP/Ed/B1 regression, PCM32combinations, CP corruption/truncation plus independently CRC-built silence-bit case, startup/ranging/retrain regression, local andCT native builds pass. Audio framing100frames passes (first invocation used wrongcwd; corrected invocation passed).

New native binary deployed toCT105, unpaced48k retained. First hardwareattempt b91fe8a9-d0be-41a2-beb5-415107ace50a failed beforePPP, so provides no live recovery result. Further hardwareverification in progress.

Live trial4db8a489-e80c-490b-ad57-e2a0f9cfe85c (native10527/pppd10528) authenticated48k and fetched publicexample.com559bytes with correct hash in1.561seconds. Eight additional periodic requests completed in~0.87seconds each, through Windows duration185.877seconds; CRC rose0->10. **First live rate-response exchange:** S at phase4 time203.306750s, Sbar203.321750, alignedRd/Rd-bar complete203.373000, and validCP(type1,48k,ack0) at205.007750. No CP acknowledgement followed; caller later full-retrained at startup217.298750. Next HTTPprobe timedout30seconds, WindowsCRC12, no successful recovery. API disconnect confirmedDisconnected. RX/TX10527 and probeJSON savedprivately.

**Correction to initial99ef8fa implementation:** 8.6 changes the spectral shaping parameters for renegotiation, whereas8.6.5 retains the CPt constellation. The first implementation incorrectly used preceding data Ucodes as well as shaping, explaining a concrete possible reason MP was not acknowledged (not yet live-proven). Updated v90_pcm_renegotiate to retain CPt's maps andK and copy only preceding data Sr/S, lookahead and filter coefficients. Strengthened the replay regression with explicit CPt amplitude-set assertion {1244,3772,6140,8316}; it fails the initial implementation and passes the correction. Existing initialPhase4/PCMregressions stillpass; local/CTbuilds pass, correctedbinary deployed. Ordinaryrate recovery still needs a NEW hardwaretest with this correction. No claim that the open-loop replay proves recovery. CPs and local timeout/initiation remain unfinished.

Finalstate: correctednativebinarydeployed,unpaced48k,serviceactive;allcallsandtoolhandles terminal. Goalactive. Nextpriority is liveverificationofthisconcrete rate-renegotiation correction before further transport configuration experiments.


## First live rate-renegotiation recovery (2026-09-09)

RevalidatedCTservice/noPPPcall and sourceSHA256match for correctedv90pcm.c. Attempt d7dcae02-7500-4579-90df-f81cbb22cf85 (native10713/pppd10715, Windows0xE30000) authenticated48k at16:19:05UTC and completed initial publicHTTP200/559bytes/hashmatch in1.323seconds, errorcounters0. PeriodicPPP-bound public probes continue on this SAMEcall.

**Live recovery verified twice:** first S at phase4 time157.720750s, Sbar157.728250, Rd/Rd-bar complete157.779750, CP ack0 at159.396875 and CP-prime ack1 at159.463750 selecting140000/3bps(~46.667k). Ed complete159.467250, B1d sent, upstreamE159.533750. A second request at193.067250/193.082250 completedCP-prime194.817125, Ed/B1d194.820750, upstreamE194.887125 at the same rate. PublicHTTPprobes atWindowsduration165.538,186.418,207.298seconds andlater allcomplete. pppd10715 unchanged, no redial or newPPPauthentication. This verifies the corrected CPt-amplitude rate-response path restores bidirectional data in a live call. WindowsRAS linkBitsPerSecond remains48000 after renegotiation; nativeCRC-validCP is authoritative for the changed rate.

Longer stability/bulk/idle testing still in progress; this is not completion of the full goal. Initial-startup reliability, CPs, local recovery timeouts and broader acceptance remain unresolved.

Same call's twenty periodic public probes all completedHTTP200 with the expected559-byte body hash, through Windowsduration416.148seconds. Following those, thirty32768-byte controlled downloads all completedHTTP200 with expectedpattern hash:983040bodybytes,244.465seconds summed request duration, median8.002seconds (~4.0kB/s aggregate). Windowsduration691.624seconds at the last transfer; CRC18->21 duringbulk,alignment1unchanged, othererrorcounters0. pppd10715 remained unchanged. This is the first sustained post-renegotiation bulk pass, not just a carrier/handshake result.

Post-bulk idle acceptance FAILED. After three minutes without application traffic, the publicprobe timedout30.020seconds. Windowsduration931.129seconds, receivedbytes1054135(noincrease duringprobe), CRC1374/alignment37. pppd10715 logged four unanswered echoes at16:34:02UTC, connecttime15.0minutes, terminated16:34:12 and exited16:34:13. API disconnect returned terminalFailed; subsequentGETconnections wasempty. No activePPPcall remained. Temporarybulkserver stopped; SIPfaxactive.

Full log reveals an additional successful renegotiation duringidle: phase4S767.645750/Sbar767.653250, TRN2d767.704500, CP-prime769.388000, Ed/B1d769.392000, upstreamE769.457875. The FOURTH exchange failed: S828.567000/Sbar828.574500, TRN2d828.625500, CP ack0 at830.243875, noCP-prime, then fullretrain. There was no applicationprobe between the third and fourth exchange, so the third is a handshake/E completion rather than independently verified post-recovery applicationtraffic. Earlier commentary's 'third failed' was based on an incomplete log window; this full audit supersedes it.

OfflineCPmetadata(first two exchanges and failedfourth): type1,drn15, Sr1,lookahead1,gain8178,filter[63,0,0,0],silence0; firsttwo includeack1, fourthonlyack0. Initial12000TXsamples(1.5seconds) of all FOUR renegotiation TRN2d/MP emissions are sample-identical after aligning rawstarts174.066375,209.419875,783.991125,844.912125. This does not prove the remainder or analog delivery, but argues against an early repeat-count/state-dependent training waveform error. Do not compare phase4-relative timestamps directly with startup-relative retrain timestamps; their origins differ.

RX/TX10713,complete log and probeJSONsavedprivately. Alltesthandles terminal. This turn proves a substantive improvement: ordinaryrate renegotiation successfully returns to data and supports sustained bulk transfer, but prolongedidle/repeat recovery remainsunreliable. Goalactive; no overallcompletionclaim. No runtime/code changes beyond the alreadydeployed8f74afe in this verificationturn.

## Independent acknowledgement audit (2026-09-09)

Extended the TX10713 comparison beyond the common first 1.5 seconds. A private Python decoder independently reverses the captured four-point CPt mapping, sign shaping and GPC scrambling, verifies the initial 340 all-one training frames, and checks MP frame synchronization and CRC. This decoder assumes this call's K12/S5 and CPt levels; it is not a general decoder for arbitrary constellations.

The first three exchanges each contain 318 complete valid MP frames in the selected 13500-sample windows, with first acknowledgement 1.6185 seconds after TRN2d starts. The failed fourth exchange contains 552 complete valid MP frames in a 21912-sample window, with first acknowledgement at 1.623 seconds and acknowledgements continuing through 2.7345 seconds. No invalid MP CRC appears in those windows. Therefore the saved server output did not omit MP-prime or corrupt its checksum in the failed exchange. This does not prove the caller received the same waveform.

An independent Python receive frontend also finds only CP acknowledgement zero during the failed exchange: 28 CRC-valid messages in the inspected raw 845.8–847.7 second window, decoded timestamps 846.4589375–847.39525 across timing phases 3/4. A successful exchange's comparison window contains both acknowledgement values. This corroborates the native receiver's observation, rather than establishing a missed CP-prime as the explanation. It does not exclude a message outside the inspected window or a receive impairment shared by both decoders.

No runtime configuration changed or new call placed for this audit. CT105 SIPfax service is active. Remaining work still includes reliable startup, repeated/idle recovery, CPs and local recovery timeouts; the earlier successful internet transfers do not satisfy those acceptance criteria.

## E recovery when CP-prime is missed (2026-09-09)

The preceding acknowledgement audit was progress; it eliminated a raw transmitted MP checksum/omission explanation. Reinspection against V.90 9.4.1.4, also referenced by ordinary renegotiation 9.6.1.2.3, found another concrete omission: Ed may follow a received CP-prime OR a 20-bit E, after sending MP-prime. Our training receiver required CP's acknowledgement before detecting E, and the transmitter separately required CP-prime to leave MP. Thus the allowed E alternative could never recover a missed CP-prime.

Removed the CP acknowledgement prerequisite from E detection. Instead require a CRC-valid nonsilence data CP on the SAME receive timing lane; another lane's valid CP, pre-CP SCR, CPt or CPs cannot authorize E. The transmitter accepts E as the alternative to CP-prime, still requiring data CP and completing its first MP-prime before Ed. CPs remains unsupported. This fixes a standards omission, not a proven explanation for the failed fourth exchange.

New tools/tests/v90-e-recovery.py sends independently GPA-scrambled valid/corrupt CP bitstreams followed by 19/20/30 ones, exercises wrong-lane and pre-CP guards, and verifies a complete MP-prime precedes Ed. The new regression, full hardware RX6711 Phase4 wire/Ed/B1 replay, RX10057 late renegotiation replay, startup/ranging/retrain suite, local/CT full builds, 100-frame audio framing and diff whitespace checks pass. The test's initial independent scrambler recurrence was corrected to GPA taps 5/23 before passing. This is bitstream/state-machine and recorded-waveform evidence, not a live E-only recovery result.

CT105 had no pppd before deployment. Updated native sources built on CT successfully; unpaced48k configuration retained. Hardware attempt 881bcb02-a338-407f-b790-86d972b9432a started for verification.

That attempt ended Failed during startup/repeated retraining before PPP; it provides no live E-only recovery result. The helper is terminal, API connections empty, no pppd and service active. New code remains deployed. Next concrete incomplete protocol requirement is the digital-side renegotiation timeout and local full-retrain initiation; caller-initiated retrain already exists. Goal remains active.

## Digital-side renegotiation timeout (2026-09-09)

Previous turn was progress: deployed the E alternative with regression coverage. Added the missing 9.6.1 deadline: if no upstream E is recognized within 40000 samples plus twice the measured round-trip delay after the Rd/Rd-bar transition, initiate a full retrain under 9.5.1.1. The transition is 384 samples after Rd starts, not 408. Clamp the native data pump at the first timeout sample, reset training while preserving the outer sample clock and PCM law, emit 560 silent samples then Tone B, and reuse the existing Tone A/reversal ranging path. The surrounding PTY/PPP process is untouched. Negative measured RTD is treated as zero for this deadline. Received E cancels it. This timer does not yet cover initial startup, the pre-Rd S/Sbar wait, or general data-mode impairments.

Extended the startup regression with both PCM laws, RTDs -20/0/420/1280 samples, 157-sample processing chunks, exact deadline and 70ms mute assertions, E cancellation and no-renegotiation guards, DTE consumption checks, and a synthetic caller Tone A/reversal that resumes ranging with the required 40ms response. Full startup/ranging/caller-retrain tests, the E-recovery regression, native build, 100-frame audio framing and whitespace checks pass. These verify the protocol state machine, not successful live recovery after a channel impairment.

Service active and no pppd before deployment; unpaced48k configuration retained. Updated startup source is being built on CT105 for hardware verification.

CT build completed successfully. Hardware attempt 1b34c4e8-88e4-4dfa-afda-62c1855f44b0 ended Failed before PPP. Initial DIL reached its first S/Sbar transition, followed by caller full retrain; it did not enter data/rate renegotiation and therefore did not exercise the new timeout. Helper terminal, API connections empty, no pppd, service active. The regression-tested timeout remains deployed at 43c42dc. No claim of improved live startup or completed recovery acceptance. Initial DIL reliability remains the immediate obstacle to longer hardware verification.

## Controlled end-to-end A-law startup trial (2026-09-09)

The preceding turn made progress with a deployed, regression-tested timeout. To test a different supported PCM law without transcoding, temporarily set allow=alaw on only FreePBX endpoint sipfax and test line63416874. Both endpoints were idle beforehand. Original custom endpoint file saved under /var/backups/sipfax-v90-alaw-trial-20260909 on FreePBX. This Asterisk version has no `pjsip reload` command; `module reload res_pjsip.so` succeeded and live endpoint inspection confirmed only alaw. No ATA firmware/provisioning or SIPfax source changed.

Attempt c4093607-7597-4cf3-9ea5-cc07194e3ba7, native10992, ended Failed before PPP. Native startup explicitly used A-law. Both live Asterisk channels reported native/read/write alaw and no read/write transcoding. First DIL again reached one S/Sbar transition followed by caller retrain; A-law did not establish a connection in this trial. A single trial does not measure comparative success probability.

The bounded120s PBX capture completed12236packets with0kernel-dropped. All four RTP legs used payload type8,3041packets of160bytes each, no sequence gaps. The known initial upstream timestamp120-step persisted. Exact ordered payloads match acrossPBX in both directions. ATA-facing downstream interarrival16.347/20.014/23.492ms min/median/max. Two complete17334-sample A-law DIL cycles match the caller's CRC-valid Ja request at raw13.657625 and15.824375seconds, both in nativeTX and independently decoded outgoing RTP. This checks PBX-side payload/sample integrity, not ATA analog delivery or receiver training correctness.

After call termination and empty Asterisk channel list, restored allow=ulaw,alaw on both modified endpoints and reloaded res_pjsip.so; live endpoint inspection confirms restoration. Native timeout/E-recovery binary and unpaced48k remain deployed, SIPfaxactive, no pppd. Dial helper and packet-capture handles terminal. Private RX/TX10992, v90-alaw-trial.pcap and decoded wire samples remain outsidegit. This narrows the codec/transcoding explanation; it does not fix startup reliability or complete the goal.

## Missed DIL-ending transition: caller already sending CPt (2026-09-09)

Previous A-law experiment was progress. A proposed returned-echo timing audit found only weak correlation (~0.03–0.05), insufficient to infer analog timing or sample slips. Spectral inspection instead showed wideband upstream training where the server was still sending DIL. Independent Python CP decoding over raw16–18.7seconds finds 16 CRC-valid CPt in failed mu-law10222 (first17.6143125s) and67 in failed A-law10992 (first16.05825s). Successful10057 contains CPt too, as expected. Thus the earlier characterization as caller failure to complete DIL was incomplete: in these two failures, the caller HAD advanced to Phase4, and the server missed the preceding S/Sbar transition. This is a concrete receive/state-handling defect; it does not establish why the tone transition was missed.

Added a CPt monitor while DIL is active. It uses the otherwise unused Phase4 receiver storage before Phase4 initialization. A CRC-valid type0 CPt proves the caller has entered final training, so request normal segment-boundary DIL termination and enter Phase4. The regular S/Sbar path remains, and the monitor stops when that path has already requested termination. Phase4 initializes its normal receiver and receives repeated CPt before configuring transmission. No fabricated constellation or unsolicited data-mode transition is introduced.

New tools/tests/v90-dil-cpt.py replays CPt from both failed hardware captures with no S/Sbar present: DIL ends at a complete segment boundary and Phase4 receives CPt/TRN/MP; silence does not advance. Full startup suite including timeout/retrain, complete RX6711 Phase4 wire replay, native build, 100-frame framing and whitespace checks pass. These are open-loop recovery checks, not proof of a live established connection. CT had no pppd before deployment; updated startup source is being built there with original mu-law-preferred, unpaced48k configuration.

**Live recovery verified:** deployment built successfully. Attempt 1cc1e631-b93c-49b4-8b45-5d87aaf20ae4 (native11015/pppd11017) used the NEW path: first S/Sbar at startup8.883000s, CRC-valid CPt during DIL11.294875s, segment completion/Phase4 at11.303250s. Phase4 received repeated CPt, negotiated48k, completed Ed/B1 and received E. Windows connection0xE70000 authenticated with addresses10.64.0.2/10.64.0.1 at17:04:00.774UTC. Initial publicHTTP200/559bytes completed in1.348s with expected body hash and zero reported errors. Unlike earlier retries, this directly verifies the implemented missed-transition recovery in a live call. Further periodic probes are running on the same connection; overall startup repeatability and long idle/recovery acceptance remain unproven.

All six additional periodic public probes passed (~0.88–0.91s each), through Windows duration135.455s; CRC2, other error counters0. Probe helper terminal; pppd11017 remains live and SIPfaxactive. Connection0xE70000 is intentionally left established for continued long-duration/idle acceptance in the next goal turn; do not redial or restart it without inspecting this live connection first. No packet-capture process is active. Goal remains active, not complete.

## Sustained transfer, full retrain recovery and idle pass (2026-09-09)

Previous turn made progress by proving the CPt startup recovery on hardware. Revalidated the SAME connection0xE70000: public probe passed at Windows207.789s and pppd11017 remained live. Started the existing known-hash fixture on temporary PPP-bound port8082 and completed thirty32768-byte downloads, all correct hashes:983040bytes total,273.660seconds summed request durations, median7.982s. Last transfer Windows512.848s, CRC21/alignment1, other error counters0. No redial or new pppd.

Full current-call log (split at the final pipe-engine startup marker) shows actual recovery beyond startup: two ordinary rate exchanges completed at Phase4Ed138.659250/143.795250s. A later failed exchange led to caller full retrain at startup199.338125s, another at209.418125s, then CPt-assisted DIL completion220.544375s and a new Phase4Ed3.592500s. Data/public probes and bulk transfers subsequently passed on the same PPP process. A further ordinary renegotiation selected46.667k, and a later failed exchange led to caller full retrain3 at startup396.173125s; its Phase4 completedEd3.600750s selecting48k, followed by successful bulk transfers. This is the first verified return to working application traffic after FULL retraining while retaining PPP. Individual Phase4 timestamp origins reset at full retrain; do not directly subtract them from startup timestamps. No digital E-timeout firing appears in this inspected log.

After the bulk batch, a bounded180-second application-idle interval and public request PASSED: HTTP200/559bytes with expected hash in0.889s, Windowsduration723.260s, CRC33/alignment1, all other error counters0. This satisfies this call's three-minute idle test, unlike the prior failed idle test; it is not proof of indefinite reliability or repeatable startup. Temporaryfixture stopped, both probe/bulk/idle helper processes terminal. SIPfaxactive and pppd11017 still live; leave attempt1cc1e631-b93c-49b4-8b45-5d87aaf20ae4 / connection0xE70000 established for longer soak verification. Private JSON and current-call log saved outsidegit. Goal remains active.

Additional offline S-window audit: real short S in RX10057/10222/10992 admits two qualifying existing100-sample windows only for7/16/12 of100 clock alignments respectively under the current thresholds. This points to timing sensitivity in the detector, but a naive five-consecutive-overlapping-window variant also fails these captures and was NOT implemented. Any detector revision needs recorded-signal and false-positive regressions. No runtime/DSP change in this verification turn.

## Ramped S detector correction under test (2026-09-09)

Previous turn was progress: full-retrain and idle acceptance passed on the live call. Revalidated public HTTP at Windows812.839s and began20 further periodic probes on the same connection, retaining the deployed CPt-recovery binary for the soak test.

Offline inspection identifies why the fixed-window S coherence is fragile: the received S amplitude ramps sharply while its carrier phase remains stable. A longer200-sample coherent window reduces the score to~0.64–0.67; scanning common frequency offset does not improve it materially. Normalizing each20-sample block before the100-sample three-line coherence calculation removes amplitude-envelope variation from that decision. Keep the same0.85 total and0.10/0.20/0.10 per-line thresholds and two-window duration requirement; reduce the absolute RMS gate from100 to30 PCM units to include the early ramp while remaining above captured idle noise (~10RMS). This is a receive detector adjustment based on recorded waveform evidence; it does not assume a particular physical cause of the ramp.

New tools/tests/v90-s-ramp.py detects exactly one S and one Sbar for all100 timing alignments in each of three real recordings (300cases). No false S on410seconds of hardware data, CPt,20seconds of Gaussian noise or20seconds each of five individual tones. Existing late rate-renegotiation, startup/timeout/retrain, CPt fallback, E alternative, fullPhase4, nativebuild and100-frame framing tests pass. The CPt-only regression's old mu-law slice began inside the missed S/Sbar: the new detector exposed that fixture error. Move its start from17.5 to17.58s so it now actually excludes the tone transition and verifies CPt independently.

These changes are LOCAL ONLY pending completion of the established-call soak test. Do not confuse local build with deployed hardware: CT is still running the earlier f38c49e startup-recovery implementation. The live connection remains the verification priority before deployment.

Longer soak FAILED:16 additional public probes passed through Windows1188.028s, then requests beginning1208.037s and the next two retries each timedout30s. Receivedbytes remained1092907; finalWindows1338.114s, CRC2442/alignment41. Server continued receiving CRC-valid upstream PPP frames, so this was not merely a closed call. Neither old nor new S detector finds an S event in the inspected RX11015 prefix from raw1000s through the failure interval; do not attribute this late failure to the ramp detector without further evidence. Helper stopped at its three-failure bound. API disconnect confirmedDisconnected and connections empty. Complete RX/TX11015 saved privately after termination. The earlier successful full-retrain/idle tests remain valid, but they do not prove longer stability.

After teardown, built/deployed the normalized S detector on CT105. Fresh attempt78ae1119-0686-47c2-bb44-fff8297937d3 (native11245/pppd11246) authenticated48k at17:29:22.302UTC, connection0xE80000. Both NORMAL S/Sbar transitions were detected at startup8.914125/11.151625s; DIL ended11.158125s without the CPt fallback. Initial publicHTTP200/559bytes/hashmatch took1.520s with all error counters0. Two subsequent ordinary rate renegotiations completed at Phase4Ed25.188000/27.661500s, selecting46.667k; an additional public probe after them passed0.927s at Windows52.515s, CRC4/othererrors0. This verifies live normal startup with the new detector and post-renegotiation traffic, not a fix for the separate twenty-minute one-way failure.

Ended this short verification call explicitly to prepare repeat-startup testing: API Disconnected, connections empty; dial and probe helpers terminal. New detector remains deployed, original unpaced48k/codec settings retained. Next priorities: several independent startup attempts, then investigation of the saved late one-way failure. Goal remains active.

## Four independent startup attempts and late TX integrity audit (2026-09-09)

Previous turn made progress with the normalized detector and hardware verification. Revalidated deployed service/no pppd, then ran four separate calls, each followed by a PPP-bound public probe when connected and explicit teardown before the next. Results:6a1d789f-a917-4f4e-8904-e73df6ae4976 PASS48k/public1.809s;9e6a0b52-605a-4540-ad0a-d319dc950365 FAILED beforePPP (RAS678);fcae45f5-c63f-4963-be70-e39a436a4733 FAILED beforePPP;88cfc0af-83a6-4865-b67d-7658d8b45b53 PASS48k/public1.562s. Both successful responses have the expected559-byte hash. Thus2/4 startup attempts passed; repeatability is not achieved. Private report work/v90-startup-repeat-1788975113.json. Some failed-attempt API metadata remains Disconnecting after disconnect, but actual connections were empty before each subsequent dial; final successful disconnect is terminal, helper terminal, no pppd and serviceactive.

Both failed native logs detect both S/Sbar transitions, receive CRC-valid CPt and transmit MP, then the caller retrains and selects unsupported V.34 downstream4. They are no longer stuck in DIL from a missed transition. Failed native11357 TX independently decodes554 complete MP frames over2.7495s beginning raw16.395125s with0invalid CRC, ack0 as expected without received dataCP. Independent RX16–19.5s decoding finds3valid CPt and no type1CP. Saved RX/TX11357 privately. A sparse ATA read during the fourth call reports PacketLostByNet1; this counter still does not distinguish physical loss from internal discard and does not by itself explain the failures.

Separately decoded the late server TX from the prior twenty-minute failure, RX/TX11015. Its last relevant CP-prime at raw1000.1381875s selects48k/Sr1/36-code constellation. TX raw1200–1300s at the correct frame alignment contains24complete flag-delimited PPP frames, all24FCS-valid:17IP,1VJ-uncompressed,3VJ-compressed,3LCP. LCP echo requests continue near raw1229.284/1259.304/1289.344s and IP output continues during the failed public retries. This rules out absent or checksum-corrupted PPP encoding in that saved TX interval; it does not prove correct delivery through RTP/ATA/analog or diagnose the receiving modem. Payloads remain private; diagnostics print protocol/count/timing only.

No DSP/runtime settings changed this turn. Normalized S detector remains deployed. Next work must address remaining Phase4 acquisition failures and late one-way loss rather than claim the S fix made startup reliable. Goal active.

## Initial TRN2d duration experiment (2026-09-09)

Previous turn was progress: measured2/4 startups and verified correct TX framing during the late failure. Rechecked V.90 9.4.1.2/3: TRN2d minimum2040PCM samples; MP must begin within2000ms. Added optional SIPFAX_V90_INITIAL_TRN2D_MS (255..2000ms; default255, invalid values use default). Convert to whole six-sample frames by rounding down, keeping even the maximum below the2s limit. Apply only to initial Phase4; S-triggered rate renegotiation resets to the existing340frames/255ms. This tests acquisition time, not an established root cause, and does not change PCM levels, rate ceiling or codec.

Extended full captured-waveform Phase4 regression to independently decode1500ms of TRN2d, following MP/CRC/Ed/B1, as well as the default duration. Configuration boundaries and malformed/overflow values checked. Late rate-renegotiation replay under the1500ms environment still emits the original255ms training interval. Startup/timeout/retrain, E recovery, native build, framing100frames and whitespace checks pass. No active lm/pppd before CT deployment; full CT build passed. Temporary /etc/systemd/system/sipfax.service.d/v90-initial-trn.conf selects1500ms; daemon-reloaded/restarted, serviceactive. Four-call hardware comparison started, report work/v90-startup-repeat-1788975718.json.

**Four of four hardware attempts passed** with the1500ms initial interval: a5a3cc3f-d554-4b0f-b098-85b7c9456d2c,56296e59-ae3f-4432-981b-37601bdf5296,e910e7cd-75bf-418e-8c44-d36fc48ea82f,94227bce-ae36-41bf-8ed4-854389335152. Each authenticated48k, fetched the expected559-byte public page with correct hash, and explicitly disconnected with no remaining connection. Live logs confirm TRN2d-to-MP duration1.500s. Compared with2/4 at255ms in the preceding batch, this is encouraging interoperability evidence, not a statistically established success rate or proof of the underlying cause. Retain the1500ms drop-in for further testing; compile-time/default behavior remains255ms and rate renegotiation remains255ms.

Batch helper terminal; final API disconnect/empty-connection checks passed, no lm/pppd and serviceactive. No long-duration test of this setting yet; the separate prior twenty-minute one-way failure remains unresolved. Next: confirm startup over more independent calls and capture the RTP path during another longer session so late failure can be tied to actual transport/PCM events. Goal active.

## Longer 1500ms session with PBX RTP capture in progress (2026-09-09)

Previous turn made progress with4/4 short startup passes. Revalidated configuration, idle server and FreePBX disk capacity. Started a2100-second bounded tcpdump on FreePBX eth0, snaplen256 (complete160-byte G.711 RTP packets), restricted to UDP involving CT105 or this ATA. Remote file /tmp/v90-long-1500.pcap; execution handle2690 is running. Do not restart this capture merely because a poll returns no output.

Attempt4c2c3397-cdaf-496e-aa15-194b3a71f452, native11794/pppd11795, connected48k at17:47:15.215UTC, Windowsconnection0xF10000. Initial public request passed1.836s/559bytes/correct hash, zero reported errors. This is a fifth consecutive startup/public pass with1500ms initial training. Dial helper terminal. Long-probe helper handle6702 runs90public requests with20-second gaps and a three-consecutive-failure stop; private output work/v90-probes-0xF10000.json. It samples ATA counters at most once per300seconds and records an observation epoch for correlation. First ATA read:Tx2590/Rx2586/PacketLostByNet3/jitter0 while public traffic still worked. Counter meaning/observer limitations remain as previously described.

Private copied prefix v90-long-1500-prefix.pcap contains4621upstream and4620downstream packets per corresponding PBX leg, no sequence gaps, exact ordered payload equality acrossPBX bothways. First RTP sequences: ATA17372,PBX-to-CT27372,CT0,PBX-to-ATA2713. This capture will allow checking later sequence rollover and timing without attributing the earlier failure to them speculatively. Native recordings and the current call must remain intact for correlation. Both capture2690 and probe6702 are live; no runtime/DSP change in this turn. Goal remains active.

Resumed both live handles without restarting. At Windows363.933s all17periodic probes pass. Sparse ATA counter increased3->9 while traffic remained usable; counter alone does not establish failure. Extended research/v90/audit_rtp.py to report absolute/relative epochs of normal sequence rollovers separately from sequence anomalies, timestamps of the five largest arrival intervals, and exact ordered payload comparison (in addition to content multiplicity). Hand-constructed wrap/gap/reordering cases pass, including reordered equal-content packets that the old multiplicity count could not distinguish; real prefix still parses. These diagnostics will support correlation when the capture completes. Runtime remains unchanged and the two long-test handles remain live.


## Reproducible native launcher and continuing long-call verification (2026-09-09)

Resumed the existing capture2690 and probe6702; both remain live. All51 periodic public requests through Windows1109.034s passed HTTP200/559bytes with the expected body hash. Last CRC216/alignment7, other reported error counters0. The call has recovered full retraining and remains usable, but has not yet completed the long test or excluded the previous late one-way failure. No runtime setting changed.

The copied RTP prefix covers875.982seconds:43800 packets per upstream leg with exact ordered payload equality; downstream has43800 input and43799 output, identical through the entire43799-packet common prefix. The single unmatched trailing input is at the live-copy boundary, not evidence of an internal missing packet. No sequence gaps. PBX-to-CT sequence wraps normally at epoch1788976766.578475 (relative763.281757s); later public traffic passed. Both upstream legs share an initial120-sample timestamp increment; subsequent increments are normal. Largest observed interarrival is~43ms on all four legs at relative180.884s. These observations do not identify the source of ATA discard counters or establish analog delivery.

Added bin/sipfax-linmodem and README build/configuration instructions for the experimental backend, including the tested48k/4.8k scope and unresolved reliability limitations. Launcher resolves the repository native binary, enables framed audio with -P and forwards arguments; no capture is enabled. Local100-frame protocol check passes, diagnostics remain on stderr, and whitespace check passes. Copied launcher to CT105 and verified its SHA256 matches the local file. Persisted modem.command still uses the private capture wrapper for this ongoing test; the installed launcher is available for subsequent normal deployment. Goal remains active.


## All four RTP rollovers passed on the same long call (2026-09-09)

Previous turn was progress (launcher committed/deployed and live evidence). Revalidated running handles6702 and2690; both remain live. Same attempt4c2c3397-cdaf-496e-aa15-194b3a71f452/native11794/pppd11795. All62 periodic public requests through Windows1340.547s (~22m20s) have HTTP200,559bytes and the expected hash; latest observation epoch1788977369.2658565. This passes the earlier call's twenty-minute failure point but does not establish that separate fault is fixed. At Windows1319.667s CRC227/alignment7; other error counters0. Sparse ATA sample at1276.385s reports Tx65117/Rx65076/PacketLostByNet40, jitter0; traffic still passes.

Updated live-copy capture covers67798 ATA input packets; other legs67797. All67797 downstream payloads match in order and the67797-packet upstream common prefix matches; unmatched final upstream input is again the live-copy boundary. No sequence gaps on any leg. Normal wraps now captured on ALL four legs: PBX-to-CT epoch1788976766.578475, ATA-to-PBX1788976966.578589, PBX-to-ATA1788977259.759855, CT-to-PBX1788977314.01978. Successful public traffic after the last wrap rules out an unconditional first-rollover failure on this call. It does not retroactively identify the earlier failure without its missing packet capture. Initial upstream120-sample timestamp increment remains the only timestamp anomaly.

No runtime or DSP changes. Reviewed the outstanding CPs path against V.90 9.6.1.2.4–6/8.6.4: it remains explicitly unimplemented in v90phase4.c; do not claim full conformance. Existing timeout/retrain recovery and ordinary rate changes do not substitute for this procedure. Preserve the active call/capture for completion of the90-probe/2100-second bounds before any deployment. Goal remains active.


## Hardware verification of server-initiated timeout recovery (2026-09-09)

Previous turn was progress: all four RTP rollover boundaries passed. Revalidated both live execution handles; no restart. Full current-call log inspection (split at the final pipe-engine marker) reveals an important previously unreported event: local E-timeout recovery DID fire on this call. After rate S at Phase4 143.875750s, Sbar143.883250s, Rd/Rbar completion143.934750s and MP144.189750s, CP-prime145.551375s/Ed145.559250s were received/transmitted but upstream E did not follow. Native startup initiated retrain3 at684.010875s with the timeout reason. Caller answered Tone A; normal probing/Ja/DIL followed, new Phase4 CP-prime3.596625s, Ed3.600750s and receivedE3.666625s at46.667k. CRC-valid upstream PPP frames resumed.

This verifies the server-initiated recovery path on actual hardware, not only the previously observed caller-initiated retrains. The same PPP process11795 and Windows connection0xF10000 remained established. Probe30 completed at Windows678.976s in13.312s with expected public body hash; probes31/32 passed0.865/0.867s at699.856/720.736s, with later probes continuing. Startup, raw call and Windows clocks have different origins; do not directly equate their numeric timestamps.

Current call log through about1430 rawseconds has16 S-triggered rate exchanges and5 full retrains (one local E timeout, four caller requests). Thus successful application traffic depends on repeated recovery; this is not a clean error-free channel. No CPs event appears. One later public request took15.586s across further recovery. All68 inspected periodic requests have HTTP200/559bytes/correct hash. Native11794/pppd11795 remain live at elapsed25m22s/24m56s, nativeCPU1.7%, RSS2768KiB. Long helper6702 still runs to90probes; capture2690 still runs to2100seconds. No DSP/runtime changes this turn; preserve both handles and live recordings. Goal active.


## Captured late one-way failure and PPP timeout (2026-09-09)

The previous turn was a verified wait. The long test subsequently FAILED: first75 periodic probes passed, last at Windows1628.431s; probes75–77 (zero-based) timed out30seconds each at Windows1678.443/1728.485/1778.527s. Receivedbytes froze at88027; CRC rose1271→2681→3621, alignment30→53→85. Helper6702 exited normally at its three-failure bound (78total requests), not90successful probes. No bulk test started. A subsequent API connections read returned[]; attempted probe returned404, confirming that connection was already gone. Native11794 and pppd11795 are absent, SIPfaxactive.

pppd11795 journal gives the termination reason: at18:16:42UTC no response to4echo-requests; serial link declared disconnected after29.5minutes, termination18:16:47 and exit18:16:48. Native had continued receiving repeated CRC-valid51byte upstream frames during the one-way period. It eventually detected a late rate request, then caller fullretrain6 at startup1784.700875s; new Phase4 CP-prime3.604375s/Ed3.608250s/E3.674375s and valid upstream frames followed, but PPP had already timed out. Therefore the existing training timeout recovery cannot help while the modem stays in nominal data mode; waiting for caller recovery was too late here.

Stopped the exact tcpdump process with SIGINT AFTER call termination. Capturehandle2690 is terminal:362999packets captured,363000receivedbyfilter,0kernel drops. Saved complete private RX/TX11794, final pcap and final audit. Four RTP legs each contain90412 G.711 packets, no sequence gaps, exact ordered payload equality in both directions throughFreePBX. The initial upstream120-sample timestamp step is the only timestamp anomaly. Largest interarrival~43ms occurred much earlier (relative180.884s); a later~37ms interval at1625.320s alone does not prove the failure cause.

Independent vectorized G.711 conversion compared native RX and TX against corresponding packet audio: ALL14465024available samples match exactly in both directions. Packet audio has14465920samples;896trailing native samples were not flushed when the process terminated, so comparison correctly covers only the available prefix. This excludes missing/altered native-to-RTP samples and PBX payload corruption across the recorded failure. It does not prove ATA playout or analog delivery. Private helper work/check_v90_capture_delivery.py emits counts only; recordings stay outsidegit.

Next concrete recovery work: inspect/passively track outgoing LCP echo requests and matching CRC-valid incoming replies, so a sustained unresponsive downstream can request local fullretraining before pppd's four-echo deadline. The existing v90_serial_bit and v90_ppp_frame boundaries can observe actual transmitted/received PPP without changing PPP ownership. Any watchdog needs bounded state, frame/CRC/identifier guards, no triggers during ordinary training, and a cooldown/rearm policy; do not substitute arbitrary idle silence for evidence of a failed link. Root-cause investigation of the captured one-way fault and CPs/full-rate work remain open. No live call or test/capture handle remains; runtime is unchanged48k/1500ms with private capture wrapper. Goal active.


## Passive PPP echo recovery deployed; hardware soak started (2026-09-09)

Previous turn was progress: captured the failure and established pppd's four-echo timeout. Added bounded v90echo.c/h observer, hooked at outgoing serial-byte consumption and CRC-valid incoming PPP frame delivery. It parses only bounded headers and validates framing/FCS; no PPP payload is logged or modified and pppd still owns negotiation/echo generation. A matching reply first establishes peer support. At least two requests separated by10seconds,40seconds from the first unanswered request and10seconds from the latest counted request permit one local fullretrain. Unknown/stale identifiers, malformed/CRC-invalid frames, looped magic numbers and unsolicited replies cannot arm/rearm it. Training clears outstanding observations. A fired watchdog stays latched until a new matching reply, preventing repeated retrain loops. Deployment's echo interval30seconds/failure4 leaves recovery time before PPP termination.

New startup API permits this request only in data stage4 with upstreamE received; it reuses existing70ms silence/ToneB/fullretrain behavior. Synthetic PPP regression covers framing/escapes/FCS, peer support, boundary timing, repeated/wrapped identifiers, malformed/stale/loop replies, training pause and single-shot rearm. Extended startup test verifies data-state guards, one retrain,70ms mute and no DTE consumption during retrain. Complete startup suite, E recovery, native build, framing100frames and whitespace pass. README documents scope and timing. Idle CT105 verified before deployment; CT native build passed. No service restart or rate/codec change.

Fresh hardware attempt bbe11f5e-5d4d-4c81-9da7-1dc983a98713, native11992/pppd11993, Windowsconnection0xF20000 connected48k at18:25:22.003UTC. Initial publicHTTP200/559bytes/expectedhash passed1.394s with all reported errors0. Full current-call log includes PPP echo health monitoring armed, proving the observer sees matching real echo traffic. This does not yet verify failure-triggered watchdog recovery. Dial helper27980 is terminal and leaves the call connected.

New periodic helper75386 runs90public probes with20second gaps/three-failurestop, file work/v90-probes-0xF20000.json. New capturehandle29670 runs2400seconds onFreePBX /tmp/v90-echo-soak.pcap, started AFTER this call was already established; do not assume it covers startup or aligns with native sample0. Both handles live; native private RX/TX11992 retain full-call samples. Goal active, late one-way root cause and broader conformance/rates unresolved.


## Echo deadline edge case and early caller hangup (2026-09-09)

Previous turn was progress: watchdog deployed and armed on hardware. Revalidated live handles75386/29670, then added a regression for10-second requests arriving immediately before deadline polling. It reproduced indefinite deadline postponement because every request updated the last-request clock. Fix locally: only the second sufficiently separated request establishes that clock; later requests cannot move recovery later. Existing30-second deployment behavior is unchanged. Echo suite, local native build and whitespace checks pass. README clarified second counted request. This correction is LOCAL ONLY; CT still has99c7406.

The hardware call0xF20000 then failed early. Five periodic requests passed (lastWindows180.099s), the sixth timedout30s at230.111s withCRC786/alignment23, then two API probes returned404 as the call had ended. Helper75386 terminal after8total observations. Native11992/pppd11993 absent; serviceactive. pppd journal reportsSIGTERM at18:29:16, not four unanswered echoes. Current native log shows echo monitoring armed and caller retrains atstartup161.388875/250.053875s, but NO watchdog trigger. Late retrain ended during Phase2. Packet capture establishes ATA-initiated SIP BYE at epoch1788978556.963522, forwardedPBX→CT1788978556.966094. Do not attribute this hangup to watchdog timeout or claim watchdog recovery passed.

Stopped exact tcpdump after termination;29670 terminal,31634captured/received,0kernel drops. Saved work/v90-echo-soak.pcap (starts mid-call). No active call/probe/capture remains. RX/TX11992 remain onCT and are not overwritten by laterPID recordings. No bulk test started.

Additional independent audit of PREVIOUS failed11794: decoded CRC-valid dataCP-prime at raw1624.3059375s, drn15/Sr1/32codes. Correct TX six-sample alignment is2. Raw1630–1780s contains38complete flag-delimited PPP frames, ALL38FCS-valid:25IP,2VJ-uncompressed,6VJ-compressed,5LCPecho-requests. Echo request rawtimes1649.303536/1679.323786/1709.344036/1739.383786/1769.404036. Timing conversion uses actual35bitsper6samples (46.667k), not initial48k RAS display. Private helper work/audit_v90_tx_11794_metadata.py and metadata report saved. Combined with exact packet/native sample match, this confirms correctly encoded PPP was transmitted through the observed digital path during the older one-way failure; analog/ATA playout and interoperability remain unproven.

Next: deploy pending cadence correction while idle, continue hardware recovery verification and investigate late impairment. Goal remains active.


## Fixed watchdog deployed and recorded-failure timing verified (2026-09-09)

Previous turn was progress: fixed cadence starvation and traced early ATA hangup. Revalidated idle CT105, pushed91d63ba v90echo.c and rebuilt native successfully. No service restart or codec/rate change. CT now includes the fixed second-request deadline.

Independently decoded the earlier11794 real echo reply from RXraw1648–1652s and five transmitted LCPrequests from the checksum-valid TX stream. Replayed only those control frames into the current observer, preserving original PPP headers/FCS and escaping them for the TX-byte interface. Matching recorded reply arms it. With subsequent requests unanswered, observer does NOT fire one sample before its deadline and DOES fire at raw1719.344s,40.020214s after first missing echo1679.323786s (the actual second request arrives30.02025s later). This is before the recorded late PPP termination. Reply arrival is conservatively placed at end of its decoded window, before the next request; exact arrival does not affect the missing-request deadline. Private scripts work/extract_v90_11794_echo.py and work/replay_v90_11794_echo.py emit metadata only. This demonstrates detector timing on recorded traffic, not closed-loop recovery or a fix to the underlying physical impairment.

Started PBX tcpdump BEFORE the next dial, /tmp/v90-echo-fixed.pcap,2400secondbound,handle25884 live. Fresh attempt2262cb96-f6d2-4557-9609-f7a47cbede45, native12067/pppd12068, Windows0xF30000 connected48k at18:34:35.259UTC. Initial publicHTTP200/559byte expectedhash passed1.491s with all error counters0. Dial helper43443 terminal; call remains established.

Periodic90-probe helper37671 now live on0xF30000, file work/v90-probes-0xF30000.json,20secondgaps/three-failurestop with sparseATA observations. Keep this call and capture running; do not restart from a polling timeout. Native RX/TX12067 capture the whole call. Goal active; successful watchdog-triggered hardware recovery remains unverified.


## Controlled single-packet loss exposes fallback failure (2026-09-09)

Previous turn was a verified wait. Revalidated live probe37671/capture25884. Currentcall0xF30000 initially passed16periodic public requests throughWindows376.040s, withCRC13/alignment0. Changed this run explicitly from an unmodified soak into a controlled loss experiment; do not present later results as spontaneous behavior or a completed90-probe soak.

Confirmed current RTP leg PBX19242→ATA5012 from capture. Installed a temporary OUTPUT rule restricted to source192.168.1.29:19242/destination192.168.1.235:5012 UDP, token bucket1/day burst1, comment sipfax-v90-onepacket-12067. A shell cleanup trap covered removal. Installed at epoch1788979269.071213, after1second counter showed exactly1packet/200IPbytes dropped; removed by1788979270.087446, OUTPUT restored to sole policyACCEPT. No other call/traffic flow matched. Private rule/counter record work/v90-onepacket-12067.txt.

The modem responded with caller fullretrain1 at startup421.781s, then repeated retrains, eventually selecting unsupported V.34 downstream4/UINFO69 every~4.85s. No PPP-watchdog trigger appeared because the modem was already training rather than stalled in data mode. Two public requests timedout30seconds atWindows427.915/477.927s; receivedbytes froze20410,CRC14/alignment0. This response differs from the spontaneous high-CRC data-mode failures and does not validate or disprove watchdog recovery. Ended the unrecovering test explicitly through API; statusDisconnected. Third probe endedConnectFailure at520.748s;37671terminal. Do not call that third observation an independent natural failure because teardown was requested.

Capture25884 stopped after disconnect:109920captured/received,0kernel drops. Final privatepcap work/v90-echo-fixed-final.pcap/audit saved. Exactly27377packets on each upstream leg and CT→PBX,27376PBX→ATA. The ATA-facing leg alone has one sequence step2/timestampstep320 atrelative426.060716s, confirming the intended single160-sample packet omission. All other sequence increments normal; initial upstream120-sample timestampstep remains. Serveridle,nolm/nopppd,SIPfaxactive, all current helper/capturehandles terminal. NativeRX/TX12067 remain onCT. Fixed watchdog build91d63ba remains deployed.

This test establishes inadequate recovery when this client selects V.34 after an impairment; it does not establish the cause of the earlier on-wire-loss-free failures. Future work must keep these two failure modes distinct. Goal active; watchdog-triggered closed-loop recovery and long stability remain unverified.


## Buffered RTP comparison after startup fixes (2026-09-09)

Previous turn was progress via controlled loss/fallback evidence. Closer inspection qualifies that test: rate S/MP exchange was already underway BEFORE the dropped packet (S near raw424s; missingpacketrelative426.060716s). Therefore it interrupted renegotiation, not clean steady data; do not attribute the preceding S to injected loss. First subsequent fullretrain reached CPt and MP but caller requested another retrain beforedataCP; next stopped inDIL, thenV.34fallback. No newly demonstrated malformed native signal from this log alone. Complete private RX/TX12067 copied locally.

Revalidated idleCT and enabled existing60ms RTP pacer in /etc/systemd/system/sipfax.service.d/v90-pacing.conf (Environment=SIPFAX_RTP_PLAYOUT_MS=60),daemonreload/restart/serviceactive. Keep48k ceiling,1500ms initialTRN and fixedwatchdog91d63ba unchanged. This is a controlled new comparison because prior60ms trials failed inDIL BEFORE CPt/S-detector and longer-initial-training fixes. Do not claim buffering is already a cure.

Capturehandle79248 live,2400secondbound,FreePBX /tmp/v90-paced60-echo.pcap,startedbeforedial. Attempta007386d-8cc9-48d6-bdb4-3f9ab61cc0aa, native12191/pppd12192, Windows0xF40000 connected48k at18:46:16.657UTC. Initial publicHTTP200/559byte expectedhash passed1.570s, all reported error counters0. Dial helper61633terminal; call remainsestablished.

Copied prefix covers5427upstream/5424downstream packets per pairedleg, no sequencegaps, exactorderedpayloadmatchbothdirections. Incoming intervals13.526..26.546ms, CTegress17.606..22.551ms, ATA-facing15.990..22.676ms, medians~20ms. This verifies smoothing in this prefix, not full-call delivery or analogplayout. Prefix files work/v90-paced60-echo-prefix.pcap and-audit.jsonl.

Started temporary PPP-bound known-hash fixture via v90-transfer-check.service, Usernobody/RuntimeMaxSec900,HTTP8082bind10.64.0.1. Bulk helper97068 live,30requests of32768bytes, filework/v90-bulk-0xF40000.json. Firsttransfer PASSED32768bytes/hash e11360251d1173650cdcd20f111d8f1ca2e412f572e8b36a4dc067121c1799b8 in8.823s atWindows104.620s,CRC5/othererrors0. No periodicprobe helper currentlyrunning; avoid overlapping probes with this batch. Afterbatch,stopfixture and testpublic/idle/longrecovery on SAMEcall. Keepcapture79248 andbulk97068 as authoritative livehandles. RuntimeNOWuses60mspacing;restoreonlywhenidleifcomparisonfails. Goalactive.


## Watchdog-triggered full recovery verified on hardware (2026-09-09)

Previous turn was progress: pacedstartup/firstbulkpass. Resumedbulk97068/capture79248 without restart. Bulkbatch completed30attempts:28successful32768byte/hashverified transfers (917504verifiedbytes), one partial24820byte IOException and one30second timeout. Median successfulrequest7.9185s; summedallrequestdurations359.732s. Some successfulrequests required19.504/17.403/29.489seconds. Two ordinaryrate exchanges initially recovered; a local E-timeout fullretrain also fired at startup314.369s. Servicejournal inspected for this call had0pacer-issues; no kernel/transport claim beyond capture evidence.

Spontaneous one-way loss reproduced WITH60msbuffering: transfer27(zero-based) failedpartial atWindows403.650s, CRC464/alignment13; nexttimedout at433.693s, receivedbytes986914frozen,CRC999/alignment21. Therefore buffering is not a sufficient reliability fix. Keptcallconnected for watchdog observation rather than redialing.

**First verified hardware PPP-watchdog recovery:** native initiated fullretrain2 atstartup454.020s with explicit unansweredPPPe echoes reason. Caller repliedToneA; RTD117.500ms. NormalINFO1a(up4/down6),Ja,DIL/CPt and1500msTRN followed. NewPhase4 CP-prime3.673250s,Ed3.678000s at48k,upstreamE3.810125s, thenCRC-validPPP. Echo replies resumed/rearmedwatchdog at newupstream6.076875s. Same native12191/pppd12192 andWindows0xF40000 throughout. Finalbulkrequest29 recovered andPASSED32768bytes/expectedhash in22.512s atWindows456.216s. This is closed-loop recovery from the spontaneousfailure, not just synthetic/replaytiming. It does not remove the originalimpairment or prove indefinite reliability.

Bulk97068terminal. Stoppedtemporaryv90-transfer-check.service. Subsequent publicHTTP200/559byte expectedhash PASSED1.009s atWindows480.180s,CRC1001/alignment22; private work/v90-paced60-after-watchdog.json. Started a180second applicationidle test at epoch1788980078.6680148, handle85882live, then onepublicprobe savedto work/v90-paced60-idle-after-watchdog.json. Do not start overlapping probes duringidle. Capture79248remainslive until2400secondbound. Call/native/pppdremainestablished; currentconfig60mspacing/48k/1500ms/fixedwatchdog unchanged. Afteridleoutcome, continuelongerverification on SAMEcall. Goalactive.


## Post-watchdog idle passes; longer same-call test resumes (2026-09-09)

Previous turn was progress: hardware watchdog-triggered retrain restoredbulk/publictraffic. Revalidated idle85882/capture79248 aslive, preserving the intended idleperiod. Idle85882 is nowterminal:180seconds applicationidle thenHTTP200/559byte expectedhash in0.994s, Windows689.842s,CRC1013/alignment23, othererrors0. Same0xF40000/native12191/pppd12192, no redial/restart. Fixture remainsstopped.

Started75periodicpublicprobes with20secondgaps andthree-failurestop on SAMEconnection, filework/v90-probes-0xF40000.json. This extends past the earlier20–27minute failure windows; it is not a completedtest yet. Existing2400secondcapture79248remainslive. Runtimecontinues60mspacing,48kceiling,1500msinitialTRN,fixedwatchdog. Goalactive.

Read-only GitHub check found oneopenPR,20 LKMA-197 Phase3a HDLCframer/ptybridge, twoJunecommits. Its described scope is the olderV.21softmodem path, notV.90 or a higher-rate datapump. Its commits are not ancestors/patch-equivalent toHEAD, while currentbaseline already contains anHDLCptyimplementation through subsequentwork; no blindmerge orPRclosure performed. CurrentV.90developmentbranch has59commits beyond localorigin/main and remainsunpublished; release integration stillneedsreview. This branch check does not alter the live modemtest.


## Native engine selection and continuing recovered-call verification (2026-09-09)

Previous turn was progress via postrecoveryidlepass. Revalidated periodic19164 andcapture79248; both remainlive. Twelve periodic requests throughWindows994.209s have completed; all responsehashes checked. One took20.744s, laterrequests~1s. Same0xF40000/12191/12192 after priorwatchdogrecovery; no restart. Runtime60mspacing/48k/1500ms unchanged.

Added explicit linmodem engine selection LOCALLY: new purecommandresolver chooses /opt/sipfax/bin/sipfax-linmodem or SIPFAX_LINMODEM_BINARY, preserving highest-priority storedcustomcommand and independent spandsp/slmodem overrides. index.js uses resolver pernewcall. Environmentseed SIPFAX_MODEM_ENGINE=linmodem defaultsmodulationv90; storedconfiguration remainsauthoritative. README/envexample documentselection andinitialtraining setting. Admin remainsexistingcredential/concurrencyUI; no unsupported claim of a newmodemselector. This removes the requirement to supply a customcommand for normalnative deployment while retaining privatecapturewrappercompatibility.

Bundled localNode is available at /home/ncolasumaras/.cache/codex-runtimes/codex-primary-runtime/dependencies/node/bin/node. Native-selection/configuration/multiline/admin/deploymentchecks:15tests pass; indexsyntax andwhitespacepass. No need to useCTfor these tests. Changes remain LOCAL ONLY pending idledeployment, so they cannot affectthislive modemtest. Goalactive.


## Development branch published for review; long call still active (2026-09-09)

Previous turn was progress via nativeengineselection. Revalidated probe19164/capture79248 aslive and retainedruntime. Currentcall log also shows E-timeout fullretrain3 atstartup828.808875s; applicationtraffic continuedafterward. All25periodicpost-idlepublicrequests verifiedHTTP200/559bytehash throughWindows1268.844s (~21m09s). No newPPP-watchdog event beyond the previously verifiedrecovery.

Release review: fetchedorigin,mainhad0newcommits relative tobranchbase; developmentwas61commitsahead. Full localNode suite50/50passes. In-memory scan of466389bytes ofbranchhistorypatches foundnoneofcurrentDialUpLab/admin/PPPcredentials. Fullprivatecaptures remainoutsidegit; committedbinaryfixtures arepredatahandshake/constellationmessages. This is a targetedcredentialcheck, not an unlimitedsecurityclaim.

Pushedcodex/v90-development andcreateddraftPR29: https://github.com/nicolasumaras/SIPfax/pull/29 . VerifiedOPEN/isDrafttrue andhead5001671105ab92f99fafbb2a6a2539e7301fb33d matchinglocalHEADatthetime. PR describes48k/4.8k scope, realwatchdog/bulk/idle evidence, partialbulkfailures, unresolvedimpairment andCPs/fallback/higher-upstreamlimitations. No mergeperformed. Nativeengine-selectionJSchangesstillawaitidledeployment; liveCTremainsfixednativewatchdogwith60mspacing/1500msTRN/privatecapturecommand. Continueexistingcall0xF40000/12191/12192 andhandles19164/79248. Goalactive.


## Paced call ends in unsupported fallback; native CI coverage (2026-09-09)

The same call 0xF40000/native12191/pppd12192 completed 27 periodic public requests through Windows1310.804s, followed by two RequestCanceled responses (1360.816s and1412.391s) and ConnectFailure. Received bytes remained1057303, CRC1048/alignment23 during both timeouts. Earlier watchdog recovery remains verified, but this is not a long-duration reliability pass.

Last rate renegotiation received a CRC-valid48k CP without completing the exchange. Caller retrain4 at startup1340.758875s reached Ja and DIL, then caller retrain5 at1351.253875s abandoned another V.90 attempt. From retrain6 at1357.203875s INFO1a repeatedly requested downstream4/UINFO69 (unsupported V.34 fallback), through retrain24. PPP terminated on signal15 at19:10:13UTC after24.0minutes; no second PPP-watchdog recovery occurred.

Stopped the exact capture process after call termination; handle79248 terminal,295096packets captured,0kernel drops. Final private capture work/v90-paced60-echo-final.pcap audited:73491upstream and73489downstream G.711 packets on each corresponding PBX leg, exact ordered payload matches, no sequence gaps. Downstream timestamp increments normal; upstream only the previously observed initial120sample increment. CT output intervals17.177..22.711ms; PBX-to-ATA12.022..26.945ms. This excludes missing or altered packets across the captured PBX paths, not ATA playout or analog impairments. Probe19164 terminal; no live call remains from this test.

Added a native-modem GitHub CI job building C and running11 standalone regressions using synthetic/committed pre-data fixtures: V.8 CJ/menu, V.90 CP/DIL/S/startup/training transmitter/PCM/E-recovery/PPP echo, and framed audio diagnostics. All11 passed locally with the workflow build flags. Existing JS CI was green before this change; the new remote job still needs execution. Private hardware captures and SpanDSP-reference test are outside this job. Runtime was not changed in this turn.


## Retraining audio inspection and detector test allocation fix (2026-09-09)

Previous turn made progress by closing the paced test, auditing its final RTP capture, and adding native CI. Both native-modem and JS jobs passed on11a20e9. During deeper inspection found v90-s-detect.py had an obsolete ctypes State definition missing the C detector's normalized arrays/block energy. This underallocated state and invalidated its prior pass as reliable evidence. Replaced the duplicated layout with calloc/free wrappers compiled against v90training.h; corrected test passes. Other reviewed detector ramp/startup tests already use C-defined structures. This is a test-harness memory error, not evidence of an equivalent native runtime allocation error.

Downloaded private RX12191 for offline analysis. Corrected detector independently finds S/Sbar at raw1355.062375/1355.194875s in the first abandoned retrain, agreeing approximately with runtime events after its startup-clock offset. RMS is near9PCM during approximately1351.25..1355s, followed by the short S/Sbar and renewed silence before Tone A. Second abandoned attempt raw1359..1364 has no detector events; Ja-like high energy ends around1361.5s before a return to retraining tones. Thus the first failure is not simply a missed S under the current detector. Need compare transmitted training and caller reaction; original impairment and fallback remain unresolved. No runtime change or redial this turn.


## Training transmit comparison and idle deployment pass (2026-09-09)

Previous turn progressed via corrected test allocation and RX analysis. Both CI jobs on999db6c passed. Private TX12191 is now saved locally alongside RX. Comparing actual transmitted PCM to the initial successful training: all33440samples (4.18s) match exactly at raw starts324.494625,464.253125,838.990625 and1350.870750s; the second abandoned retrain at1361.359375s matches the initial2544samples. Independent DIL auditor finds full17334sample cycles in initial and three successful retrains. Failed retrain DIL starts1355.069250s and matches11397samples before the caller-triggered restart interrupts it. This supports correct emitted training content, not analog delivery or timing conformance.

Verified CT idle (no lm/pppd, service active), backed up src/config.js andsrc/index.js to CT/tmp/sipfax-before-engine-selection.tgz, deployed those two files plus src/modem-command.js from the tested branch. Node syntax check and service restart passed. Existing private capture command remains configured, so hardware verification exercises preservation of the explicit command override; it does not independently exercise default-command selection. Runtime still60ms/48k/1500ms/fixedwatchdog.

Fresh attempt fcf1b640-320c-4744-9bdb-da3ac5481567 connected19:19:05.749UTC, Windows0xF50000/native12337/pppd12339. First public probe HTTP200/559byte expectedhash in2.001s atWindows6.259s, allmodemerrors0. Dialhelper34780 terminal and leavescallconnected. Started100periodicpublicprobes with20s gaps/threefailurestop, handle41659live, work/v90-probes-0xF50000.json. Capture49834live,2400s bound, FreePBX/tmp/v90-engine-deployment.pcap; capture begins AFTER startup, so do not claim complete-call packet evidence. Preserve both handles and the existing call. Goalactive; stability stillunproven.


## CPs local prototype while deployed call continues (2026-09-09)

Previous turn progressed by training comparison and actual idle deployment. Revalidated41659; latest12publicrequests allcomplete throughWindows269.528s, CRC18/alignment0. Existing0xF50000/native12337/pppd12339 remainsactive; preserve probe41659 and capture49834 (capture has not been stopped). Runtime unchanged.

Read ITU-T V.90 9.6.1.2.4–6 and8.6.1–5 from official PDF. Added LOCAL UNCOMMITTED CPs prototype in vendor/linmodem/v90phase4.c/h plus untracked tools/tests/v90-cps.py. Do not deploy yet. receive_cp helper retains ordinary handling; CPs requires CPs-prime (E cannot substitute), completes Ed, sends Ucode0 silence in newstage7, accepts clear CP into aligned384/24 Rt stage8, resumes MP with noTRN, then ordinary CP-prime/Ed/B1/data. Test uses header-defined C state, bothG711laws and6framephases, DTEclamp, repeatedCPs, noEsubstitution, tone lengths/polarity and resumeddata. Newtest, existing E-recovery/startup tests and nativebuild pass locally. Test not yet added toCI; prototype notpublished.

Remaining review issue BEFORE deployment: post-Rt code currently calls v90_pcm_renegotiate, resetting scrambler/differential/filter state. Standard explicitly resets beforeTRN2d andB1d but does not explicitly specify a reset beforeMP followingRt in the silence procedure. Need resolve correct continuity, including prefetched encoder lookahead frames generated beyondEd; simply preserving encoder may also emit queuedzero-fill beforeMP. Currentsynthetic state-machine test does not establish this wire-level continuity. Do not call CPs complete or claim it explains recorded hardware failures (noCPs seen in those logs). Goalactive.


## CPs continuity implementation and another hardware watchdog recovery (2026-09-09)

Previous turn progressed via localCPsprototype. Resolved prototype's unconditionalpost-Rtreset by preservingcoding/filtermemory acrossuncodedSilence/Rt; this follows the absence of a reset requirement there, while8.6.5 explicitlyresetsbeforeTRN2d. Added v90_pcm_discard_lookahead at sixsampleboundaries: savedinputscrambler/differentialstate rewinds onlyunsentprefetchedframes; transmitted q/t/filtermemory remains. CPs Ed boundary discardslookahead/rewinds generatedcount, pause/Rt leaveencoderunchanged, resumedMP startswithpreservedstate. Initialtraining/B1 remainexplicitresetpaths. This interpretation stillneedshardwareinteroperabilityevidence; noCPswasobserved in the recentfailures.

Expandedtests: CPs acknowledgement/Eguard, exactU0silence, aligned384/24Rt, unchangedencoderthroughpause/Rt, resumedMP andDTErelease afterB1 across192law/framephase/Sr/lookaheadcombinations allpass. PCMtest checks discardedlookaheadregenerates identicaloutput withsameinput across32combinations and retains transmittedfilterstate. ExistingE-recovery,startup,framednativeaudioandnativebuild pass. AddedCPstesttoCI. Newnativechanges NOTDEPLOYED; currentlivecallremainspriornative.

Liveprobe41659 remainsactive: request14timedout atWindows361.540s CRC643/alignment6; request15recovered6.068s at389.150s, subsequent16/17passed at410.180/431.170s. Native log explicitlyinitiated unansweredPPPechoretrain3 atstartup394.020s, receivedCPprime andreportedPPPechorepliesresumed. This is another real watchdog recovery on a distinctcall, not the newCPscode. Earliercallerretrain1startup189.414875 andEtimeoutretrain2startup253.305s alsocompleted. Same0xF50000/native12337/pppd12339 remainsactive; preserve41659 andcapture49834. Private snapshot work/v90-engine-deployment-latest-log.txt. Goalactive.


## CPs audio integration regression passes (2026-09-09)

Previous turn made progress implementingcodingcontinuity andpublishingb029b9f. BothCIjobs onthatcommitpass. Addedtools/tests/v90-cps-wave.py: independentGPA recurrence,differentialfourpointmapping,rootraisedcosinepulseand1920Hzcarrier synthesizecallerCPs/CPs-prime/SCR/CP/CP-prime. Inputpassesv90_phase4_next andactualmatchedfilter/CRCparser, notdirectreceive_cp calls. Validsequence reachesU0silence,408samplesRt/Rbar,anddata; CRC-corruptedsequence neverstartsCPshandshake. Allfourfractionalsamplephases pass. Fixtureexpandedconstellationallowsvaliddatamapping; this is syntheticprotocolcoverage, nothardwareCPsacceptance. AddedtoCI. Nativecodeunchangedfromb029b9f andstillNOTDEPLOYED.

Revalidatedprobe41659 andcapture49834LIVE. Current0xF50000 reachesWindows641.122s with27successfulperiodicrequests/oneearliertimeout(index14) outof28completed; lastCRC871/alignment7. Samecallcontinuesafterwatchdogrecovery. No redial,restart,deployment,overlappingprobeornewcapture. Preserveexistinghandles; extendedtestnotfinished.


## CPs watchdog/deadline integration checked (2026-09-09)

Previous turn progressed via actualreceiver CPsaudio regression. BothCIjobs on256ccb3 pass. Extendedstartup tests to prove PPPhealth-watchdog cannotinterruptCPssilence/Rt and that a caller neverclearingCPs reaches theexisting5s+2RTD E deadline, followedbyexact70msmute/retrain andnoDTEconsumption. Checkedbothlaws; fixtureexplicitlysetsphase4lawtomatchstartup. Fullstartuptestpasses. No productioncodechange orruntimechange thisturn.

Revalidated41659live:36completedperiodicrequests throughWindows810.606s,35complete/oneearliertimeout; latestCRC875/alignment7. Keep0xF50000/native12337/pppd12339 andcapture49834, no restart. ReadonlyATAconfigconfirmsversion20260908-v90-faxmode1,g711ulaw/alaw,faxMode1; noexplicitjitter/VAD/echo settingwasreturned bytargetedsearch. This doesnotestablishdevice defaults. PendingCPsnative changes remainundeployed untilidle. Goalactive.


## CPs native candidate staged and checked inside CT105 (2026-09-09)

Previous turn progressed viaCPsdeadline tests. BothCIjobs on2da26ec pass. Archivedtrackedvendor/linmodem,tools/tests,test/fixtures andlauncher fromHEAD2da26eca1763d929c4db4d9e69cc511fc19d61b2 intoCT/tmp/sipfax-cps-2da26ec. Builtisolatedcandidate withnice10 andstandardCFLAGS; no /opt/sipfax source/binary orservicechange. Targetnativeframing,192CPsstatecases,32PCMcombinations andE-recovery testsPASS. CandidatebinarySHA256030036b41ce42ae782a3c1ffc4ee3bfbc5bdaf2196bc2b34348813759dba0458. This stagedbuildisreadyforidledeployment, notyetinstalled.

Attemptedtargetaudiointegrationtest couldnotstart: systemPythonlacksNumPy. No packagesinstalledduringactivecall. SameaudiointegrationtestpassedlocallyandinCI; do notclaimtargetaudiointegrationpassed. LogsCT/tmp/sipfax-cps-2da26ec-{build,wave,states}.log. Allstaginghandles41391/58582/42931 terminal. UpdateddraftPR29descriptiontoincludeCPscoverage,pendinghardwareverification,deployedJSengineselection,twowatchdogrecoveriesand24minfallbackfailure; PRremainsdraft.

Probe41659live throughindex42/Windows957.947s (~15m58s),42successful/oneearliertimeout. LatestCRC880/alignment7. Same0xF50000/native12337/pppd12339, capture49834stillactive; preserve. Runtimecontinuespriornative,60ms/48k/1500ms. Goalactive.


## Recovered call passes previous 24-minute disconnect window (2026-09-09)

Previous turn progressed viaisolatedCTcandidatebuild. Thisturn revalidatedprobe41659/capture49834 and waitedon41659 withoutrestarting. Currentcall0xF50000/native12337/pppd12339 has70completedperiodicrequests, 69HTTP200/559byte responses individuallyhashverified, andoneearliertimeout(index14). LatestWindowsduration1528598ms, CRC915/alignment7. It passedtheprior24minterminationwindow onsameconnection afterwatchdogrecovery; teststillrunning toward100requests, notacleanreliabilitypass. ATAreportedloss51atWindows1295.963s whiletrafficcontinued; inspectfinalpcapbeforeattributingwireloss. Keep41659 and49834; nocall/native/service/ATAchanges. CandidateCT/tmp/sipfax-cps-2da26ec remainsstagednotdeployed. Goalactive.


## 36-minute run complete; CPs build deployed and hardware data verified (2026-09-09)

Previous turn progressed by passingpriorfailurewindow. Keptsame0xF50000/native12337/pppd12339 through100periodicrequests. Probe41659 nowterminal:99HTTP200/559byte expectedhash responses individuallyverified,oneRequestCanceled(index14),finalWindows2163.771s (~36m04s),CRC963/alignment8. No additionaltimeouts afterearlierwatchdogrecovery. Explicitlydisconnectedattemptfcf1b640 aftertestcompletion(API200); confirmedno lm/pppd. This is a recoveredextendedrun, notzero-failureacceptance.

StoppedexactFreePBXtcpdumppid11505; capture49834terminal437054packets/0kerneldrops. Privatework/v90-engine-deployment-final.pcap and-audit.txt:108858upstreampackets matchedexactlybetweenPBXlegs;108860downstreampackets likewise. Nosequence/timestampanomalies. Capturestartedafterstartup, so onlycapturedintervalclaimed. CTout17.634..22.521ms/PBX-to-ATA12.668..26.862ms. ATAreportedloss96/jitter22atWindows1931.057s despitecontinuedtraffic; notexplainedbycapturedmissingpackets.

Backedupentire/opt/sipfax/vendor/linmodem toCT/tmp/sipfax-before-cps-native.tgz. Installedstaged2da26ec v90pcm.c/h,v90phase4.c/h andvalidatednativebinary; installedSHA256030036b41ce42ae782a3c1ffc4ee3bfbc5bdaf2196bc2b34348813759dba0458. Service restartedactive. Runtime60ms/48k/1500ms/privatecapturecommandunchanged. NewCPsnativeisNOWDEPLOYED.

Startedcapture16738beforefreshdial,FreePBX/tmp/v90-cps-live.pcap,2400sbound; remainsLIVEandcontainsbothattempts. Firstattempt3c2e7526-7bfe-4de9-b444-0dc1b1d165c7 failed678: Ja/S/DIL/CPt received,callerrenegretrainatstartup14.923625 thenV34fallback. NoCPsseen; failuremustremaininacceptanceresults. Privatework/v90-cps-first-startup-log.txt; helper67239terminal. Retryce96428f-f366-4c75-b24f-2150be272a64 connected19:58:54.333UTC,Windows0xF70000/native12605/pppd12606. FirstpublicHTTP200/559byte expectedhash in1.257s,allmodemerrors0. Helper14121terminalleavesconnected. Thisverifiesordinaryhardwaredataonnewbuild, notCPsinteroperabilityorabsenceofregression.

Started100periodicprobeson0xF70000,handle35217LIVE,work/v90-probes-0xF70000.json. Preserve35217/capture16738andexistingcall; nooverlappingprobe/redial. Goalactive; intermittentstartup,underlyingimpairment,hardwareCPs/fallback/higherupstreamremainincomplete.


## Failed startup replay excludes changed Phase4 waveform (2026-09-09)

Previous turn progressed throughcompleted36minsoak,newnative deploymentandretryhardwaredata. Revalidated35217live. DownloadedprivateRX/TX12601forfailedfirstcall; copies40322/46879terminal. ExtractedpreCPs0add7d3sourceinto privatework/sipfax-pre-cps-source. Privatehelperwork/compare_v90_cps_startup.py compiledold/currentPhase4 librariesandfedidenticalRXraw16.876125s through20.163625s (26300samples), coveringRi/CPt/TRN2d/MP untilcallerretrain. BothdecodeCPt gain8175 at0.130125s,TRNstart0.1335s andMP1.6335s. ALL26300TXsamples identicaloldvsnewandidenticaltocapturedactualTX. This rulesouta changedPhase4transmitwaveform inthatinterval; it doesnotprove allregressionsabsent orresolveexistingstartupfailure. Fullprivatecapturesoutsidegit.

Live0xF70000/native12605/pppd12606 has8successfulperiodicrequests throughWindows169.373s, CRC9/alignment1. Preserveprobe35217 andcapture16738, no restart/runtimechange. Goalactive.


## Independent captured-MP validation on failed startup (2026-09-09)

Previous turn progressedbyold/newwaveformequality. Revalidated35217live. Existingv90-phase4.py withfailedRX12601 and--long-training passesindependentTRN/MP/CRCdecoding. Moreimportantly, privatehelperwork/audit_v90_failed_startup_tx.py appliesindependentdemapping/descrambling directlytocapturedTXraw16.876125..20.163625s: actualRi/Rbar,1500msTRN,firsttworepeatedMPmessagesandMPCRC allPASS. This supplementsold/newequalitywithwirecontentvalidation, withoutclaiminganalogdeliveryortimingconformance. No productionchange.

Current0xF70000/native12605/pppd12606 has15successfulperiodicrequests throughWindows316.385s,CRC11/alignment1. Keep35217/capture16738; no restart. ReadonlylegacyV34inspectionconfirmsseparateV34_init/V34_process state API,notanexistingdrop-infallbackhook inV90_startup. Nounverifiedfallbackwiringadded. Goalactive.


## Larger upstream request passes on deployed CPs build (2026-09-09)

Previous turn progressedbycapturedMPvalidation. Revalidated35217live. Reviewedupstreamreceiver: stillhard-sliced4800bit/s withno trellisFEC/adaptivetiming; smallperiodicGETs alone donotvalidatefull-sizedupstreampackets. StartedtemporaryPPP-onlyfixturev90-upstream-check on10.64.0.1:8082 with300slifetime. Privatehelperwork/probe_large_v90_request.py verifiedexactworkerPID728062 commandline, waitedforcompletedperiodicrequest, SIGSTOPpedonlythatworker betweenAPIrequests, issuedoneGETwith4096-bytequery, andSIGCONTresumedworker in finally. Modem/PPP remainedconnected. This intentionaladditionaltestmustbeincludedinthetrafficprofile.

GETPASSEDHTTP200/15byte expectedSHA256179937346cbd120ccd471233f6718604bf7a22884538f0f12efb2b56d50b73a0 in16.667s atWindows502.983s. Windowsbytecounterdelta sent8696/received602,8framesSent/9framesReceived,0additionalCRCerrors. NativelogincludesCRCvalidupstreamPPPframes1467,1503,1267,1303bytes. Thisvalidateslargerrequestdelivery, notoptimalupstreamthroughput; extra sentbytes/delaymayinvolveretransmissions butnotverifiedfromTCPmetadata. Privatework/v90-large-upstream-0xF70000.json and-native-log.txt. Helper71494terminal; temporaryfixtureexplicitlySTOPPED.

Confirmedperiodicworkerresumed: request23PASSED3.729s atWindows509.853s, CRC19/alignment3. Total24periodicrequestsallcomplete. Same0xF70000/native12605/pppd12606. Preserve35217/capture16738. No modemconfigurationorcodechange. Goalactive.


## TCP retransmissions isolated; smaller-segment comparison improves upload (2026-09-09)

Previous turn progressedthroughlargerupstreamdelivery. Repeatedsame4096bytequerywithPPP-headercapture: MSS1460run16.671s,8696sentbytecounterdelta. PrivateCT/tmp/v90-upstream-tcp.pcap(28packets/0drops),localwork/v90-upstream-tcp-metadata.txt showoriginal1460byteTCPsegments acknowledgedimmediatelybyserverthenretransmitted;7receiveddatasegmentscontained3uniquebyteranges/9620payloadbytes includingpost-API-completionretries. This verifiesduplicateTCPdata, notthecauseofclienttimer/ACKhandling.

Controlledendpoint-onlycomparison: temporaryPythonHTTPlistener TCP_MAXSEG536 advertised536inSYNACK (captured), withoutchangingPPP/modem/globalnetworkconfiguration. TwoidenticalqueriesPASS15bytehash in10.175s and10.258s,4680sentbytes each. PrivateCT/tmp/v90-upstream-small-mss.pcap (48packets/0drops),localwork/v90-upstream-small-mss-metadata.txt show8segments/8uniqueranges/4180payloadbytes perrequest, noduplicateddataranges. Approximately39%shorterrequesttime thanboth1460baselineattempts16.667/16.671s. Supportscontrolledsmallsegmenttuningforcurrent4800upstream; higherupstreamimplementationstillrequired, andfullinternet/downstreamtradeoffnotyettested.

Eachlargeprobeusedexistingprivateworkerpause/finally-resumehelperonlybetweenperiodicrequests; samecallthroughout. Bothtemporaryfixturesv90-upstream-check andv90-upstream-small-mss explicitlySTOPPED. ExactCTtcpdumppids12652/12665stopped; handles10372/68007terminal, alllargeprobehelpers53395/78924/52935terminal. NoTCPMSSclamporPPPsettingsleftchanged. Savedfirst/baseline/MSS536JSONseparately, currentwork/v90-large-upstream-0xF70000.json holdssecond536result. Privatehelperwork/v90-small-mss-server.py.

Periodic35217live through42requests/Windows899.753s, allcomplete,CRC36/alignment3; request40took7.724s, next0.956s. Preserve35217/capture16738 and0xF70000/native12605/pppd12606. Goalactive.


## Optional upstream MSS limit deployed; CPs soak fails during recovery (2026-09-09)

Previous turn progressedviaTCP-sizecomparison. Implemented38fb104 optionalSIPFAX_PPP_UPSTREAM_TCP_MSS inEgressPolicy/index: unsetdisabled,integer256..1460validated. nftpostrouting/mangle andiptablesPOSTROUTINGtargetonlyIPv4destinationPPPpool SYN(withRSTclear), loweronlyMSSabovelimit. Oppositedirectionunchanged. Perleasecleanupremovesthenfttable/iptablesrule. Diagnostics/descriptorexposevalue. README/envexampledocumentrequiredegresshelperandtradeoff. All52Node testspass, CTnft-cgeneratedrulesPASS, iptables-translatePASS; bothGitHubCIjobson38fb104pass.

Duringimplementation, CPsbuildsoak35217terminatedafter3consecutivefailures. 47periodicrequests:44complete,3timeouts(index44..46), final1108.864sWindows,received58048frozen,CRC1970/alignment57. Nativecallerrenegretrain1startup883.764875s completed; callerretrain2startup941.214875s completedat46.667kCP. Watchdoginitiatedretrain3startup1054.660s,butfailedtrainingandcallerenteredV34fallbackfromretrain4startup1069.149875s onward. NoCPssilenceeventininspectedfailurecontext. Explicitlydisconnectedce96428f afterobservingfallbackloop,verifiednolmorpppd. Privatefailurecontextswork/v90-cps-{late-failure-log,failure-context}.txt. StoppedFreePBXpid15577;16738terminal253111packets/0drops. Savedwork/v90-cps-live-final.pcap/-audit.txt:bothfirstfailedstartupandsecondcallpairsmatchorderedpayloadsexactly (3039/59991downstream,3041/59993upstream percorrespondingPBXleg). Thisremainsfailedreliabilityacceptance, independentofMSSchange notyetdeployedattimeoffailure.

BackedupCTsrc/ppp.js/index.jsto/tmp/sipfax-before-mss.tgz, deployednewfiles,configured/etc/systemd/system/sipfax.service.d/v90-mss.conf with536,verifiedidleandsyntax/restartedactive. NativebinaryremainsCPs2da26ec. Newattempt6ce9cb64-d017-42a1-9e34-d1544f1501e9 connected20:21:02.473UTC,Windows0xF80000/native12771/pppd12772. PublicHTTP200/559bytehashPASS1.405s/0errors. Dial58401terminal. Actualnftupstream_msschainverifiedinstalledbyPPPhook; PPPMTUstill1500.

Deployed-ruleverificationwithordinaryunmodifiedHTTPfixture:4KBuploadPASS10.165s,32KiBdownloadPASS12.507s,bothexpectedhashes. CTppp0captureconfirmsXP SYN MSS1460/serverSYNACKrewrittento536,upstreampayloadmax536/downstreammax1460. Thusasymmetryverified; single12.507sdownloadisnotproofunchangedthroughput. Privatework/v90-mss-deployed-{up,down}.json/-metadata.txt. Testhelper29426terminal,fixturev90-upstream-checkSTOPPED,CTcapturepid12805stopped62412terminal85packets/0drops.

Started100periodicrequestsonsame0xF80000,handle45640LIVE,work/v90-probes-0xF80000.json. FreePBXcapture17887LIVE/tmp/v90-mss-live.pcap,2400sboundstartedbeforecall. Preservebothandexistingcall. RuntimeCPs/48k/1500ms/60ms/MSS536. Goalactive:underlyingimpairment,intermittentstartup,hardwareCPs/fallback/higherupstreamstillnotcomplete.


## Retrain immediately after unusable Phase 4 parameters (2026-09-09)

The previous failed recovery on native PID 12605 rejected a CRC-valid CPt at Phase 4 +0.556250s. An independent decode of the training-only RX interval (raw seconds 1071–1075) recovered ten identical valid frames: drn 9, Sr 1, lookahead 1, gain 8192, six transmit masks of sizes 3/3/3/5/3/3. Their product is 1215, below the 4096 combinations required for K=12. Native rejection is correct; removing the capacity guard would produce invalid mapping. The cause of the caller's unusable constellation remains unknown. Private crop and decoded bits remain outside Git.

The startup controller now initiates a full retrain immediately when Phase 4 rejects parameters, instead of remaining in failed-stage silence until the caller restarts or falls back. V.90 section 9.4.1 permits retrain at any point in Phase 4; the existing retrain path implements section 9.5.1.1 silence and Tone B. A regression uses the observed six transmit masks and verifies one restart, 70 ms mute, DTE clamp and law preservation. The complete startup test passes, including existing echo-watchdog and CPs deadline checks; the native build passes. This change is not yet deployed or hardware-validated.

The existing MSS-enabled call 0xF80000 remains untouched: 25/25 periodic HTTP probes complete through Windows duration 649.865 s. Preserve live probe handle 45640 and capture handle 17887. The active native binary remains the prior CPs build. Goal remains active; recovery reliability and higher upstream rates are unfinished.


## Exact rejected CPt regression and target build (2026-09-09)

The startup regression now parses the exact 1788-bit training-only CPt from PID 12605, committed as `test/fixtures/v90-cpt-unusable-12605.bits` with provenance documentation. It verifies CRC acceptance and the 1215-entry capacity before exercising rejection/retrain. The complete startup suite passes. Native commit 2b8f411 builds successfully in CT105 under `/tmp/sipfax-rejected-cpt-2b8f411`; the active installation is unchanged. GitHub run 34401633545 passes both JavaScript and native jobs.

Probe handle 45640 is confirmed live; 31/31 requests complete through Windows duration 777.258 s, CRC 34/alignment 0. Native PID 12771 and pppd 12772 are confirmed active. Native logs reveal full caller retrains at startup seconds 215.513125 and 275.673125, plus two later successful rate renegotiations (Phase 4 seconds 140.444750 and 296.586000). Therefore passing application probes must not be described as an uninterrupted channel. Preserve the current call and capture; the rejected-parameters recovery change still awaits deployment/hardware validation.


## Failed renegotiation wire exchange independently decoded (2026-09-09)

On the still-live MSS call PID 12771, an independent Python decoder recovered 18 CRC-valid CP frames from RX raw seconds 218–222, all requesting 48k with acknowledgement clear. A successful renegotiation from raw seconds 433–437 yields six CRC-valid frames including CP-prime. Thus the first failure is not explained by the native receiver merely missing a caller acknowledgement. The selected masks differ after the intervening full retrains; the cause of caller non-acceptance remains unknown.

The corresponding captured TX has its TRN start at crop sample 1852. Independent inverse mapping/descrambling verifies 255 ms of training ones, followed by 503 complete MP frames with valid CRCs: 306 unacknowledged MP and 197 MP-prime. This establishes that the native server emitted valid MP acknowledgements; it does not establish their delivery through the ATA analogue path or successful decoding by the caller. Private helpers/results: `work/audit_v90_mss_reneg_tx.py`, `work/v90-mss-first-reneg-tx-audit.txt`, and `work/v90-mss-{first,success}-reneg-cp.txt`. All captured audio remains outside Git.

Authoritative handle 45640 remains live. Probe 34 timed out with CRC 846/alignment 13, then probes 35–39 completed (probe 35 took 13.021 s). This is 39/40 completed through Windows duration 1008.350 s, CRC 1101/alignment 21. Native logs show caller-initiated retrain 3 at startup 928.313125 s restored the data path. Retain the call until the configured three-failure stop or completion. Recovery candidate remains staged but undeployed. Goal active.


## Recovery change deployed; new hardware call connected (2026-09-09)

Previous turn made progress through independent wire analysis. The MSS call 0xF80000 subsequently ended: probe handle 45640 is terminal, 41/45 requests completed (one 30 s cancellation, one client-side timeout, two 404 responses after disconnection). Last completed probe duration 1068.747 s; native lifetime approximately 1147 s. Caller retrains 4/5 at startup 1059.313125/1073.703125 s were followed by E-timeout retrain 6 at 1117.441250 s and V.34 fallback INFO1a (downstream 4/UINFO 69). No unusable constellation rejection appears in this final failure. Private final log `work/v90-mss-ended-log.txt`. FreePBX tcpdump PID 18269 explicitly stopped; handle 17887 terminal, 230284 packets captured, zero kernel drops.

The exact rejected-CPt regression also passes on CT105 through a standalone C harness (both laws, one retrain, 70 ms mute and DTE clamp). Private target result `work/v90-rejected-cpt-target-test.log`. This avoids claiming NumPy tests ran on CT105. The draft PR description was refreshed with current deployed features and limitations.

After verifying no native modem or pppd process remained, backed up the active binary/source to `/tmp/sipfax-before-rejected-cpt.tgz`, installed staged 2b8f411 binary and v90startup.c, and restarted sipfax successfully. Active SHA256 is e453d5c6b55a1631587f69f2ff39777f46a990fdb59280d7063790afe74ab62e. Existing 48k/1500ms/60ms/MSS536 settings persist.

New attempt 9fa71c19-7c3d-47ae-a42e-5c6fcd889cda connected at 20:41:57.357 UTC, Windows 0xF90000, native PID 13038, pppd 13039, 48k and initially zero errors. Dial handle 69303 terminal; new FreePBX capture handle 5144 LIVE (`/tmp/v90-rejected-cpt-live.pcap`, 2400 s bound). Preserve the call. Public probe is currently unavailable: repeated API HTTP 400 says another network probe is running, apparently a stale probe from the previous disconnected call. This is not proof of internet connectivity or modem failure. No new periodic probe worker started. Goal remains active.


## DialUpLab probe bind loop isolated (2026-09-09)

The deployed recovery call 0xF90000 remained connected at the beginning of this turn (Windows duration 122.987 s, CRC 1/alignment 0). Repeated HTTP probes returned 400 “Another network probe is running.” A bounded CT ppp0 capture `/tmp/v90-rejected-cpt-ppp.pcap` finished with zero IP packets; direct ping from the preceding turn had no replies. These observations do not prove Internet access.

DialUpLab source inspection found an unbounded BindIPEndPointDelegate retry returning the same unavailable PPP address. Microsoft documents up to Int32.MaxValue retries unless the delegate throws. Added an abort on a repeated bind or expired deadline, preserving strict source binding rather than falling back to LAN. Regression probes an unassigned source, requires completion within five seconds, verifies no fallback connection and then verifies a successful subsequent probe. Candidate commit 008642d is on DialUpLab branch `codex/xp-probe-bind-recovery`, draft PR https://github.com/nicolasumaras/DialUpLab/pull/1. The PR also includes previously unmerged XP diagnostics and has a title/body reflecting that scope.

Windows CI run 34403002758 has passed its normal tests and XP diagnostics step; artifact packaging is still running. Watch handle 74976 LIVE, private log `work/dialuplab-bind-ci-watch.log`. This is not yet deployed on XP. A concise asynchronous request to close/reopen DialUpLab was sent to the user because the API has no restart or cancel-probe endpoint. Preserve the current call and FreePBX capture handle 5144; do not interpret the stuck API as proof of modem failure. The V.90 goal remains active and independent native work remains possible.


## New build carries TCP over PPP; XP update packaged (2026-09-09)

DialUpLab Windows CI run 34403002758 completed successfully, including missing-source recovery and XP packaging. Watch handle 74976 terminal. Downloaded the portable artifact and prepared `outputs/DialUpLab-XP-probe-fix.zip` containing only the replacement executable, license and update instructions, preserving existing settings. Executable SHA256 c8bdc440238b8b92bd4ef97bc01f117f304ff7dcbbd5413945e7ecedee36c922. Physical XP deployment remains pending; the asynchronous reopen request has not been answered.

A direct authenticated HTTP health request from CT105 to `10.64.0.2:4782` succeeded. Bounded ppp0 capture `/tmp/v90-api-over-ppp.pcap` contains nine packets with zero drops: full TCP handshake, 158-byte request and 290-byte response followed by clean close. This verifies bidirectional IP/TCP through the newly deployed modem, independently of the stuck notebook-originated probe. It does not verify public internet egress. The earlier unanswered ping is therefore not evidence of a completely failed data channel.

Started a bounded 30-request direct PPP health test, handle 90121, private helper `work/v90_health_over_ppp.py` and results `work/v90-health-over-ppp.json`, 20-second gaps and stop after three failures. This tests server-originated TCP over PPP only. Existing call 0xF90000/native13038/pppd13039 and FreePBX capture5144 remain in place; a native E-timeout retrain3 was observed at startup462.414125s before the successful direct health request. Goal active.


## Prepare bounded renegotiation-training experiment (2026-09-09)

The direct PPP health worker 90121 remains live, with ten successful requests so far. The active deployed modem remains the rejected-parameters recovery build; no restart or configuration change was made during this turn. Notebook-originated public probes remain unavailable pending the previously requested DialUpLab restart/update.

Added optional `SIPFAX_V90_RENEG_TRN2D_MS`, default 255 ms, accepting 0–2000 ms rounded down to six-sample frames. V.90 9.6.1.2.2 explicitly permits optional TRN2d up to 2000 ms. This prepares a controlled comparison of longer training after independently validating emitted MP/MP-prime during failures; improvement is a hypothesis, not an observed result. Initial training retains its separate 255–2000 ms bounds and existing deployment value. CPs silence/Rt still resumes MP without new training.

New CI regression drives actual S/Sbar through the detector, checks one aligned 384+24-sample Rd response, DTE clamping and independently inverse-maps/descrambles training and MP CRC at default, zero, short, 1500, 2000 and invalid settings. It passes. Existing startup/deadline and CPs structural/audio suites pass; native build passes. Candidate is not deployed. Goal active.


## Renegotiation candidate built on target; PPP soak remains live (2026-09-09)

Candidate 5b45059 builds successfully in CT105 under `/tmp/sipfax-reneg-training-5b45059`. GitHub run 34403916810 passes on that exact commit. The active installation and 255 ms renegotiation setting are unchanged. The direct PPP health worker 90121 is confirmed live through eighteen successful requests; preserve it and capture5144 until the bounded test ends. The update archive for the pending XP intervention is `/home/ncolasumaras/Documents/Codex/2026-09-07/we-x20/outputs/DialUpLab-XP-probe-fix.zip`. No new hardware or internet acceptance claim is made. Goal active.


## Baseline PPP test complete; 1500 ms renegotiation trial started (2026-09-09)

Previous turn made progress by staging the candidate. Worker 90121 completed all 30 direct PPP health requests successfully; results preserved as `work/v90-health-over-ppp-baseline.json`. Final Windows baseline observation: 1237.529 s connected, CRC110/alignment1, 48k, other reported error counters zero. This follows earlier retrains and is not uninterrupted channel acceptance. Explicitly disconnected attempt 9fa71c19 after worker termination. FreePBX tcpdump20572 stopped; handle5144 terminal,257185 captured/0 kernel drops.

Independently audited the previous MSS call's final capture (`work/v90-mss-final.pcap`):57353 upstream/57351 downstream packets match ordered payloads exactly across PBX legs, no sequence gaps; only the known initial upstream timestamp anomaly. CT departure intervals17.028..23.001ms, PBX-to-ATA15.238..24.730ms. A regression of 20-second block median arrival residuals against nominal20ms cadence gives +1.442ppm upstream and -0.954ppm toward ATA, a relative difference about2.4ppm. This amounts to a few milliseconds across the call and does not by itself establish a clock-slip cause. It cannot measure ATA internal playout. Private helper `work/v90_rtp_clock_audit.py` and result `work/v90-mss-clock-audit.json`; no pacing change made.

After verifying idle, backed up binary/v90phase4.c to `/tmp/sipfax-before-long-reneg.tgz`, deployed staged5b45059 binary/source, added `/etc/systemd/system/sipfax.service.d/v90-reneg-trn.conf` with1500ms, daemon-reloaded/restarted successfully. Active binary SHA256 df6b4d0a74a7381a032bb045962a0d5376fa8ad08839bdde80800d02055bf6e1. All other experiment settings persist.

New attempt b6f36f9e-49d0-4163-a662-6400f2ba3ea8 connected21:03:58.938UTC, Windows0xFA0000/native13317/pppd13365,48k/zero initial errors. Dial66010 terminal. Public API probe still returns400 from the previously established probe lock; no claim of public internet validation. New FreePBX capture94833 LIVE (`/tmp/v90-long-reneg-live.pcap`,1800sbound). Direct PPP health worker95490 LIVE,30requests/20sgaps/3failstop, `work/v90-health-over-ppp.json`. Preserve live handles. The baseline direct test began later in its call, so comparisons must account for differing call ages and do not prove a causal improvement. Goal active.


## Longer training exercised; one-packet loss tolerated (2026-09-09)

The 1500 ms trial's undisturbed direct PPP batch completed30/30; worker95490 terminal and results copied to `work/v90-health-over-ppp-long-reneg-baseline.json`. Full native log reveals two spontaneous rate renegotiations (Phase4 S at627.694 and688.229s), both completed. The second has TRN start688.2975, MP689.7975 (exactly1500ms), Ed complete690.051 and upstreamE690.11825. No full retrain appears in the inspected current-call log. This exercises the setting on hardware, not only in CI; it does not establish improvement over a matched randomized baseline.

Prepared and executed the scoped one-packet helper `work/v90-onepacket-long-reneg.sh` after confirming the call had returned to data and the request worker ended. It targets only PBX192.168.1.29:17162→ATA192.168.1.235:5010, limits DROP to one/day burst1, and removes its exact rule through a cleanup trap. Counter verified1packet/200IPbytes; installed1788988550.936889 and removed1788988551.944798, no experiment rule remains. Private record `work/v90-onepacket-13317.txt`.

Copied running capture prefix `work/v90-long-reneg-fault-prefix.pcap`; ATA-facing RTP alone shows one sequence step2 at following-packet epoch1788988550.976583. Other paths have no sequence gaps. Prefix endpoints differ by one in-flight packet, so the basic full-list comparer reports end mismatches; do not mislabel them extra packet loss. Independently matched a complete decoded RTP packet to native RX raw730.0s, deriving epoch origin1788987801.676626. Independent S detector locates later S at raw765.799875/epoch1788988567.476501, about16.5s after omission. Thus the later exchange must not be claimed as definitely triggered by the fault. Private timing helper/result `work/v90_fault_timing.py`, `work/v90-long-reneg-fault-timing.txt`.

Later native exchange S748.2635,TRN748.332,MP749.832,Ed750.0855,E750.15325 returns to48k data. New worker15773 LIVE, six direct PPP health requests passed so far; same `work/v90-health-over-ppp.json`,30requests/20sgaps/3failstop. Preserve call0xFA0000/native13317/pppd13365 and capture94833. The stream tolerated the injected omission and continued passing IP traffic, but long-term reliability/public egress/higher upstream remain unproven. Draft PR29 deployment text refreshed. Goal active.
