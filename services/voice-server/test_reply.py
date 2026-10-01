"""End-to-end reply-chain test: text in, audio out. No device required.

Exercises exactly what the device triggers after an utterance, so LLM/TTS
problems can be debugged without holding a button on the toy.
"""

from __future__ import annotations

import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from config import load_settings  # noqa: E402
from llm import Conversation  # noqa: E402
from tts import Speaker  # noqa: E402


def main() -> int:
    settings = load_settings()
    text = " ".join(sys.argv[1:]) or "你好，请用一句话介绍你自己"

    print(f"input        : {text}")

    # ---- LLM ----
    conversation = Conversation(settings.llm)
    print(f"llm          : {settings.llm.model} @ {settings.llm.base_url}")
    print(f"llm key      : {'set' if settings.llm.resolved_key() else 'MISSING'}")
    reply = conversation.reply(text)
    if reply.error:
        print(f"LLM FAILED   : {reply.error}")
        return 1
    print(f"reply ({reply.latency:.2f} s): {reply.text}")

    # ---- TTS ----
    speaker = Speaker(settings.tts)
    print(f"tts          : {settings.tts.voice} @ {settings.tts.base_url}")
    started = time.monotonic()
    speech = speaker.synthesize(reply.text)
    if speech.error:
        print(f"TTS FAILED   : {speech.error}")
        return 1

    seconds = len(speech.pcm) / (2.0 * speech.sample_rate)
    print(f"audio ({speech.latency:.2f} s): {len(speech.pcm)} B = {seconds:.2f} s "
          f"@ {speech.sample_rate} Hz")

    out = Path(__file__).resolve().parent / "last_reply.wav"
    import wave

    with wave.open(str(out), "wb") as handle:
        handle.setnchannels(1)
        handle.setsampwidth(2)
        handle.setframerate(speech.sample_rate)
        handle.writeframes(speech.pcm)
    print(f"saved        : {out}")

    silence = sum(1 for _ in range(0, min(len(speech.pcm), 3200), 2)) if speech.pcm else 0
    print(f"total        : {time.monotonic() - started:.2f} s")
    print(f"non-silent   : {any(speech.pcm)}")
    del silence
    print("REPLY CHAIN TEST: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
