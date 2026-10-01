"""Measure real STT latency on the actual converted model.

Run this before wiring anything to the device: it answers "is large-v3-turbo
fast enough for conversation?" with a number instead of a guess.
"""

from __future__ import annotations

import os
import struct
import sys
import time
import wave
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "voice-server"))

from stt import Transcriber  # noqa: E402


def synthetic_speech(seconds: float, rate: int = 16000) -> bytes:
    """A few tones and pauses; not speech, but it makes Whisper do real work."""
    import math

    samples = []
    for index in range(int(rate * seconds)):
        position = index / rate
        if 0.3 < position < 1.0 or 1.4 < position < 2.2:
            value = 12000 * math.sin(2 * math.pi * 180 * position)
            value += 5000 * math.sin(2 * math.pi * 700 * position)
        else:
            value = 0
        samples.append(int(value))
    return struct.pack(f"<{len(samples)}h", *samples)


def main() -> int:
    wav = Path(sys.argv[1]) if len(sys.argv) > 1 else None
    if wav and wav.is_file():
        with wave.open(str(wav), "rb") as handle:
            pcm = handle.readframes(handle.getnframes())
            rate = handle.getframerate()
        print(f"using {wav.name}: {len(pcm)} B @ {rate} Hz")
    else:
        rate = 16000
        pcm = synthetic_speech(2.5, rate)
        print(f"using {len(pcm)} B of synthetic audio @ {rate} Hz")

    transcriber = Transcriber(model_name="large-v3", device="cuda",
                              compute_type="float16", language="zh")

    started = time.monotonic()
    transcriber.load()
    print(f"load          : {time.monotonic() - started:.2f} s")
    print(f"resolved model: {transcriber.model_name}")
    print(f"device        : {transcriber.device} ({transcriber.compute_type})")

    # 第一次包含 CUDA kernel 编译,单独计时避免误判稳态性能。
    first = transcriber.transcribe(pcm, rate)
    print(f"first call    : {first.latency:.2f} s (includes CUDA warm-up)")

    runs = []
    for _ in range(3):
        result = transcriber.transcribe(pcm, rate)
        runs.append(result.latency)
    print(f"steady state  : {min(runs):.2f} - {max(runs):.2f} s "
          f"(audio {len(pcm) / (2 * rate):.2f} s)")
    print(f"realtime factor: {len(pcm) / (2 * rate) / (sum(runs) / len(runs)):.1f}x")
    print(f"transcript    : {result.text!r}")
    print("STT LATENCY TEST: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
