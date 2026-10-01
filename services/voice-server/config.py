"""Configuration for the voice server.

Secrets never live in this file: ``api_key`` reads from an environment variable
by default, and ``config.local.toml`` (gitignored) is the place to put machine
specific overrides.
"""

from __future__ import annotations

import os
from dataclasses import dataclass, field
from pathlib import Path

try:
    import tomllib  # Python 3.11+
except ModuleNotFoundError:  # 3.10 and earlier
    import tomli as tomllib  # type: ignore[no-redef]

HERE = Path(__file__).resolve().parent


@dataclass
class LLMSettings:
    base_url: str = "https://api.deepseek.com/v1"
    model: str = "deepseek-chat"
    # Key 优先从配置文件读(api_key)。api_key_env 只是兜底:当 api_key 为空
    # 时才去看环境变量,方便在 CI 或临时会话里覆盖。
    api_key: str = ""
    api_key_env: str = "DEEPSEEK_API_KEY"
    # 这个提示词是给"被念出来"服务的,不是给人读的。改动时请守住三条:
    #   1. 短 —— 超过两句话,用户要干等十几秒才听完;
    #   2. 纯口语 —— 任何 TTS 念不出来或念得怪的东西都禁止(列表/符号/emoji/括号);
    #   3. 无寒暄 —— 每次回复都重复"好的""让我想想"会让对话变得很吵。
    system_prompt: str = (
        "你是一个语音助理。你和用户的全部交流都通过语音进行。\n"
        "\n"
        "最重要的一条：每次回答控制在四十个字以内。\n"
        "回答会被念出来，四十个字大约要说六到八秒，再多用户就要等得不耐烦。\n"
        "先把最重要的说清楚；用户想知道更多，会接着问。\n"
        "\n"
        "硬性要求：\n"
        "1. 只输出要念出来的内容，不要有旁白、动作描写、括号说明或舞台提示。\n"
        "2. 直接给答案，不要复述用户的问题，不要开场白，不要结束语。\n"
        "3. 没有把握的事实就说不知道，不要编造，也不要长篇解释为什么不知道。\n"
        "4. 记住对话里出现过的信息（名字、偏好、刚才聊的事）并在合适时使用。\n"
        "\n"
        "口语化要求（内容会被朗读，必须遵守）：\n"
        "- 一般一到三句话；需要列举时最多说三项，不要写成清单。\n"
        "- 不使用任何符号：没有星号、井号、短横线列表、引号、括号、表情符号。\n"
        "- 数字、时间、单位写成中文口语形式：说“三点一四”而不是“3.14”，"
        "说“下午三点半”而不是“15:30”，说“百分之二十”而不是“20%”。\n"
        "- 英文缩写能用中文说就用中文，例如说“人工智能”而不是“AI”。\n"
        "- 句子里用逗号和句号来控制停顿，不要用分号、冒号、破折号。\n"
        "\n"
        "示例：\n"
        "用户：现在几点了？\n"
        "回答：我看不到表，抱歉。\n"
        "用户：帮我记一下明天要买牛奶。\n"
        "回答：好的，明天买牛奶。\n"
        "用户：介绍一下量子力学。\n"
        "回答：它研究微观粒子的规律，比如电子既像粒子又像波，"
        "还有测不准和纠缠这些现象。想先听哪个？\n"
        "用户：今天天气怎么样？\n"
        "回答：我查不到实时天气，你在哪个城市？"
    )
    max_tokens: int = 240
    temperature: float = 0.4
    history_turns: int = 6       # how many past exchanges to keep as context
    # 回答的字数上限。TTS 大约每字 0.25 秒,所以 48 字 ≈ 12 秒音频。
    # 这是提示词之外的硬约束:模型偶尔会无视字数要求。
    max_reply_chars: int = 48

    def resolved_key(self) -> str:
        """Config file wins; the environment variable is only a fallback."""
        return self.api_key.strip() or os.environ.get(self.api_key_env, "").strip()


@dataclass
class TTSSettings:
    base_url: str = "http://127.0.0.1:9880"
    model: str = "tts-1"
    voice: str = "voice_01"
    # 与 LLM 同样的优先级:配置文件优先,环境变量兜底。
    api_key: str = ""
    api_key_env: str = "INDEXTTS_OPENAI_BEARER_TOKEN"
    timeout_s: float = 60.0
    # Device-side playback rate.
    target_rate: int = 16000
    # 服务端合成用的源采样率。原始 PCM 没有采样率头,必须知道源速率才能
    # 正确重采样 —— 猜错会让播放变调(22050 当成 16000 → 语速快 38%、音调高)。
    # IndexTTS 输出 22050;换别的 TTS 时按它文档改。
    source_rate: int = 22050
    # 预合成的填充语,用来盖住 IndexTTS 生成回答的等待时间。
    enable_fillers: bool = True
    # IndexTTS 的输出峰值只有满量程的 ~19%,直接送喇叭就是"声音很小"。
    # 这里做峰值归一化,把音量提上来(实测能白赚约 13 dB),同时留出余量
    # 避免削波失真。设为 0 关闭增益。
    target_peak: float = 0.90

    def resolved_key(self) -> str:
        """Config file wins; the environment variable is only a fallback."""
        return self.api_key.strip() or os.environ.get(self.api_key_env, "").strip()


@dataclass
class ServerSettings:
    host: str = "0.0.0.0"
    port: int = 8090
    # Stage 1 = STT only, stage 2 = +LLM, stage 3 = +TTS.
    enable_llm: bool = True
    enable_tts: bool = True
    # Keep the mirror reply available for isolated audio testing.
    echo_fallback: bool = False


@dataclass
class Settings:
    llm: LLMSettings = field(default_factory=LLMSettings)
    tts: TTSSettings = field(default_factory=TTSSettings)
    server: ServerSettings = field(default_factory=ServerSettings)


def _apply(section: object, values: dict) -> None:
    for key, value in values.items():
        if hasattr(section, key):
            setattr(section, key, value)


def load_settings(path: Path | None = None) -> Settings:
    settings = Settings()
    for candidate in (path, HERE / "config.local.toml", HERE / "config.toml"):
        if candidate is None or not candidate.is_file():
            continue
        try:
            with candidate.open("rb") as handle:
                data = tomllib.load(handle)
        except (OSError, tomllib.TOMLDecodeError):
            continue
        for name, section in (("llm", settings.llm), ("tts", settings.tts),
                              ("server", settings.server)):
            if isinstance(data.get(name), dict):
                _apply(section, data[name])
    return settings
