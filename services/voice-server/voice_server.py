"""Stage 0 voice server: capture one utterance and echo it straight back.

This is deliberately the *dumbest possible* server. It exists so that the
whole audio path can be validated on its own, before any AI is involved:

    mic -> I2S -> device buffer -> TCP -> this script -> TCP -> speaker

Every stage-0 bug is therefore an audio/transport bug, never an STT/LLM/TTS
bug. Later stages replace ``echo_utterance`` with STT -> LLM -> TTS while
keeping the transport and the framing byte-for-byte identical.

Protocol (tiny, line for control + length-prefixed frames for PCM):

    client -> server   HELLO 1 <rate> <bits> <ch>\\n
    server -> client   OK\\n
    client -> server   REC\\n
                       [u16 big-endian length][pcm] ... [u16 0]
    server -> client   PLAY <total_bytes>\\n
                       [u16 length][pcm] ... [u16 0]

Run:
    python tools/voice_server.py --host 0.0.0.0 --port 8090
"""

from __future__ import annotations

import argparse
import logging
import socket
import socketserver
import struct
import time
import wave
from pathlib import Path

from stt import Transcriber
from config import load_settings
from fillers import FillerCache
from llm import Conversation
from tts import Speaker

CHUNK = 640  # 20 ms of 16 kHz mono 16-bit, matches the firmware default


def recv_exact(conn, count: int) -> bytes:
    """Read exactly ``count`` bytes or raise ConnectionError."""
    buffer = bytearray()
    while len(buffer) < count:
        block = conn.recv(count - len(buffer))
        if not block:
            raise ConnectionError(f"connection closed after {len(buffer)}/{count} bytes")
        buffer += block
    return bytes(buffer)


def read_line(conn) -> str:
    """Read one \\n-terminated ASCII control line (without the newline)."""
    buffer = bytearray()
    while True:
        char = conn.recv(1)
        if not char:
            raise ConnectionError("connection closed while reading a control line")
        if char == b"\n":
            return buffer.decode("ascii", "replace")
        if len(buffer) > 512:
            raise ValueError("control line too long")
        buffer += char


def send_frame(conn, payload: bytes) -> None:
    conn.sendall(struct.pack(">H", len(payload)) + payload)


def send_text(conn, text: str) -> None:
    """Push a display line to the device.

    The text is UTF-8 and terminated by a newline. It must not itself contain a
    newline, so anything embedded is turned into a space — otherwise the device
    would treat the tail as a bogus control line.
    """
    flat = " ".join(text.split())
    if not flat:
        return
    conn.sendall(f"TXT {flat}\n".encode("utf-8"))


def enable_utf8_console() -> None:
    """Make Chinese reach the Windows console instead of turning into '?'.

    The console defaults to a legacy code page, so printing the transcript
    shows question marks and makes debugging the STT result impossible.
    """
    try:
        import sys

        sys.stdout.reconfigure(encoding="utf-8", errors="replace")
        sys.stderr.reconfigure(encoding="utf-8", errors="replace")
    except (AttributeError, OSError):
        pass


def recv_frames(conn) -> bytes:
    """Collect length-prefixed frames until the zero-length end marker."""
    audio = bytearray()
    while True:
        (length,) = struct.unpack(">H", recv_exact(conn, 2))
        if length == 0:
            return bytes(audio)
        audio += recv_exact(conn, length)


def save_wav(path: Path, pcm: bytes, rate: int, channels: int) -> None:
    with wave.open(str(path), "wb") as handle:
        handle.setnchannels(channels)
        handle.setsampwidth(2)
        handle.setframerate(rate)
        handle.writeframes(pcm)


