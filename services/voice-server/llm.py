"""Conversation: turn a transcript into a short spoken-style reply.

Kept provider-agnostic on purpose — any OpenAI-compatible ``/chat/completions``
endpoint works (DeepSeek, 智谱, 百炼, Moonshot, OpenAI, a local Ollama...）。
Swapping providers is a config change, not a code change.
"""

from __future__ import annotations

import json
import logging
import re
import time
import urllib.error
import urllib.request
from dataclasses import dataclass, field

from config import LLMSettings

LOGGER = logging.getLogger("llm")

# 提示词已经禁止这些,但模型偶尔仍会漏出来。它们会被 TTS 念成很怪的东西
# (星号读不出来、"# "读成"井号"),所以在送进 TTS 之前再兜一层。
_MARKUP = re.compile(r"[*#`_~|<>\[\]{}]+")
_WRAPPERS = re.compile(r'[“”"\'‘’（）()【】]')
_TRAILING_LABEL = re.compile(r"^(回答|助手|答案|助理)\s*[:：]\s*")
_LIST_MARKER = re.compile(r"(?:^|\n)\s*(?:[-•·*]|\d+[.、)]|\(\d+\))\s*")
_NUMBER_PERCENT = re.compile(r"(\d+(?:\.\d+)?)\s*%")
_NUMBER_RANGE = re.compile(r"(\d)\s*[-—~]\s*(\d)")
_NUMBER_DECIMAL = re.compile(r"\d+(?:\.\d+)?")
_NUMBER_COLON_TIME = re.compile(r"(\d{1,2}):(\d{2})")
# 整词的拉丁字母:读音对 TTS 不友好,交给下面的映射表处理,查不到就保留。
_LATIN_WORD = re.compile(r"\b[A-Za-z]{2,}\b")

# 常见缩写的口语写法。宁可查表也不猜,猜错比不转换更糟。
_LATIN_SPOKEN = {
    "ai": "人工智能",
    "ok": "好的",
    "app": "应用",
    "id": "账号",
    "wifi": "无线网",
    "gps": "定位",
    "usb": "USB 接口",
    "cpu": "处理器",
    "pdf": "PDF 文件",
}


def _spell_number(value: str) -> str:
    """Turn digits into spoken Chinese so the TTS reads them naturally."""
    digits = "零一二三四五六七八九"
    if "." in value:
        whole, _, fraction = value.partition(".")
        spoken = "".join(digits[int(c)] if c.isdigit() else c for c in whole)
        spoken += "点" + "".join(digits[int(c)] if c.isdigit() else c for c in fraction)
        return spoken
    # 常见的一位到四位数读法;更长的直接逐位,总好过读成天文数字。
    if value.isdigit() and len(value) <= 4:
        number = int(value)
        if number < 10:
            return digits[number]
        if number < 20:
            # 10~19 是"十""十一"..."十九",不能说成"一十"。
            return "十" if number == 10 else "十" + digits[number % 10]
        if number < 100:
            tens, ones = divmod(number, 10)
            return digits[tens] + "十" + (digits[ones] if ones else "")
        return "".join(digits[int(c)] for c in value)
    return "".join(digits[int(c)] if c.isdigit() else c for c in value)


def sanitize_for_speech(text: str) -> str:
    """Make text safe to hand to a TTS engine.

    Deliberately conservative: it removes markup and awkward symbols rather than
    trying to rewrite meaning, because a wrong rewrite is worse than a shorter
    sentence. The system prompt does the real work; this is the safety net.
    """
    cleaned = text.strip()
    cleaned = _TRAILING_LABEL.sub("", cleaned)
    # 列表/编号先处理:符号去掉后还要补一个停顿,否则两项会黏成一句话。
    cleaned = _LIST_MARKER.sub("，", cleaned)
    cleaned = cleaned.replace("**", "").replace("##", "")
    cleaned = _MARKUP.sub("", cleaned)
    cleaned = _WRAPPERS.sub("", cleaned)
    # 百分号:中文说"百分之二十",不是"二十百分之"。
    cleaned = _NUMBER_PERCENT.sub(lambda m: "百分之" + _spell_number(m.group(1)), cleaned)
    cleaned = _NUMBER_RANGE.sub(r"\1到\2", cleaned)
    cleaned = _NUMBER_COLON_TIME.sub(
        lambda m: f"{_spell_number(m.group(1))}点"
                  f"{'零' + _spell_number(m.group(2)) if m.group(2).startswith('0') else _spell_number(m.group(2))}",
        cleaned,
    )
    cleaned = cleaned.replace(":", "，").replace(";", "，").replace("；", "，")
    cleaned = cleaned.replace("、", "，").replace("—", "，").replace("–", "，")
    cleaned = cleaned.replace("…", "。")
    # 剩余的裸数字统一转中文口语。
    cleaned = _NUMBER_DECIMAL.sub(lambda m: _spell_number(m.group(0)), cleaned)
    # 英文缩写:查表;查不到就原样留着(有些牌子名本来就读英文)。
    cleaned = _LATIN_WORD.sub(lambda m: _LATIN_SPOKEN.get(m.group(0).lower(), m.group(0)),
                              cleaned)
    cleaned = cleaned.replace("\n", "")
    # 折叠重复标点,避免 TTS 出现奇怪的长停顿。
    cleaned = re.sub(r"[，。！？]{2,}", lambda m: m.group(0)[0], cleaned)
    cleaned = re.sub(r"^[，。！？]+", "", cleaned)
    # 中文正文里不该有空格:TTS 会把空格读成停顿或吞掉。只保留
    # 拉丁字母之间的空格(品牌名之类),其余全部去掉。
    cleaned = re.sub(r"(?<=[\u4e00-\u9fff，。！？])\s+", "", cleaned)
    cleaned = re.sub(r"\s+(?=[\u4e00-\u9fff，。！？])", "", cleaned)
    cleaned = re.sub(r"\s{2,}", " ", cleaned)
    return cleaned.strip()


