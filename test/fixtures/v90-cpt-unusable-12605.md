# Unusable six-mask V.90 training request

This fixture contains one 1788-bit, CRC-valid CPt message captured during full retraining on 2026-09-09. Each byte stores one decoded bit (0 or 1). It contains only modem constellation parameters; no PPP data or credentials.

The message requests drn 9 and Sr 1, requiring K=12 magnitude bits. Its six selected transmit masks contain 3, 3, 3, 5, 3 and 3 entries: 1215 combinations, below the required 4096. The decoder must accept the message CRC, while the mapper must reject its capacity. The startup controller must initiate recovery rather than remain in failed-stage silence.

`tools/tests/v90-startup.py` parses this exact message and verifies the retrain, 70 ms mute and DTE clamp. The source audio remains private and is not required by the test.
