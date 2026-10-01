"""Local speech-to-text with faster-whisper, running on the PC's GPU.

The device never sees this: it just ships PCM over TCP and gets text back.
Keeping STT local means no API cost and no per-utterance network latency,
which is why the heavy lifting lives here rather than on the ESP32.

Model resolution order (first hit wins):
  1. an explicit local directory (``--model`` pointing at a folder)
  2. ``VOICE_WHISPER_MODEL`` environment variable
  3. the local CTranslate2 conversion of whisper-large-v3-turbo, if present
  4. the model name as-is (would trigger a HuggingFace download)

Step 3 exists because the 3 GB ``faster-whisper-large-v3`` download stalls on
networks where HuggingFace's Xet transfer domain is unreachable, while
``openai/whisper-large-v3-turbo`` may already be cached locally. See
``convert_whisper_to_ct2.py``.
"""

from __future__ import annotations

import logging
import os
import time
from dataclasses import dataclass
from pathlib import Path

LOGGER = logging.getLogger("stt")

# 本地转换好的模型目录(见 convert_whisper_to_ct2.py)。
LOCAL_TURBO_CT2 = Path(r"H:\esp\whisper-models\large-v3-turbo-ct2")


def resolve_model(model: str) -> str:
    """Turn a model name into something faster-whisper can load without network."""
    if Path(model).is_dir():
        return model

    from_env = os.environ.get("VOICE_WHISPER_MODEL", "").strip()
    if from_env:
        return from_env

    if model == "large-v3" and LOCAL_TURBO_CT2.is_dir():
        LOGGER.info("using local turbo conversion at %s instead of downloading large-v3",
                    LOCAL_TURBO_CT2)
        return str(LOCAL_TURBO_CT2)

    return model


def _add_cuda_dll_dirs() -> None:
    """Make ctranslate2 able to find the pip-installed cuBLAS/cuDNN runtimes.

    The ``nvidia-*-cu12`` wheels drop their DLLs in ``site-packages/nvidia/*/bin``
    and do NOT put that on PATH, so a bare ``import ctranslate2`` on Windows
    fails with "could not load cudnn". Adding the directories here keeps the
    fix local to this module instead of requiring a shell tweak.
    """
    try:
        import site

        roots = site.getsitepackages()
    except Exception:  # noqa: BLE001
        roots = []

    for root in roots:
        for sub in ("cublas", "cudnn", "cuda_nvrtc"):
            candidate = os.path.join(root, "nvidia", sub, "bin")
            if os.path.isdir(candidate):
                try:
                    os.add_dll_directory(candidate)
                except (AttributeError, OSError):
                    pass


@dataclass
class Transcript:
    text: str
    language: str
    duration: float
    latency: float


class Transcriber:
    """Lazy-loading wrapper so the server can start before the model is ready."""

    def __init__(self, model_name: str = "large-v3", device: str = "cuda",
                 compute_type: str = "float16", language: str = "zh") -> None:
        self.model_name = model_name
        self.device = device
        self.compute_type = compute_type
        self.language = language
        self._model = None

    @property
    def loaded(self) -> bool:
        return self._model is not None

    def load(self) -> None:
        if self._model is not None:
            return

        _add_cuda_dll_dirs()
        from faster_whisper import WhisperModel

        resolved = resolve_model(self.model_name)
        self.model_name = resolved

        # 本地 CTranslate2 目录只需要 model.bin/config.json/vocabulary.json,
        # 但 faster-whisper 仍会去 HuggingFace 拉 tokenizer.json。断网时那一步
        # 会抛 LocalEntryNotFoundError —— 之前被下面的 except 误判成"显存不足",
        # 于是回退 CPU 又失败一次,错误信息完全指向错误的方向。
        # 用 offline 模式把这类失败直接暴露成"模型文件不全",不再误导。
        os.environ.setdefault("HF_HUB_OFFLINE", "1")
        os.environ.setdefault("TRANSFORMERS_OFFLINE", "1")

        LOGGER.info("loading %s on %s (%s) ...", resolved, self.device, self.compute_type)
        started = time.monotonic()
        try:
            self._model = WhisperModel(resolved, device=self.device,
                                       compute_type=self.compute_type)
        except Exception as error:  # noqa: BLE001
            # 只在"疑似显存不足"时回退 CPU;其他错误直接抛出,避免把
            # 网络/文件问题伪装成硬件问题。
            message = str(error).lower()
            out_of_memory = any(token in message for token in
                                ("out of memory", "cuda", "cublas", "cudnn",
                                 "no kernel image", "device"))
            if not out_of_memory:
                raise
            LOGGER.warning("GPU load failed (%s); falling back to CPU int8. "
                           "另一个进程(如 ComfyUI)可能占满了显存。", error)
            self.device = "cpu"
            self.compute_type = "int8"
            self._model = WhisperModel(resolved, device="cpu", compute_type="int8")
        LOGGER.info("model ready in %.1f s", time.monotonic() - started)

    def transcribe(self, pcm: bytes, sample_rate: int) -> Transcript:
        self.load()
        import numpy as np

        audio = np.frombuffer(pcm, dtype=np.int16).astype(np.float32) / 32768.0
        duration = len(audio) / sample_rate if sample_rate else 0.0

        started = time.monotonic()
        segments, info = self._model.transcribe(
            audio,
            language=self.language,
            beam_size=1,             # 对话场景要的是快;beam=1 损失很小
            vad_filter=True,         # 掐掉静音,避免空录音产生幻觉文本
            condition_on_previous_text=False,   # 每次都是独立一句,防止串词
        )
        text = "".join(segment.text for segment in segments).strip()
        latency = time.monotonic() - started

        LOGGER.info("transcribed %.2f s audio in %.2f s -> %r", duration, latency, text)
        return Transcript(text=text, language=info.language, duration=duration,
                          latency=latency)