def _shorten(text: str, limit: int) -> str:
    """Trim a reply that ignores the length instruction.

    Cut at the last sentence boundary that fits, so the result still sounds
    complete instead of stopping mid-clause. Falls back to a hard cut with an
    ellipsis when a single sentence is longer than the whole budget.
    """
    if limit <= 0 or len(text) <= limit:
        return text

    head = text[:limit]
    boundary = max(head.rfind(mark) for mark in "。！？")
    if boundary >= limit // 3:
        LOGGER.info("reply truncated at sentence boundary (%d -> %d chars)",
                    len(text), boundary + 1)
        return head[: boundary + 1]

    # 没有合适的句末:退到逗号,再加句号收尾,免得听上去被掐断。
    comma = max(head.rfind(mark) for mark in "，,")
    if comma >= limit // 3:
        LOGGER.info("reply truncated at comma (%d -> %d chars)", len(text), comma + 1)
        return head[:comma].rstrip("，,") + "。"

    # 连逗号都没有:只能硬切。切点前留一个字给收尾的句号,
    # 否则"截断后补句号"反而会超过上限。
    LOGGER.info("reply hard-truncated (%d -> %d chars)", len(text), limit)
    return text[: max(1, limit - 1)].rstrip("，,") + "。"


@dataclass
class Reply:
    text: str
    latency: float
    raw: str = ""
    error: str = ""


class Conversation:
    """Keeps a bounded rolling history so the toy remembers recent context."""

    def __init__(self, settings: LLMSettings) -> None:
        self.settings = settings
        self.history: list[dict] = []

    def reset(self) -> None:
        self.history.clear()

    def _remember(self, role: str, content: str) -> None:
        self.history.append({"role": role, "content": content})
        # history_turns counts exchanges; each is two messages.
        limit = max(2, self.settings.history_turns * 2)
        if len(self.history) > limit:
            del self.history[: len(self.history) - limit]

    def reply(self, user_text: str) -> Reply:
        key = self.settings.resolved_key()
        if not key:
            hint = (f"未设置 {self.settings.api_key_env};"
                    "把它放进环境变量或 config.local.toml")
            LOGGER.error(hint)
            return Reply(text="", latency=0.0, error=hint)

        messages = [{"role": "system", "content": self.settings.system_prompt}]
        messages += self.history
        messages.append({"role": "user", "content": user_text})

        body = json.dumps({
            "model": self.settings.model,
            "messages": messages,
            "max_tokens": self.settings.max_tokens,
            "temperature": self.settings.temperature,
            "stream": False,
        }).encode("utf-8")

        request = urllib.request.Request(
            f"{self.settings.base_url.rstrip('/')}/chat/completions",
            data=body,
            headers={
                "Content-Type": "application/json",
                "Authorization": f"Bearer {key}",
            },
            method="POST",
        )

        started = time.monotonic()
        try:
            with urllib.request.urlopen(request, timeout=30) as response:
                payload = json.loads(response.read().decode("utf-8"))
        except urllib.error.HTTPError as error:
            detail = error.read().decode("utf-8", "replace")[:200]
            LOGGER.error("LLM HTTP %s: %s", error.code, detail)
            return Reply(text="", latency=time.monotonic() - started,
                         error=f"HTTP {error.code}")
        except (urllib.error.URLError, TimeoutError, OSError) as error:
            LOGGER.error("LLM request failed: %s", error)
            return Reply(text="", latency=time.monotonic() - started, error=str(error))

        latency = time.monotonic() - started
        try:
            raw = payload["choices"][0]["message"]["content"].strip()
        except (KeyError, IndexError, TypeError):
            LOGGER.error("unexpected LLM payload: %s", str(payload)[:200])
            return Reply(text="", latency=latency, error="bad payload")

        text = sanitize_for_speech(raw)
        text = _shorten(text, self.settings.max_reply_chars)
        if raw != text:
            LOGGER.info("sanitised for speech: %r -> %r", raw, text)

        if text:
            self._remember("user", user_text)
            self._remember("assistant", text)
        LOGGER.info("LLM %.2f s -> %r", latency, text)
        return Reply(text=text, latency=latency, raw=raw)
