"""Text to speech via a local IndexTTS server.

IndexTTS exposes an OpenAI-compatible ``/v1/audio/speech`` endpoint, so this
module speaks the same protocol as any commercial TTS: swapping to a cloud
provider later means changing ``base_url``, nothing else.

Two device-specific details live here:

* we ask for ``response_format="pcm"`` because the ESP32 wants raw 16-bit
  little-endian samples with no container to parse;
* IndexTTS synthesises at 22050 Hz, while the device captures and plays at
  16 kHz, so the reply is resampled before it goes on the wire.

Speed note: this model runs at RTF ~= 0.96 on the target machine (measured
across every generation parameter combination), i.e. it needs almost as long to
synthesise as the audio lasts. That is fine for playback but fatal for
latency, which is why the server synthesises **per sentence and streams each
one as soon as it is ready** rather than waiting for the whole reply.
"""

from __future__ import annotations

import audioop
import json
import logging
import re
import time
import urllib.error
import urllib.request
import wave
from dataclasses import dataclass
from typing import Iterator

from config import TTSSettings

LOGGER = logging.getLogger("tts")

# 按中英文句末标点切句。保留标点,让 TTS 能读出正确的语调。
_SENTENCE_SPLIT = re.compile(r"(?<=[。！？!?；;])\s*|(?<=[.!?])\s+")
_MIN_SEGMENT_CHARS = 4
_HARD_SPLIT = re.compile(r"(?<=[，,、])")


def split_sentences(text: str, max_chars: int = 60) -> list[str]:
    """Split a reply into speakable chunks.

    Two goals pull in opposite directions:短句 让首包更快, 但太短会让韵律断裂
    and adds per-call overhead (each call has fixed model warm-up cost). So we
    merge anything below ``_MIN_SEGMENT_CHARS`` and hard-split anything above
    ``max_chars`` at a comma so a single long sentence cannot dominate the wait.
    """
    raw = [part.strip() for part in _SENTENCE_SPLIT.split(text) if part.strip()]
    if not raw:
        return [text.strip()] if text.strip() else []

    # 合并过短的碎片(比如单独的"好。"),避免为几个字付一次模型开销。
    merged: list[str] = []
    for part in raw:
        if merged and len(merged[-1]) < _MIN_SEGMENT_CHARS:
            merged[-1] += part
        else:
            merged.append(part)

    # 一句太长就按逗号再切,保证首包不会被一个长句拖住。
    chunks: list[str] = []
    for part in merged:
        if len(part) <= max_chars:
            chunks.append(part)
            continue
        buffer = ""
        for piece in _HARD_SPLIT.split(part):
            if not piece:
                continue
            if buffer and len(buffer) + len(piece) > max_chars:
                chunks.append(buffer)
                buffer = piece
            else:
                buffer += piece
        if buffer:
            chunks.append(buffer)
    return [chunk for chunk in chunks if chunk.strip()]


@dataclass
class Speech:
    pcm: bytes
    sample_rate: int
    latency: float
    error: str = ""


def _resample(pcm: bytes, source_rate: int, target_rate: int) -> bytes:
    """Linear resample of 16-bit mono PCM.

    audioop.ratecv is the standard-library tool for this: no scipy/soxr
    dependency, and at 24k->16k on short utterances the quality difference is
    inaudible for a toy speaker.
    """
    if source_rate == target_rate:
        return pcm
    converted, _ = audioop.ratecv(pcm, 2, 1, source_rate, target_rate, None)
    return converted


