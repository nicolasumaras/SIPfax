#!/usr/bin/env python3
"""Compare a default native build with a clean build in another directory.

Run immediately after `make -C vendor/linmodem`, with the same compiler and
unmodified sources. This checks path independence, not cross-compiler identity.
"""
import hashlib
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

repo = Path(__file__).resolve().parents[2]
binary = Path(sys.argv[1]).resolve()
expected = hashlib.sha256(binary.read_bytes()).hexdigest()
with tempfile.TemporaryDirectory(prefix='sipfax-native-repro-') as directory:
    root = Path(directory)
    for name in ('linmodem', 'spandsp-v42'):
        shutil.copytree(repo / 'vendor' / name, root / 'vendor' / name,
                        ignore=shutil.ignore_patterns('*.o', '*.d', '__pycache__',
                                                     'lm', 'v34gen', 'v90gen'))
    result = subprocess.run(['make', '-C', str(root / 'vendor/linmodem'), '-j2'],
                            capture_output=True, text=True, timeout=300)
    if result.returncode:
        sys.stderr.write((result.stdout + result.stderr)[-6000:])
    result.check_returncode()
    actual = hashlib.sha256((root / 'vendor/linmodem/lm').read_bytes()).hexdigest()
    assert actual == expected, f'Native build differs across directories: {expected} != {actual}'
print(f'PASS: native SHA-256 {actual} matches across build directories')
