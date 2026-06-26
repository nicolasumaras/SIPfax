#!/usr/bin/env python3

from __future__ import annotations

import json
import os
import selectors
import struct
import subprocess
import sys
import termios
import time
from pathlib import Path


FRAME_SAMPLES = 160
PCMU_IDLE = b"\xff" * FRAME_SAMPLES
FIXTURE = b"~sipfax-hdlc-pty-fixture}\x00\x11"


def read_json_event(fd: int, timeout_seconds: float) -> dict:
    selector = selectors.DefaultSelector()
    selector.register(fd, selectors.EVENT_READ)
    deadline = time.monotonic() + timeout_seconds
    buffer = b""

    while time.monotonic() < deadline:
        ready = selector.select(max(0.0, min(0.1, deadline - time.monotonic())))
        if not ready:
            continue
        chunk = os.read(fd, 4096)
        if not chunk:
            break
        buffer += chunk
        while b"\n" in buffer:
            line, buffer = buffer.split(b"\n", 1)
            if line:
                return json.loads(line.decode("utf-8"))

    raise TimeoutError("timed out waiting for worker control event")


def read_exact(fd: int, length: int, timeout_seconds: float) -> bytes:
    selector = selectors.DefaultSelector()
    selector.register(fd, selectors.EVENT_READ)
    deadline = time.monotonic() + timeout_seconds
    buffer = b""

    while len(buffer) < length and time.monotonic() < deadline:
        ready = selector.select(max(0.0, min(0.1, deadline - time.monotonic())))
        if not ready:
            continue
        chunk = os.read(fd, length - len(buffer))
        if not chunk:
            break
        buffer += chunk

    if len(buffer) != length:
        raise TimeoutError(f"wanted {length} bytes, received {len(buffer)}")
    return buffer


def read_audio_frame(fd: int, timeout_seconds: float) -> bytes:
    header = read_exact(fd, 2, timeout_seconds)
    length = struct.unpack(">H", header)[0]
    return read_exact(fd, length, timeout_seconds)


def main() -> int:
    if len(sys.argv) != 2:
        print(f"usage: {sys.argv[0]} /path/to/sipfax-softmodem", file=sys.stderr)
        return 2

    worker = Path(sys.argv[1]).resolve()
    control_r, control_w = os.pipe()
    os.set_inheritable(control_w, True)

    proc = subprocess.Popen(
        [str(worker)],
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        close_fds=True,
        pass_fds=(control_w,),
        env={
            **os.environ,
            "SIPFAX_MODEM_CODEC": "PCMU",
            "SIPFAX_MODEM_PAYLOAD_TYPE": "0",
            "SIPFAX_MODEM_CLOCK_RATE": "8000",
            "SIPFAX_MODEM_CONTROL_FD": str(control_w),
            "SIPFAX_MODEM_FORCE_DATA_MODE": "1",
        },
    )
    os.close(control_w)

    try:
        assert proc.stdin is not None
        assert proc.stdout is not None

        pty_event = None
        try:
            for _ in range(5):
                event = read_json_event(control_r, 5.0)
                if event.get("lastEvent") == "pty-opened":
                    pty_event = event
                    break
        except TimeoutError as error:
            returncode = proc.poll()
            if returncode is None:
                proc.kill()
                proc.wait()
                returncode = proc.returncode
            stderr = proc.stderr.read().decode("utf-8", errors="replace") if proc.stderr else ""
            raise TimeoutError(f"{error}; workerReturncode={returncode} stderr={stderr!r}") from error
        if not pty_event:
            raise AssertionError("worker did not emit pty-opened")

        slave_path = pty_event.get("ptySlavePath")
        if not slave_path:
            raise AssertionError(f"pty-opened event has no pty path: {pty_event}")

        slave_fd = os.open(slave_path, os.O_RDWR | os.O_NOCTTY)
        try:
            attrs = termios.tcgetattr(slave_fd)
            attrs[0] = 0
            attrs[1] = 0
            attrs[3] = 0
            attrs[2] = (attrs[2] & ~(termios.CSIZE | termios.PARENB)) | termios.CS8
            attrs[6][termios.VMIN] = 1
            attrs[6][termios.VTIME] = 0
            termios.tcsetattr(slave_fd, termios.TCSANOW, attrs)
            os.write(slave_fd, FIXTURE)
            outbound = []
            for _ in range(24):
                proc.stdin.write(struct.pack(">H", len(PCMU_IDLE)))
                proc.stdin.write(PCMU_IDLE)
                proc.stdin.flush()
                outbound.append(read_audio_frame(proc.stdout.fileno(), 5.0))
        finally:
            os.close(slave_fd)

        changed_frames = [frame for frame in outbound if frame != PCMU_IDLE]
        if not changed_frames:
            raise AssertionError("pty fixture did not produce outbound modem audio")

        proc.stdin.close()
        proc.wait(timeout=5.0)
        if proc.returncode != 0:
            stderr = proc.stderr.read().decode("utf-8", errors="replace") if proc.stderr else ""
            raise AssertionError(f"worker exited {proc.returncode}: {stderr}")

        print(json.dumps({
            "ptySlavePath": slave_path,
            "fixtureBytes": len(FIXTURE),
            "outboundFrames": len(outbound),
            "changedFrames": len(changed_frames),
        }))
        return 0
    finally:
        if proc.poll() is None:
            proc.kill()
            proc.wait()


if __name__ == "__main__":
    raise SystemExit(main())
