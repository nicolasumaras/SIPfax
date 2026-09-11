# V.42 error-control integration assessment

The current V.90 link uses asynchronous PPP without LAPM. Occasional received errors therefore reach PPP and can require TCP retransmissions. V.42 would add modem-level error recovery; it would not repair an underlying PCM or playback problem, and should not be claimed as working until hardware interoperability is demonstrated.

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
