# V.42 error-control integration assessment

The native V.90 link now has a hardware-validated V.42/LAPM path. It negotiates error correction with the XP notebook's physical modem, carries PPP DTE octets through LAPM, and has completed authenticated internet probes. The earlier asynchronous PPP path remains available when `SIPFAX_V90_V42` is unset.

The upstream [SpanDSP V.42 source](https://github.com/freeswitch/spandsp/blob/8f1e1646bdec99eac5fd2cd92c35563f736b9b89/src/v42.c) is explicitly labelled unfinished. It provides detection, HDLC and LAPM state handling under the source file's LGPL-2.1 terms. It is a reference candidate, not a drop-in dependency qualified for this server.

Concrete integration checks:

- Its timers count calls to `v42_tx_bit`, and `v42_init` hardcodes 28800 bit/s. The digital V.90 transmitter runs at the negotiated downstream rate, often 148000/3 bit/s. Timers must follow real elapsed sample time or the actual transmit rate, including pauses and retraining.
- The default configuration advertises compression (`comp = 1`) with a TODO about V.42bis startup. Disable unsupported compression or implement and independently qualify the negotiated compressor; never advertise a capability solely because a default enables it.
- The [upstream test](https://github.com/freeswitch/spandsp/blob/8f1e1646bdec99eac5fd2cd92c35563f736b9b89/tests/v42_tests.c) declares error-injection periods as `bool` and assigns 10000/11000. Those become true, so the `-b` branch flips every bit rather than injecting sparse faults. It also ends with success without a final data-integrity assertion. Its exit status is not sufficient evidence of reliable error recovery.
- First build an independent two-peer harness with asymmetric clocks, verified bidirectional payloads, deterministic corrupted/dropped frames, retransmission/duplicate checks, bounded memory and busy/backpressure tests. Then integrate synchronous bits before the existing UART parser and after the V.90 descrambler, with a corresponding downstream bit source, retaining asynchronous fallback and clean retrain behavior.

No V.42/LAPM implementation was enabled or deployed during this assessment. The existing optional E/NUL decline handshake is not LAPM.


## Isolated qualification result

The pinned reference was built from the seven required C modules without installing a system dependency. A two-peer lab found an unsolicited-final-bit defect: the RR/RNR response helper sends F=1 even when P=0. This contradicts V.42 8.4.2 and stalls the clean unequal-rate case. Correcting the response to echo the incoming P bit fixes that regression.

The corrected implementation passes 44/46 expanded cases covering exact bidirectional data, final acknowledgements, unequal clocks, propagation delay, backpressure, variable frame sizes and corrupted bits/bursts. Two fixed-frame cases with dense continuous periodic faults at long delay remain incomplete; both recover when the faults stop. See [the pinned lab, procedure and limitations](v42-lab/README.md). The runner returns failure for the incomplete corrected cases. None of this enables LAPM on the live service.


## Timer and detection corrections

A second defect keeps the idle timer active when sending I frames; V.42 8.4.1 requires the acknowledgement timer. A direct per-frame timer check reproduces the violation and verifies the correction. ASan/UBSan also found a negative signed shift in detection; bounding the register to its ten used bits fixes it.

With all three corrections, 45/46 primary cases pass and all 46 run without sanitizer findings. The remaining fixed-size dense-error case exceeds 120 simulated seconds but completes with exact data and acknowledgements at 164.961250 seconds in a separately labelled continuous-fault diagnostic. The original deadline failure remains reported. A trial restriction on sending during timer recovery produced no change in these cases and was not retained. Malformed-frame handling, severe-outage behavior and native/hardware integration remain to be qualified; the live V.90 service is unchanged.

## Receive envelopes and XID serialization

The lab now rejects incomplete frame headers and malformed XID group/TLV envelopes before protocol state can change. Fourteen explicit invalid inputs pass unchanged-state checks, including an empty frame that reproduces a heap-buffer-overflow in the previous reference. Independent wire fixtures detect and verify a missing pointer advance after HDLC options; both compression-disabled and compression-advertisement encodings now match the expected bytes. Corrected cases run without sanitizer findings, including the longer recovery diagnostic. Parameter-specific widths/values, broader malformed inputs, outage handling and hardware integration remain open. These changes remain isolated from production.

## Negotiated directions and lifetime

Independent asymmetric fixtures now verify peer-TX/local-RX mapping, selected response values, standard defaults for missing parameters, persistence through SABME and restoration of preferences on modem restart. Four new complete data-transfer cases with unequal frame sizes and windows pass, expanding primary transfer coverage to 49/50 within the deadline. The original dense-error deadline failure remains visible. Invalid parameter values, out-of-range responses, optional-function handling and outage behavior still need qualification before native integration.

## Native integration boundaries verified from current code

- `v90upstream.c:bit` exposes descrambled upstream bits immediately before the UART parser. It is called for multiple timing/pair candidates. A LAPM receiver must retain a selected candidate and receive explicit discontinuities; merging callbacks from all candidates would corrupt HDLC ordering. Existing `receive_frame` deduplication works on complete PPP frames and cannot substitute for bitstream selection.
- `v90.c:v90_serial_bit` consumes byte-valued DTE FIFO entries and adds 8N1 framing. A negotiated LAPM transmitter must replace this framing with HDLC bits. LAPM information callbacks carry DTE octets; their boundaries are unrelated to PPP packet boundaries.
- `v90phase4.c:data_bit` emits B1, clamps DTE during retraining and can optionally emit a decline handshake. LAPM support must coordinate detection here and never run alongside decline mode. The V.42 timer must follow usable negotiated downstream bits/sample time deliberately across these pauses.
- `v90upstream.c` resets candidate parser state on first mapping frames and erasures. `v90phase4.c` can reinitialize the complete upstream receiver during renegotiation. Protocol state and callback ownership must survive or explicitly reset at the correct layer.
- `v90startup.c` currently uses valid initial LCP as evidence to suppress startup rate fallback. LAPM establishment needs an explicit corresponding state; fabricating `lcp_seen` would hide missing PPP startup.
- Native receive FIFO backpressure must map to LAPM local-busy behavior without dropping acknowledged DTE octets. The existing complete-PPP-frame discard path is not adequate for reliable LAPM delivery.

These are code-inspected integration requirements, not implemented hooks or hardware proof.

## Native synchronous-bit boundary

The upstream receiver now has an optional callback for descrambled synchronous
bits. It identifies each QAM, soft, or hard timing candidate separately and
reports QAM erasures as candidate invalidations. Profile initialization clears
the callback so a call owner cannot accidentally retain stale callback state.

A replay fixture independently reconstructs the existing asynchronous PPP
frames from this callback. Across nine captured startup recordings it observes
102 native CRC-valid frames and matches all 102 exactly; eight recordings
contain frames. Two representative recordings, including one with repeated
candidate resets, also pass ASan/UBSan in a temporary CT105 build. Production is
unchanged.

Selecting the first candidate that recognizes the V.42 ODP sequence is rejected:
it reaches valid LCP in only three of the nine recordings. Some early candidates
reset before PPP begins. Native LAPM therefore needs per-candidate HDLC evidence
and buffered replay before committing to one ordered stream. The callback must
not merge candidates.

The pinned, corrected LGPL-2.1 V.42 subset is now vendored with its licence,
upstream source hashes and reproducible generation command. The complete native
modem links it, but no runtime path calls it yet.

An initial passive selector maintains separate detection and CRC-16 HDLC state
for all 40 receiver candidates. It accepts selection evidence only from a valid
general-format V.42 XID after a complete ODP sequence and trailing mark gap. It
buffers 8192 bits per detected candidate, replays the chosen stream in order,
ignores a valid XID without ODP, rejects a bad FCS, isolates other candidates,
reports invalidation, and fails closed on buffer overflow. A synthetic test
exercises each condition under ASan/UBSan. Hardware has not sent an XID to this
path yet, so selection is not an interoperability result.

## Opt-in runtime bridge

The native server now has an opt-in `SIPFAX_V90_V42=1` path that connects the
candidate selector to the corrected V.42 answerer. It replaces downstream 8N1
framing with LAPM HDLC bits, passes upstream selected bits to LAPM, preserves
the bit callback across the phase-4 receiver reset, and exposes LAPM connection
state to the PTY/PPP launcher. The default path remains the existing
asynchronous PPP implementation.

Received LAPM information is held in a 4096-byte queue before the native DTE
FIFO. The bridge asserts V.42 local-busy before that bounded queue can fill and
does not consume additional information while busy. A two-peer test holds the
answering DTE closed, resumes with partial writes, and transfers 16384 verified
bytes each way after ODP, CRC-valid XID candidate selection, and LAPM link
establishment. The complete bridge test passes ASan/UBSan on CT105 and the full
native modem links successfully. Hardware interoperability is still required
before retaining this option in the service configuration.

The first hardware trial detected a complete ODP on candidate 8 but received no
XID before repeated retraining. Inspection found that the pinned answerer
entered LAPM before emitting its tenth advertised detection pattern. The
generator now emits all ten patterns, and a direct wire-count assertion covers
the 360 detection bits. The corrected two-peer transfer matrix retains its
49/50 primary deadline result. A second hardware call still received no XID.
The runtime therefore follows the robustness guidance in V.42 Appendix III.1:
after ODP it repeats supported ADPs until the selector observes the
originator's CRC-valid XID, then changes the reference protocol state before
replaying the buffered frame. A third hardware call transmitted the repeated
ADP but still selected no XID before the notebook requested rate
renegotiation. An independent inverse of the negotiated 49.333 kbit/s PCM
mapping recovered all 1776 B1d mark bits followed by 387 consecutive,
bit-exact E/C ADPs from the captured server audio. This rules out the logical
ADP generator, downstream scrambler and PCM mapper for that interval. The next
hardware build records bounded candidate metadata (transitions, flags and
HDLC frame validity) to determine whether the notebook recognized ADP and the
upstream receiver lost the subsequent XID. It passed the two-peer bridge under
ASan/UBSan; the notebook was offline when that diagnostic call was attempted.

## Hardware interoperability result

The candidate metadata showed that the notebook recognized the repeated ADP
and changed to continuous HDLC flags. V.42 7.2.1.3 permits protocol
establishment to begin on continuous flags, so the selector now retains a
rolling bounded tail and selects an ODP-qualified candidate after ten adjacent
flags or a CRC-valid XID. The selected stream then decoded repeated CRC-valid
77-byte XID commands from the notebook.

Those XIDs exposed the final protocol defect. Their standard V.42 and V.42bis
groups were followed by the V.42 User Data subfield containing V.44 parameters.
Unlike the length-prefixed negotiation groups, the `0xff` User Data subfield
occupies the rest of the XID information field. The inherited validator treated
its first two parameter octets as a 16-bit group length and rejected the frame.
The parser now validates the remainder as bounded parameter TLVs. A regression
fixture uses the exact hardware XID, accepts all 77 bytes, queues an XID
response, and rejects a one-byte truncation.

Hardware attempt `c59c9a91-0fb5-4724-a920-7cb07aff45f3` then established LAPM,
authenticated PPP, negotiated 49,296 bit/s, assigned `10.64.0.2` and fetched an
HTTP resource with status 200. A follow-up fix marks LAPM establishment as the
startup data evidence, preventing the raw asynchronous PPP watchdog from
starting an unrelated retrain. Attempt
`23d80f5e-b9b5-45ae-8889-d1d98ff156e5` stayed connected beyond that deadline
and completed two HTTP 200 probes 15 seconds apart with zero CRC, timeout,
alignment, framing or overrun errors.

One intervening call established LAPM but stalled during PPP authentication
after two caller information frames. Another selected an ODP-qualified stream
but received no XID before the caller timed out. Its captured downstream PCM
contains a valid XID response, UA and three CRC-valid information frames, and a
synthetic two-second downstream interruption recovers exact 16 KiB transfers
in both directions. These cold-call failures remain a reliability measurement
to track; they do not invalidate the complete hardware PPP/internet results.

Retained commit `70614d7` completed production attempt
`3eaf6728-7bc4-4c67-bed5-b59b6b0f247d` at 49,296 bit/s. PPP assigned
`10.64.0.2`, and two HTTP 200 probes 15 seconds apart completed with zero modem
errors through 23.594 seconds of connected time. The service remained healthy
after automated disconnect.
