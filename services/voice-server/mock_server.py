"""Mock voice server for UI and interaction testing.

Exists so the device UI can be exercised without loading IndexTTS (which costs
~6 GB of VRAM and a minute of startup) or spending LLM tokens. It speaks the
exact same protocol as the real server, so the firmware cannot tell them apart.

What it does:
    REC  -> a canned transcript is echoed back as TXT
    -> a canned reply is sent as a second TXT
    -> PLAY, then a short beep-like tone instead of speech

Use it to check layout, message bubbles, scrolling, battery, the settings page
and the button feel. Not for judging audio quality or real latency.
"""

from __future__ import annotations

import argparse
import math
import random
import socketserver
import struct
import time

CHUNK = 640          # 20 ms at 16 kHz mono
RATE = 16000

# 假识别结果与假回复。故意长短不一,好看出气泡换行和滚动是否正常。
FAKE_PROMPTS = [
    "今天天气怎么样",
    "帮我记一下明天要买牛奶",
    "介绍一下量子力学",
    "你好",
    "杭州有什么好吃的",
    "讲个笑话吧",
]

FAKE_REPLIES = [
    "我查不到实时天气，你在哪个城市？",
    "好的，明天买牛奶。",
    "它研究微观粒子的规律，比如电子既像粒子又像波，还有测不准和纠缠这些现象。"
    "想先听哪个？",
    "你好，有什么我能帮你的吗？",
    "西湖醋鱼和龙井虾仁，想先听哪个？",
    "为什么鱼不上学？因为它们已经在学游泳了。",
]


def recv_exact(conn, count: int) -> bytes:
    buffer = bytearray()
    while len(buffer) < count:
        block = conn.recv(count - len(buffer))
        if not block:
            raise ConnectionError("peer closed")
        buffer += block
    return bytes(buffer)


def read_line(conn) -> str:
    buffer = bytearray()
    while True:
        char = recv_exact(conn, 1)
        if char == b"\n":
            return buffer.decode("ascii", "replace")
        buffer += char


def recv_frames(conn) -> bytes:
    audio = bytearray()
    while True:
        (length,) = struct.unpack(">H", recv_exact(conn, 2))
        if length == 0:
            return bytes(audio)
        audio += recv_exact(conn, length)


def send_frame(conn, payload: bytes) -> None:
    conn.sendall(struct.pack(">H", len(payload)) + payload)


def send_text(conn, text: str) -> None:
    flat = " ".join(text.split())
    if flat:
        conn.sendall(f"TXT {flat}\n".encode("utf-8"))


def tone(seconds: float, freq: float = 440.0) -> bytes:
    """A soft beep, so you can hear that audio is flowing without TTS."""
    count = int(RATE * seconds)
    fade = int(RATE * 0.02)
    samples = []
    for index in range(count):
        envelope = 1.0
        if index < fade:
            envelope = index / fade
        elif index > count - fade:
            envelope = (count - index) / fade
        value = 9000 * envelope * math.sin(2 * math.pi * freq * index / RATE)
        samples.append(int(value))
    return struct.pack(f"<{len(samples)}h", *samples)


class MockHandler(socketserver.BaseRequestHandler):
    def handle(self) -> None:
        peer = f"{self.client_address[0]}:{self.client_address[1]}"
        print(f"[{peer}] connected", flush=True)
        try:
            while True:
                line = read_line(self.request)
                if not line:
                    continue

                if line.startswith("HELLO"):
                    print(f"[{peer}] {line}", flush=True)
                    self.request.sendall(b"OK\n")

                elif line.startswith("REC"):
                    started = time.monotonic()
                    pcm = recv_frames(self.request)
                    seconds = len(pcm) / (2 * RATE) if pcm else 0.0
                    print(f"[{peer}] REC {len(pcm)} B = {seconds:.2f} s", flush=True)

                    if not pcm:
                        self.request.sendall(b"PLAY 0\n")
                        self.request.sendall(struct.pack(">H", 0))
                        continue

                    prompt = random.choice(FAKE_PROMPTS)
                    reply = random.choice(FAKE_REPLIES)

                    # 顺序必须与真服务器一致:先所有文本,再 PLAY,最后音频。
                    # PLAY 之后只能跟二进制帧,否则设备会把 "TX" 当帧长度。
                    send_text(self.request, prompt)
                    time.sleep(0.3)          # 假装在想
                    send_text(self.request, reply)
                    self.request.sendall(b"PLAY 0\n")

                    # 用与回复长度相称的音频,方便观察播放中的 UI 状态。
                    audio = tone(max(1.0, min(6.0, len(reply) * 0.25)))
                    for offset in range(0, len(audio), CHUNK):
                        send_frame(self.request, audio[offset:offset + CHUNK])
                    self.request.sendall(struct.pack(">H", 0))

                    print(f"[{peer}] replied {seconds:.1f}s in -> "
                          f"{len(audio) / (2 * RATE):.1f}s out "
                          f"({time.monotonic() - started:.2f} s)", flush=True)

                    if len(pcm) < RATE:      # 太短的一句当作"误触",提示一下
                        send_text(self.request, "刚才没听清，再说一次？")

                else:
                    print(f"[{peer}] unknown: {line!r}", flush=True)

        except (ConnectionError, ValueError) as error:
            print(f"[{peer}] closed: {error}", flush=True)


class Server(socketserver.ThreadingTCPServer):
    allow_reuse_address = True
    daemon_threads = True
    socket_options = []


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", default="0.0.0.0")
    parser.add_argument("--port", type=int, default=8090)
    args = parser.parse_args()

    with Server((args.host, args.port), MockHandler) as server:
        print(f"mock voice server on {args.host}:{args.port}", flush=True)
        print("UI 调试用:假识别、假回复、蜂鸣音。不加载模型、不花 token。", flush=True)
        server.serve_forever()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
