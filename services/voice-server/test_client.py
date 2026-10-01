"""End-to-end test against the voice server, mimicking the ESP32 byte for byte.

Speaks the real protocol (HELLO / REC + frames / TXT lines / PLAY + frames) so
latency numbers reflect what the device will actually experience.
"""

from __future__ import annotations

import socket
import struct
import sys
import time
import wave

HOST, PORT = "127.0.0.1", 8090
WAV = r"H:\Work\Personal\ESP32\Folotoy\ai-passport\voice-server\last_utterance.wav"


def recv_exact(sock, count: int) -> bytes:
    buffer = bytearray()
    while len(buffer) < count:
        block = sock.recv(count - len(buffer))
        if not block:
            raise ConnectionError("peer closed")
        buffer += block
    return bytes(buffer)


def read_line(sock) -> str:
    buffer = bytearray()
    while True:
        char = recv_exact(sock, 1)
        if char == b"\n":
            return buffer.decode("utf-8", "replace")
        buffer += char


def main() -> int:
    with wave.open(WAV, "rb") as handle:
        pcm = handle.readframes(handle.getnframes())
        rate = handle.getframerate()
    print(f"sending {len(pcm) / (2 * rate):.2f} s of real speech @ {rate} Hz")

    sock = socket.create_connection((HOST, PORT), timeout=300)
    # 连接超时不是读超时:服务端在句与句之间会有数秒静默(生成下一句),
    # 这里显式设置读超时,否则会误报 TimeoutError。
    sock.settimeout(120)
    sock.sendall(f"HELLO 1 {rate} 16 1\n".encode())
    print(f"handshake : {read_line(sock)}")

    started = time.monotonic()
    sock.sendall(b"REC\n")
    for offset in range(0, len(pcm), 320):
        frame = pcm[offset:offset + 320]
        sock.sendall(struct.pack(">H", len(frame)) + frame)
    sock.sendall(struct.pack(">H", 0))

    first_audio = None
    total = 0
    timeline: list[tuple[str, float, float]] = []

    # 服务器会穿插 TXT 和 PLAY,必须一直读控制行直到 PLAY 出现。
    while True:
        line = read_line(sock)
        if line.startswith("TXT "):
            print(f"  [{time.monotonic() - started:5.2f}s] device shows: {line[4:]}")
            continue
        if line.startswith("PLAY"):
            print(f"  [{time.monotonic() - started:5.2f}s] {line}")
            break
        if not line:
            continue
        print(f"  [{time.monotonic() - started:5.2f}s] unexpected: {line!r}")

    while True:
        (length,) = struct.unpack(">H", recv_exact(sock, 2))
        if length == 0:
            break
        recv_exact(sock, length)
        total += length
        now = time.monotonic() - started
        if first_audio is None:
            first_audio = now
            timeline.append(("first audio", now, total))
        elif total % (16000 * 2) < length:
            timeline.append(("~1s mark", now, total))

    seconds = total / (2 * rate)
    print()
    print("=== timeline ===")
    for name, at, size in timeline:
        print(f"  {name:<12} @{at:5.2f}s   {size:>7} B = {size / (2 * rate):5.2f} s audio")
    print(f"  {'total':<12}          {total:>7} B = {seconds:5.2f} s audio")
    print()
    print(f"USER WAITS FOR FIRST SOUND: {first_audio:.2f} s")
    print(f"TTS realtime factor       : {(time.monotonic() - started - first_audio) / seconds if seconds else 0:.2f}")
    sock.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
