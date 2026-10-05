"""Usage aggregation service — serves the compact text protocol the device reads.

The device is deliberately dumb: it renders whatever this service sends and
does no arithmetic. So all the platform-specific knowledge lives here, behind a
single endpoint:

    GET /api/usage.txt

Protocol (see firmware/usage-monitor/main/usage_model.h for the parser):

    PLATFORMS 2
    PLATFORM opencode-go OpenCode Go
    WINDOW 滚动 65 2520 1.3M / 2M tokens
    WINDOW 每周 30 259200 12M / 40M tokens
    PLATFORM chatgpt-plus ChatGPT Plus
    WINDOW 今日 40 3600 40 / 100 条
    END

Why not JSON: the device has ~40 KB of heap and JSON needs a parser plus a
parse tree. Line text with sscanf costs almost nothing.

Right now every source is a *stub* that returns plausible numbers, so the
firmware and the UI can be built and tested before any real data source is
wired up. Adding a real source means writing one class with a `fetch()` method
and registering it in SOURCES — the protocol, the firmware and the page do not
change.

Run:
    python usage_server.py --host 0.0.0.0 --port 8091
Then point the firmware at it:
    idf.py menuconfig  ->  USAGE_SERVER_PORT = 8091
"""

from __future__ import annotations

import argparse
import datetime
import random
import time
from dataclasses import dataclass, field
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

# --------------------------------------------------------------------------
# Data model (what a source produces)
# --------------------------------------------------------------------------


@dataclass
class Window:
    """One quota window: a rolling 5h limit, a weekly limit, and so on."""

    label: str
    used_percent: int
    resets_in_seconds: int = 0
    detail: str = ""


@dataclass
class Platform:
    id: str
    name: str
    windows: list[Window] = field(default_factory=list)


# --------------------------------------------------------------------------
# Sources
# --------------------------------------------------------------------------


class Source:
    """A place usage can be read from.

    `fetch()` returns None when the platform is unavailable — the service then
    omits it rather than sending zeroes, so the device can tell "no data" from
    "nothing used".
    """

    id = ""
    name = ""

    def fetch(self) -> Platform | None:
        raise NotImplementedError


class StubSource(Source):
    """Plausible-looking fake data.

    Numbers drift slowly so a screen left running visibly updates, and the
    reset countdowns decrease in real time — that exercises the device's
    countdown formatting and its refresh path.
    """

    def __init__(self, platform_id: str, name: str, windows: list[tuple[str, int, int]],
                 detail: str) -> None:
        self.id = platform_id
        self.name = name
        # (label, baseline percent, window length in seconds)
        self._windows = windows
        self._detail = detail
        self._started = time.monotonic()

    def fetch(self) -> Platform:
        elapsed = time.monotonic() - self._started
        windows = []
        for label, baseline, length in self._windows:
            # 缓慢漂移,便于肉眼确认界面在刷新。
            drift = (elapsed / 60.0) * 0.7
            percent = int(min(99, baseline + drift))
            # 距重置时间随真实时间递减,归零后重新开始。
            remaining = length - int(elapsed) % length
            windows.append(Window(label=label, used_percent=percent,
                                  resets_in_seconds=remaining,
                                  detail=self._detail))
        return Platform(id=self.id, name=self.name, windows=windows)


class RandomSource(Source):
    """A platform whose numbers jump around, for exercising the display."""

    def __init__(self, platform_id: str, name: str, labels: list[str]) -> None:
        self.id = platform_id
        self.name = name
        self._labels = labels

    def fetch(self) -> Platform:
        windows = []
        for label in self._labels:
            percent = random.randint(0, 100)
            windows.append(Window(label=label, used_percent=percent,
                                  resets_in_seconds=random.choice([0, 900, 7200, 86400]),
                                  detail=f"{percent} / 100 次"))
        return Platform(id=self.id, name=self.name, windows=windows)


