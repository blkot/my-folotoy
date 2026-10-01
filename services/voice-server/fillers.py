"""Pre-synthesised filler phrases that cover the thinking pause.

Why this exists: IndexTTS runs at RTF ~= 1.0 on this machine, so the reply
takes about as long to generate as it does to speak. Waiting 3-5 seconds in
silence feels broken, but the model cannot go faster.

The fix is to never leave silence: the moment the user stops talking we play a
short cached phrase ("嗯，我想想"), which is audio we generated long ago, so it
starts immediately. That buys the real answer several seconds to generate, and
by the time the filler ends the next chunk is usually ready.

Design notes:
* phrases are grouped by intent so a question does not get "记下了";
* one is chosen at random per utterance to avoid sounding robotic;
* they are synthesised once, on first use, then held in memory;
* if synthesis of a filler fails we simply skip it - a missing filler must
  never break the reply.
"""

from __future__ import annotations

import logging
import random
import threading
from typing import Callable

LOGGER = logging.getLogger("fillers")

# 短语要短(约 1 秒),否则填充语本身就成了等待。而且不能透露
# "我是个程序"之类的元信息,否则对话会被打断成工具味。
FILLERS: dict[str, list[str]] = {
    "question": [
        "嗯，我想想。",
        "让我看看。",
        "这个嘛……嗯。",
    ],
    "action": [
        "好的。",
        "嗯，记下了。",
        "没问题。",
    ],
    "default": [
        "嗯。",
        "好。",
        "唔……",
    ],
}

# 粗判意图:疑问句给"想想",命令句给"好的",其余给中性。
_QUESTION_MARKS = ("吗", "呢", "什么", "怎么", "为什么", "哪", "几", "多少", "是不是")
_ACTION_MARKS = ("帮", "记", "提醒", "打开", "关", "设置", "查")


def classify(text: str) -> str:
    if any(mark in text for mark in _QUESTION_MARKS):
        return "question"
    if any(mark in text for mark in _ACTION_MARKS):
        return "action"
    return "default"


class FillerCache:
    """Synthesises filler phrases on demand and remembers the audio."""

    def __init__(self, synthesize: Callable[[str], bytes]) -> None:
        self._synthesize = synthesize
        self._cache: dict[str, bytes] = {}
        self._lock = threading.Lock()
        self._disabled = False

    def preload(self, categories: list[str] | None = None) -> None:
        """Warm the cache so the very first utterance is not delayed."""
        for category in categories or list(FILLERS):
            for phrase in FILLERS[category]:
                self._get(phrase)

    def _get(self, phrase: str) -> bytes:
        with self._lock:
            cached = self._cache.get(phrase)
        if cached is not None:
            return cached
        if self._disabled:
            return b""

        try:
            audio = self._synthesize(phrase)
        except Exception as error:  # noqa: BLE001
            # 填充语是锦上添花:合成不了就永久跳过,不要每次都重试拖慢响应。
            LOGGER.warning("filler synthesis failed (%s); disabling fillers", error)
            self._disabled = True
            return b""

        if not audio:
            self._disabled = True
            return b""

        with self._lock:
            self._cache[phrase] = audio
        LOGGER.info("filler cached: %r (%d B)", phrase, len(audio))
        return audio

    def pick(self, user_text: str) -> bytes:
        """Return audio for a filler appropriate to this utterance (may be empty)."""
        phrases = FILLERS.get(classify(user_text), FILLERS["default"])
        return self._get(random.choice(phrases))
