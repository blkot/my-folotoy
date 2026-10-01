"""Scene frames (background + optional character sprite) streamed as RGB565.

The device has no image decoder and no PSRAM, so it never holds a full frame:
the server composites the scene and hands out rows on demand, and the device
blits each strip straight to the LCD.

Sources, in order of preference:

* real artwork under ``content/<game>/scenes/<id>.{png,jpg,jpeg}`` and
  ``content/<game>/sprites/<id>.png`` when Pillow is installed;
* otherwise a deterministic gradient for the background, and sprites are
  ignored.

Compositing happens here rather than on the device so the wire format stays
"plain RGB565 rows" and the firmware needs no alpha blending.
"""

from __future__ import annotations

import struct
from pathlib import Path
from typing import Any

WIDTH = 240
HEIGHT = 320
# Big-endian RGB565, matching what the ST7789 SPI path expects. The LVGL port
# byte swaps its own little-endian output, but a raw blit must already be
# swapped.
FORMAT = "rgb565be"
# The dialogue panel occupies the bottom of the screen; keep sprites above it.
SPRITE_BASELINE = 160

try:  # Pillow is optional: gradients keep the endpoints usable without it.
    from PIL import Image  # type: ignore

    _HAVE_PIL = True
except Exception:  # pragma: no cover - optional dependency
    Image = None  # type: ignore
    _HAVE_PIL = False


class SceneError(Exception):
    """Raised for unknown scenes or out-of-range strips."""


def _content_root() -> Path:
    return Path(__file__).resolve().parent.parent / "content"


def _find_asset(kind: str, asset_id: str) -> Path | None:
    if not asset_id:
        return None
    suffixes = (".png", ".jpg", ".jpeg") if kind == "scenes" else (".png",)
    root = _content_root()
    for game in sorted(p for p in root.iterdir() if p.is_dir()):
        for suffix in suffixes:
            candidate = game / kind / f"{asset_id}{suffix}"
            if candidate.is_file():
                return candidate
    return None


def info(scene_id: str, sprite_id: str = "") -> dict[str, Any]:
    sprite_path = _find_asset("sprites", sprite_id) if _HAVE_PIL else None
    return {
        "id": scene_id,
        "width": WIDTH,
        "height": HEIGHT,
        "format": FORMAT,
        "background": "image" if _find_asset("scenes", scene_id) and _HAVE_PIL else "gradient",
        "sprite": sprite_id or None,
        "sprite_source": "image" if sprite_path else None,
    }


def _palette(scene_id: str) -> tuple[tuple[int, int, int], tuple[int, int, int]]:
    seed = sum(scene_id.encode("utf-8"))
    top = (60 + (seed * 37) % 90, 70 + (seed * 53) % 80, 110 + (seed * 71) % 120)
    bottom = (8 + (seed * 17) % 40, 10 + (seed * 29) % 40, 18 + (seed * 41) % 60)
    return top, bottom


def _gradient_rgb(scene_id: str) -> bytes:
    top, bottom = _palette(scene_id)
    out = bytearray()
    for y in range(HEIGHT):
        t = y / (HEIGHT - 1)
        base = (
            int(top[0] + (bottom[0] - top[0]) * t),
            int(top[1] + (bottom[1] - top[1]) * t),
            int(top[2] + (bottom[2] - top[2]) * t),
        )
        for x in range(WIDTH):
            dx = (x - WIDTH / 2) / (WIDTH / 2)
            dy = (y - HEIGHT / 2) / (HEIGHT / 2)
            factor = 0.5 if dx * dx + dy * dy > 0.55 else 1.0
            out += bytes(
                (
                    int(base[0] * factor) & 0xFF,
                    int(base[1] * factor) & 0xFF,
                    int(base[2] * factor) & 0xFF,
                )
            )
    return bytes(out)


def _to_rgb565be(rgb: bytes) -> bytes:
    out = bytearray()
    for i in range(0, len(rgb), 3):
        r, g, b = rgb[i], rgb[i + 1], rgb[i + 2]
        out += struct.pack(">H", ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3))
    return bytes(out)


_cache: dict[tuple[str, str, int | None, int | None], bytes] = {}


def _compose_rgb(scene_id: str, sprite_id: str, sx: int | None, sy: int | None) -> bytes:
    background = _find_asset("scenes", scene_id) if _HAVE_PIL else None
    if _HAVE_PIL:
        if background is not None:
            with Image.open(background) as opened:  # type: ignore[union-attr]
                frame = opened.convert("RGB").resize((WIDTH, HEIGHT))
                frame.load()
        else:
            frame = Image.frombytes("RGB", (WIDTH, HEIGHT), _gradient_rgb(scene_id))  # type: ignore[union-attr]

        sprite = _find_asset("sprites", sprite_id) if sprite_id else None
        if sprite is not None:
            with Image.open(sprite) as opened:  # type: ignore[union-attr]
                layer = opened.convert("RGBA")
                layer.load()
            if layer.height > SPRITE_BASELINE:
                scale = SPRITE_BASELINE / layer.height
                layer = layer.resize((max(1, int(layer.width * scale)), SPRITE_BASELINE))
            px, py = _position_for(layer.size, sx, sy)
            frame.paste(layer, (px, py), layer)
        return frame.tobytes()

    return _gradient_rgb(scene_id)


def _position_for(size: tuple[int, int], sx: int | None, sy: int | None) -> tuple[int, int]:
    width, height = size
    if sx is None:
        sx = (WIDTH - width) // 2
    if sy is None:
        sy = SPRITE_BASELINE - height
    return sx, sy


def _frame(scene_id: str, sprite_id: str, sx: int | None, sy: int | None) -> bytes:
    key = (scene_id, sprite_id, sx, sy)
    cached = _cache.get(key)
    if cached is not None:
        return cached
    data = _to_rgb565be(_compose_rgb(scene_id, sprite_id, sx, sy))
    _cache[key] = data
    return data


def strip(
    scene_id: str,
    y: int,
    height: int,
    sprite_id: str = "",
    sx: int | None = None,
    sy: int | None = None,
) -> bytes:
    if height <= 0 or y < 0 or y + height > HEIGHT:
        raise SceneError(f"strip out of range: y={y} height={height}")
    frame = _frame(scene_id, sprite_id, sx, sy)
    return frame[y * WIDTH * 2 : (y + height) * WIDTH * 2]