def normalize(pcm: bytes, target_peak: float) -> bytes:
    """Scale 16-bit PCM up to ``target_peak`` of full scale.

    Measured on this setup: IndexTTS peaks at only ~19% of full scale, so the
    device ends up amplifying a very quiet signal. Normalising spends the
    headroom we already have, which is worth roughly 13 dB - a much bigger win
    than raising the speaker volume, because that amplifies noise too.

    Deliberately leaves a little headroom instead of hitting 1.0: the device
    applies its own gain and clipping there is far uglier than being a hair
    quiet. Returns the input unchanged when it is silent or the gain is off.
    """
    if target_peak <= 0 or len(pcm) < 2:
        return pcm

    import array

    samples = array.array("h")
    samples.frombytes(pcm[: len(pcm) // 2 * 2])
    if not samples:
        return pcm

    peak = max(max(samples), -min(samples))
    if peak == 0:
        return pcm

    full_scale = 32767.0 * target_peak
    if peak >= full_scale:
        return pcm  # 已经够响,不要做衰减,免得改变相对音量

    gain = full_scale / peak
    scaled = array.array("h", (int(s * gain) for s in samples))
    # 逐样本钳位,防止浮点四舍五入后越界。
    for index, value in enumerate(scaled):
        if value > 32767:
            scaled[index] = 32767
        elif value < -32768:
            scaled[index] = -32768
    return scaled.tobytes()


def _wav_to_pcm(blob: bytes) -> tuple[bytes, int]:
    """Extract raw PCM and the rate from a WAV container."""
    import io

    with wave.open(io.BytesIO(blob), "rb") as handle:
        if handle.getsampwidth() != 2:
            raise ValueError(f"expected 16-bit samples, got {handle.getsampwidth() * 8}")
        channels = handle.getnchannels()
        rate = handle.getframerate()
        pcm = handle.readframes(handle.getnframes())
    if channels > 1:
        pcm = audioop.tomono(pcm, 2, 0.5, 0.5)
    return pcm, rate


class Speaker:
    def __init__(self, settings: TTSSettings) -> None:
        self.settings = settings

    def stream(self, text: str) -> Iterator[bytes]:
        """Yield PCM sentence by sentence, as soon as each one is ready.

        This is the whole latency strategy: the caller forwards each chunk to
        the device immediately, so playback starts after the *first sentence*
        instead of after the entire reply. Because RTF < 1 the remaining
        sentences generate faster than the device plays them, so playback never
        starves.
        """
        for index, sentence in enumerate(split_sentences(text), start=1):
            speech = self.synthesize(sentence)
            if speech.error:
                LOGGER.warning("sentence %d failed (%s); skipping", index, speech.error)
                continue
            if speech.pcm:
                LOGGER.info("sentence %d/%d ready: %d B", index, len(split_sentences(text)),
                            len(speech.pcm))
                yield speech.pcm

    def synthesize(self, text: str) -> Speech:
        if not text.strip():
            return Speech(pcm=b"", sample_rate=self.settings.target_rate, latency=0.0)

        body = json.dumps({
            "model": self.settings.model,
            "input": text,
            "voice": self.settings.voice,
            "response_format": "pcm",
        }).encode("utf-8")

        headers = {"Content-Type": "application/json"}
        key = self.settings.resolved_key()
        if key:
            headers["Authorization"] = f"Bearer {key}"

        request = urllib.request.Request(
            f"{self.settings.base_url.rstrip('/')}/v1/audio/speech",
            data=body, headers=headers, method="POST",
        )

        started = time.monotonic()
        try:
            with urllib.request.urlopen(request, timeout=self.settings.timeout_s) as response:
                blob = response.read()
                content_type = response.headers.get("Content-Type", "")
        except urllib.error.HTTPError as error:
            detail = error.read().decode("utf-8", "replace")[:200]
            LOGGER.error("TTS HTTP %s: %s", error.code, detail)
            return Speech(b"", self.settings.target_rate,
                          time.monotonic() - started, f"HTTP {error.code}")
        except (urllib.error.URLError, TimeoutError, OSError) as error:
            LOGGER.error("TTS request failed: %s", error)
            return Speech(b"", self.settings.target_rate,
                          time.monotonic() - started, str(error))

        latency = time.monotonic() - started
        if not blob:
            return Speech(b"", self.settings.target_rate, latency, "empty response")

        # IndexTTS honours response_format=pcm, so we get header-less samples.
        # There is no rate in the payload, so the source rate MUST come from
        # config: assuming the target rate here silently produces chipmunk audio.
        if blob[:4] == b"RIFF":
            pcm, rate = _wav_to_pcm(blob)
        else:
            pcm, rate = blob, self.settings.source_rate

        resampled = _resample(pcm, rate, self.settings.target_rate)
        # 归一化放在重采样之后:重采样会轻微改变峰值,顺序反了会留下削波风险。
        leveled = normalize(resampled, self.settings.target_peak)
        LOGGER.info("TTS %.2f s -> %.2f s of audio (%d Hz -> %d Hz, %d B), "
                    "normalised to %.0f%% peak",
                    latency, len(leveled) / (2.0 * self.settings.target_rate),
                    rate, self.settings.target_rate, len(leveled),
                    self.settings.target_peak * 100)
        return Speech(leveled, self.settings.target_rate, latency)