# 在这里注册数据源。接真实数据时:写一个 Source 子类,替换掉对应的 StubSource。
SOURCES: list[Source] = [
    StubSource("opencode-go", "OpenCode Go",
               [("滚动", 65, 5 * 3600), ("每周", 30, 7 * 86400),
                ("每月", 12, 30 * 86400)],
               "1.3M / 2M tokens"),
    StubSource("chatgpt-plus", "ChatGPT Plus",
               [("5小时", 42, 5 * 3600), ("每周", 78, 7 * 86400)],
               "40 / 100 条消息"),
    RandomSource("claude-max", "Claude Max", ["5小时", "每周"]),
]


# --------------------------------------------------------------------------
# Rendering
# --------------------------------------------------------------------------


def render(sources: list[Source]) -> str:
    platforms = []
    for source in sources:
        try:
            platform = source.fetch()
        except Exception as error:  # noqa: BLE001
            # 一个源坏掉不该让整页失败:跳过它,其余照常显示。
            print(f"  {source.id}: fetch failed: {error}", flush=True)
            continue
        if platform is not None and platform.windows:
            platforms.append(platform)

    lines = [f"PLATFORMS {len(platforms)}"]
    for platform in platforms:
        # 名称可以含空格:设备把这一行的剩余部分整体当名字。
        lines.append(f"PLATFORM {platform.id} {platform.name}")
        for window in platform.windows:
            percent = max(0, min(100, int(window.used_percent)))
            resets = max(0, int(window.resets_in_seconds))
            # 明细同样取整行剩余,所以也可以含空格。
            lines.append(f"WINDOW {window.label} {percent} {resets} {window.detail}")
    lines.append("END")
    return "\n".join(lines) + "\n"


# --------------------------------------------------------------------------
# HTTP
# --------------------------------------------------------------------------


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def do_GET(self) -> None:  # noqa: N802 (BaseHTTPRequestHandler API)
        if self.path.startswith("/api/usage.txt"):
            body = render(SOURCES).encode("utf-8")
            self.send_response(200)
            self.send_header("Content-Type", "text/plain; charset=utf-8")
            self.send_header("Content-Length", str(len(body)))
            # 设备按固定间隔来拉,不需要任何缓存。
            self.send_header("Cache-Control", "no-store")
            self.end_headers()
            self.wfile.write(body)
            print(f"{self.client_address[0]} -> {len(body)} B", flush=True)

        elif self.path.startswith("/api/usage.json"):
            # 给浏览器/调试用。设备不用它(JSON 对 C3 太重)。
            import json

            payload = []
            for source in SOURCES:
                platform = source.fetch()
                if platform is None:
                    continue
                payload.append({
                    "id": platform.id,
                    "name": platform.name,
                    "windows": [
                        {"label": w.label, "used_percent": w.used_percent,
                         "resets_in_seconds": w.resets_in_seconds,
                         "detail": w.detail}
                        for w in platform.windows
                    ],
                })
            body = json.dumps({
                "generated_at": datetime.datetime.now(datetime.timezone.utc)
                                .replace(microsecond=0).isoformat(),
                "platforms": payload,
            }, indent=2, ensure_ascii=False).encode("utf-8")
            self.send_response(200)
            self.send_header("Content-Type", "application/json; charset=utf-8")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)

        elif self.path in ("/", "/api/usage"):
            # 浏览器里直接看文本版,方便对着调界面。
            body = render(SOURCES).encode("utf-8")
            self.send_response(200)
            self.send_header("Content-Type", "text/plain; charset=utf-8")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)

        else:
            self.send_error(404)

    def log_message(self, *args) -> None:
        """Silence the default per-request log; we print our own line."""


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", default="0.0.0.0")
    parser.add_argument("--port", type=int, default=8091)
    args = parser.parse_args()

    server = ThreadingHTTPServer((args.host, args.port), Handler)
    print(f"usage server on {args.host}:{args.port}", flush=True)
    print("  /api/usage.txt   设备读的紧凑文本", flush=True)
    print("  /api/usage.json  浏览器/调试用", flush=True)
    print(f"  当前数据源: {', '.join(s.id for s in SOURCES)} (全部为假数据)",
          flush=True)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
