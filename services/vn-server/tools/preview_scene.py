"""Dev helper: render a composed scene frame to PNG for a quick eyeball check.

Usage:
    python tools/preview_scene.py <scene> [sprite] [out.png]
"""

import struct
import sys
import zlib
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))
from vn_server import scenes  # noqa: E402

scene = sys.argv[1] if len(sys.argv) > 1 else "bg_hall"
sprite = sys.argv[2] if len(sys.argv) > 2 else ""
out = Path(sys.argv[3]) if len(sys.argv) > 3 else ROOT / "tools" / f"{scene}_preview.png"

W, H = scenes.WIDTH, scenes.HEIGHT
frame = scenes.strip(scene, 0, H, sprite)
values = [frame[i] << 8 | frame[i + 1] for i in range(0, len(frame), 2)]

rows = []
for y in range(H):
    row = bytearray()
    for x in range(W):
        v = values[y * W + x]
        row += bytes(
            (
                ((v >> 11) & 0x1F) * 255 // 31,
                ((v >> 5) & 0x3F) * 255 // 63,
                (v & 0x1F) * 255 // 31,
            )
        )
    rows.append(bytes(row))
raw = b"".join(b"\x00" + r for r in rows)


def chunk(tag: bytes, data: bytes) -> bytes:
    return (
        struct.pack(">I", len(data))
        + tag
        + data
        + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF)
    )


png = b"\x89PNG\r\n\x1a\n"
png += chunk(b"IHDR", struct.pack(">IIBBBBB", W, H, 8, 2, 0, 0, 0))
png += chunk(b"IDAT", zlib.compress(raw, 9))
png += chunk(b"IEND", b"")
out.write_bytes(png)
print(f"wrote {out} ({len(png)} bytes) scene={scene} sprite={sprite or '-'}")
