#!/usr/bin/env python3
"""Verify real incremental builds in a disposable source copy."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
with tempfile.TemporaryDirectory(prefix="sipfax-build-deps-") as temporary:
    vendor = Path(temporary) / "vendor"
    for name in ("linmodem", "spandsp-v42"):
        shutil.copytree(ROOT / "vendor" / name, vendor / name,
                        ignore=shutil.ignore_patterns("*.o", "*.d"))
    modem = vendor / "linmodem"
    command = ["make", "-C", str(modem), "-j2"]

    def build(*targets):
        result = subprocess.run(command + list(targets), text=True,
                                stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        if result.returncode:
            raise AssertionError(result.stdout)

    build("clean")
    build()
    for header, objects in (
        (modem / "v90echodelay.h", [modem / "v90echodelay.o", modem / "linpipe.o"]),
        (vendor / "spandsp-v42/src/spandsp/private/v42.h",
         [vendor / "spandsp-v42/src/v42.o", modem / "v90lapmlink.o"]),
    ):
        before = {p: p.stat().st_mtime_ns for p in objects + [modem / "lm"]}
        # Set the header just after the newest output without relying on sleeps.
        timestamp = max(before.values()) + 1
        os.utime(header, ns=(timestamp, timestamp))
        build()
        for path, previous in before.items():
            assert path.stat().st_mtime_ns > previous, f"not rebuilt: {path}"
        unchanged = (modem / "lm").stat().st_mtime_ns
        build()
        assert (modem / "lm").stat().st_mtime_ns == unchanged, "no-op build relinked"
    build("clean")
    assert not list(vendor.rglob("*.d")), "clean left dependency files"
print("PASS: native and SpanDSP header rebuilds, no-op build, clean")