class VoiceHandler(socketserver.BaseRequestHandler):
    def handle(self) -> None:
        peer = f"{self.client_address[0]}:{self.client_address[1]}"
        print(f"[{peer}] connected", flush=True)
        rate, bits, channels = 16000, 16, 1
        utterance_index = 0

        try:
            while True:
                line = read_line(self.request)
                if not line:
                    continue

                if line.startswith("HELLO"):
                    parts = line.split()
                    if len(parts) >= 5:
                        rate, bits, channels = int(parts[2]), int(parts[3]), int(parts[4])
                    print(f"[{peer}] HELLO rate={rate} bits={bits} ch={channels}", flush=True)
                    self.request.sendall(b"OK\n")

                elif line.startswith("REC"):
                    started = time.monotonic()
                    pcm = recv_frames(self.request)
                    elapsed = time.monotonic() - started
                    seconds = len(pcm) / (2 * channels * rate) if pcm else 0.0
                    print(
                        f"[{peer}] REC {len(pcm)} B = {seconds:.2f} s audio "
                        f"(arrived in {elapsed:.2f} s)",
                        flush=True,
                    )

                    if pcm:
                        utterance_index += 1
                        out = Path(__file__).resolve().parent / "last_utterance.wav"
                        save_wav(out, pcm, rate, channels)

                        # ---- STT -> LLM -> per-sentence TTS streaming ----
                        transcript = self.server.transcriber.transcribe(pcm, rate)
                        if transcript.text:
                            print(f"[{peer}] STT ({transcript.latency:.2f} s): "
                                  f"{transcript.text}", flush=True)
                            send_text(self.request, transcript.text)
                            # PLAY 头由 stream_reply 在所有文本发完后发出:
                            # 它会切换设备进入二进制帧模式,必须是最后一个文本字节。
                            sent = self.server.stream_reply(self.request, transcript.text)
                            print(f"[{peer}] streamed {sent} B", flush=True)
                        else:
                            print(f"[{peer}] STT: (no speech detected)", flush=True)
                            self.request.sendall(b"PLAY 0\n")

                        # 结束标记:不论是流式还是空回复,都用它收尾。
                        self.request.sendall(struct.pack(">H", 0))
                    else:
                        self.request.sendall(b"PLAY 0\n")
                        self.request.sendall(struct.pack(">H", 0))

                else:
                    print(f"[{peer}] unknown line: {line!r}", flush=True)

        except (ConnectionError, ValueError) as error:
            print(f"[{peer}] closed: {error}", flush=True)
        except OSError as error:
            print(f"[{peer}] socket error: {error}", flush=True)


