#!/usr/bin/env python3

from __future__ import annotations

import argparse
import json
import os
import selectors
import struct
import subprocess
import sys
import tempfile
import time
import wave
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[2]
DEFAULT_WAV = REPO_ROOT / "artifacts" / "lkma-193a" / "groundtruth-v21.wav"
FALLBACK_WAV = REPO_ROOT / "artifacts" / "lkma-193a" / "ground-truth" / "v21" / "inbound.wav"
FRAME_SAMPLES = 160


def linear_to_ulaw(sample: int) -> int:
    bias = 0x84
    if sample >= 0:
        sample = min(sample + bias, 32635)
        mask = 0xFF
    else:
        sample = min(bias - sample, 32635)
        mask = 0x7F

    segment = 7
    for candidate in range(8):
        if sample <= (0x1F << (candidate + 3)):
            segment = candidate
            break

    return ((segment << 4) | ((sample >> (segment + 3)) & 0x0F)) ^ mask


def pcm16_wav_to_ulaw_frames(path: Path) -> list[bytes]:
    with wave.open(str(path), "rb") as wav:
        if wav.getnchannels() != 1 or wav.getsampwidth() != 2 or wav.getframerate() != 8000:
            raise ValueError(f"{path} must be mono 16-bit PCM at 8000 Hz")

        pcm = wav.readframes(wav.getnframes())

    frames: list[bytes] = []
    for offset in range(0, len(pcm), FRAME_SAMPLES * 2):
        chunk = pcm[offset:offset + FRAME_SAMPLES * 2]
        if len(chunk) < FRAME_SAMPLES * 2:
            chunk = chunk + (b"\x00" * (FRAME_SAMPLES * 2 - len(chunk)))
        samples = struct.unpack("<" + "h" * FRAME_SAMPLES, chunk)
        frames.append(bytes(linear_to_ulaw(sample) for sample in samples))
    return frames


def read_available_lines(fd: int, timeout_seconds: float) -> list[dict]:
    events: list[dict] = []
    selector = selectors.DefaultSelector()
    selector.register(fd, selectors.EVENT_READ)
    deadline = time.monotonic() + timeout_seconds
    buffer = b""

    while time.monotonic() < deadline:
        remaining = max(0.0, deadline - time.monotonic())
        ready = selector.select(min(0.1, remaining))
        if not ready:
            continue
        chunk = os.read(fd, 4096)
        if not chunk:
            break
        buffer += chunk
        while b"\n" in buffer:
            line, buffer = buffer.split(b"\n", 1)
            if line:
                events.append(json.loads(line.decode("utf-8")))

    if buffer.strip():
        events.append(json.loads(buffer.decode("utf-8")))
    return events


def run_replay(worker: Path, wav_path: Path, timeout_seconds: float) -> tuple[list[dict], bytes]:
    frames = pcm16_wav_to_ulaw_frames(wav_path)
    control_r, control_w = os.pipe()
    os.set_inheritable(control_w, True)

    with tempfile.TemporaryDirectory() as tmp:
        data_path = Path(tmp) / "decoded-v21.bin"

        def child_setup() -> None:
            os.dup2(control_w, 3)

        proc = subprocess.Popen(
            [str(worker)],
            stdin=subprocess.PIPE,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.PIPE,
            close_fds=False,
            preexec_fn=child_setup,
            env={
                **os.environ,
                "SIPFAX_MODEM_CODEC": "PCMU",
                "SIPFAX_MODEM_PAYLOAD_TYPE": "0",
                "SIPFAX_MODEM_CLOCK_RATE": "8000",
                "SIPFAX_MODEM_CONTROL_FD": "3",
                "SIPFAX_MODEM_DATA_OUT": str(data_path),
            },
        )
        os.close(control_w)

        assert proc.stdin is not None
        for frame in frames:
            proc.stdin.write(struct.pack(">H", len(frame)))
            proc.stdin.write(frame)
        proc.stdin.close()

        events = read_available_lines(control_r, timeout_seconds)
        try:
            proc.wait(timeout=timeout_seconds)
        except subprocess.TimeoutExpired:
            proc.kill()
            proc.wait()
            raise

        stderr = proc.stderr.read().decode("utf-8", errors="replace") if proc.stderr else ""
        if proc.returncode != 0:
            raise RuntimeError(f"worker exited {proc.returncode}: {stderr}")

        return events, data_path.read_bytes() if data_path.exists() else b""


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--worker", type=Path, required=True)
    parser.add_argument("--wav", type=Path, default=DEFAULT_WAV)
    parser.add_argument("--timeout", type=float, default=5.0)
    parser.add_argument("--allow-missing", action="store_true")
    args = parser.parse_args()

    wav_path = args.wav
    if not wav_path.exists() and args.wav == DEFAULT_WAV and FALLBACK_WAV.exists():
        wav_path = FALLBACK_WAV

    if not wav_path.exists():
        if args.allow_missing:
            print(json.dumps({"skipped": True, "reason": f"missing {wav_path}"}))
            return 0
        print(f"missing V.21 ground-truth WAV: {wav_path}", file=sys.stderr)
        return 2

    events, decoded = run_replay(args.worker, wav_path, args.timeout)
    matched = [
        event for event in events
        if event.get("state") == "data-mode" and event.get("modulation") == "V.21"
    ]
    if not matched:
        print(json.dumps({"events": events[-10:], "decodedBytes": len(decoded)}, indent=2), file=sys.stderr)
        return 1
    print(json.dumps({"state": "data-mode", "modulation": "V.21", "decodedBytes": len(decoded)}))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
