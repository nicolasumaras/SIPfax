# Pinned SpanDSP V.42 subset

This directory contains the minimal source and headers needed for V.42/LAPM
from SpanDSP revision `8f1e1646bdec99eac5fd2cd92c35563f736b9b89`.
The upstream project is <https://github.com/freeswitch/spandsp>. The files keep
their original copyright and LGPL-2.1 notices; `COPYING` contains the licence.

`src/v42.c` is generated from that pinned source by the corrections exercised
in `research/v90/v42-lab/run.py`. Reproduce it with:

```sh
python3 research/v90/v42-lab/run.py \
  --source /path/to/pinned/spandsp \
  --output /tmp/v42-report.json \
  --emit-source vendor/spandsp-v42/src/v42.c
```

The runner intentionally exits 1 because one corrected dense-error case misses
the primary 120-second deadline; the long diagnostic completes with exact data.
An emitted source file remains valid when the report contains only that known
failure. Its expected SHA-256 is
`9ce7722543d8dd3b5d792a93b8f8e777333426965e5e352fbb67aaa4317f4df9`.

This subset is linked into the native modem for reproducible development. No
LAPM runtime path is enabled merely by linking it.