class Server(socketserver.ThreadingTCPServer):
    allow_reuse_address = True
    daemon_threads = True
    # 语音要的是低延迟而不是高吞吐。必须用常量,Windows 上 SOL_SOCKET 是 0xFFFF,
    # 硬编码 (1, 1, 1) 会直接抛 WinError 10022 把连接打断。
    socket_options = [(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)]

    def __init__(self, address, handler, transcriber: Transcriber, settings) -> None:
        self.transcriber = transcriber
        self.settings = settings
        self.conversation = Conversation(settings.llm)
        self.speaker = Speaker(settings.tts)
        # 填充语走同一条 TTS,只是缓存起来,所以几乎瞬间可播。
        self.fillers = FillerCache(lambda phrase: self.speaker.synthesize(phrase).pcm)
        super().__init__(address, handler)

    def _send_audio(self, conn, pcm: bytes) -> int:
        """Push PCM as 20 ms frames so the device can start playing immediately."""
        for offset in range(0, len(pcm), CHUNK):
            send_frame(conn, pcm[offset:offset + CHUNK])
        return len(pcm)

    def stream_reply(self, conn, user_text: str) -> int:
        """Produce the reply audio, streaming sentence by sentence.

        Ordering rule that must not be broken: the PLAY header switches the
        device into binary-frame mode, so it must be the LAST text byte on the
        wire. This method therefore sends the LLM reply text first and only then
        asks the caller to announce PLAY via ``on_audio_start`` - if PLAY went
        out before the text, the device would read "TX" as a frame length
        (0x5458 = 21592) and tear the link down.

        Returns the number of audio bytes pushed. Every failure degrades to
        "less audio" instead of a broken link, so the device always gets its
        terminating frame and returns to listening.
        """
        total = 0
        started = time.monotonic()

        # 1) 先把 LLM 回复作为文本发出去（早于 PLAY 头）。
        reply_text = ""
        if self.settings.server.enable_llm:
            result = self.conversation.reply(user_text)
            if result.error:
                print(f"  LLM failed: {result.error}", flush=True)
            else:
                reply_text = result.text
                print(f"  LLM ({result.latency:.2f} s): {reply_text}", flush=True)
                send_text(conn, reply_text)

        # 2) 到这里所有文本都发完了，可以切换成二进制帧模式。
        conn.sendall(b"PLAY 0\n")

        # 3) 播填充语,盖住 TTS 生成回答的等待时间。
        if self.settings.tts.enable_fillers:
            filler = self.fillers.pick(user_text)
            if filler:
                total += self._send_audio(conn, filler)
                print(f"  filler {len(filler)} B at {time.monotonic() - started:.2f} s",
                      flush=True)

        # 4) 真正的回答,逐句流式。
        if not reply_text or not self.settings.server.enable_tts:
            return total

        for chunk in self.speaker.stream(reply_text):
            total += self._send_audio(conn, chunk)
            print(f"  streamed {total} B after {time.monotonic() - started:.2f} s",
                  flush=True)
        if total == 0:
            print("  TTS produced no audio", flush=True)
        return total


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", default="0.0.0.0")
    parser.add_argument("--port", type=int, default=8090)
    parser.add_argument("--model", default="large-v3",
                        help="faster-whisper model name or local path")
    parser.add_argument("--device", default="cpu", choices=("cuda", "cpu"),
                        help="cpu is the safe default: the GPU may be busy with "
                             "another job (e.g. ComfyUI)")
    parser.add_argument("--compute-type", default=None,
                        help="defaults to float16 on cuda and int8 on cpu")
    parser.add_argument("--language", default="zh")
    parser.add_argument("--preload", action="store_true",
                        help="load the model before accepting connections")
    parser.add_argument("--warm-fillers", action="store_true",
                        help="pre-synthesise filler phrases at startup so the "
                             "first reply has no extra delay")
    parser.add_argument("--log-file", default=None,
                        help="write logs here (useful when stdout buffering is "
                             "unreliable under a detached launcher)")
    args = parser.parse_args()

    handlers = [logging.StreamHandler()]
    if args.log_file:
        handlers.append(logging.FileHandler(args.log_file, encoding="utf-8"))
    logging.basicConfig(level=logging.INFO, format="%(asctime)s %(levelname)s %(message)s",
                        handlers=handlers, force=True)
    enable_utf8_console()

    settings = load_settings()
    compute_type = args.compute_type or ("float16" if args.device == "cuda" else "int8")
    transcriber = Transcriber(model_name=args.model, device=args.device,
                              compute_type=compute_type, language=args.language)
    if args.preload:
        transcriber.load()
        settings.transcriber_preloaded = True

    with Server((args.host, args.port), VoiceHandler, transcriber, settings) as server:
        stages = "STT"
        if settings.server.enable_llm:
            stages += " + LLM"
        if settings.server.enable_tts:
            stages += " + TTS"
            if settings.tts.enable_fillers:
                stages += " + fillers"
        print(f"voice server ({stages}) on {args.host}:{args.port}", flush=True)
        print(f"model {args.model} on {args.device} ({compute_type}), "
              f"language {args.language}", flush=True)
        if settings.server.enable_llm:
            print(f"llm  {settings.llm.model} @ {settings.llm.base_url} "
                  f"(key: {'set' if settings.llm.resolved_key() else 'MISSING'})",
                  flush=True)
        if settings.server.enable_tts:
            print(f"tts  {settings.tts.voice} @ {settings.tts.base_url} "
                  f"({settings.tts.source_rate} -> {settings.tts.target_rate} Hz)",
                  flush=True)
            if settings.tts.enable_fillers and args.warm_fillers:
                # 首句对话不该为填充语再等一次合成。
                started = time.monotonic()
                server.fillers.preload()
                print(f"fillers preloaded in {time.monotonic() - started:.1f} s",
                      flush=True)
        if not transcriber.loaded:
            print("model loads lazily on the first utterance", flush=True)
        server.serve_forever()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
