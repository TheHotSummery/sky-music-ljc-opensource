#!/usr/bin/env python3
"""Generate 15 short plucked-string WAVs into assets/factory/piano/."""
from __future__ import annotations

import math
import random
import struct
import wave
from pathlib import Path

SR = 22050
DUR = 0.85
# 15-key C major from C4
NAMES = ["C4", "D4", "E4", "F4", "G4", "A4", "B4", "C5", "D5", "E5", "F5", "G5", "A5", "B5", "C6"]
FREQS = [261.63 * (2 ** (i / 12.0)) for i in (0, 2, 4, 5, 7, 9, 11, 12, 14, 16, 17, 19, 21, 23, 24)]


def karplus(freq: float) -> list[int]:
    delay = max(8, int(SR / freq))
    buf = [random.uniform(-1.0, 1.0) for _ in range(delay)]
    n = int(SR * DUR)
    out = [0] * n
    idx = 0
    for i in range(n):
        nxt = (idx + 1) % delay
        x = 0.996 * 0.5 * (buf[idx] + buf[nxt])
        buf[idx] = x
        idx = nxt
        env = math.exp(-4.2 * i / n)
        att = min(1.0, i / 80.0)
        out[i] = int(max(-32767, min(32767, x * env * att * 22000)))
    return out


def write_wav(path: Path, pcm: list[int]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with wave.open(str(path), "w") as wf:
        wf.setnchannels(1)
        wf.setsampwidth(2)
        wf.setframerate(SR)
        wf.writeframes(b"".join(struct.pack("<h", s) for s in pcm))


def main() -> None:
    random.seed(15)
    root = Path(__file__).resolve().parents[1] / "assets" / "factory" / "piano"
    for i, (name, freq) in enumerate(zip(NAMES, FREQS), start=1):
        write_wav(root / f"{i}.wav", karplus(freq))
        print(f"wrote {root / f'{i}.wav'}  {name}  {freq:.1f}Hz")


if __name__ == "__main__":
    main()
