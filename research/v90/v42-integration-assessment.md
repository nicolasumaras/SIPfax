# V.42 error-control integration assessment

The current V.90 link uses asynchronous PPP without LAPM. Occasional received errors therefore reach PPP and can require TCP retransmissions. V.42 would add modem-level error recovery; it would not repair an underlying PCM or playback problem, and should not be claimed as working until hardware interoperability is demonstrated.

The upstream [SpanDSP V.42 source](https://github.com/freeswitch/spandsp/blob/8f1e1646bdec99eac5fd2cd92c35563f736b9b89/src/v42.c) is explicitly labelled unfinished. It provides detection, HDLC and LAPM state handling under the source file's LGPL-2.1 terms. It is a reference candidate, not a drop-in dependency qualified for this server.

Concrete integration checks:

- Its timers count calls to `v42_tx_bit`, and `v42_init` hardcodes 28800 bit/s. The digital V.90 transmitter runs at the negotiated downstream rate, often 148000/3 bit/s. Timers must follow real elapsed sample time or the actual transmit rate, including pauses and retraining.
- The default configuration advertises compression (`comp = 1`) with a TODO about V.42bis startup. Disable unsupported compression or implement and independently qualify the negotiated compressor; never advertise a capability solely because a default enables it.
- The [upstream test](https://github.com/freeswitch/spandsp/blob/8f1e1646bdec99eac5fd2cd92c35563f736b9b89/tests/v42_tests.c) declares error-injection periods as `bool` and assigns 10000/11000. Those become true, so the `-b` branch flips every bit rather than injecting sparse faults. It also ends with success without a final data-integrity assertion. Its exit status is not sufficient evidence of reliable error recovery.
- First build an independent two-peer harness with asymmetric clocks, verified bidirectional payloads, deterministic corrupted/dropped frames, retransmission/duplicate checks, bounded memory and busy/backpressure tests. Then integrate synchronous bits before the existing UART parser and after the V.90 descrambler, with a corresponding downstream bit source, retaining asynchronous fallback and clean retrain behavior.

No V.42/LAPM implementation was enabled or deployed during this assessment. The existing optional E/NUL decline handshake is not LAPM.
