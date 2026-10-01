"""Generate the demo scene art and a character sprite.

The artwork is generated procedurally so the repository carries no third-party
image assets. Drop real PNG/JPEG files with the same names into
``content/<game>/scenes/`` or ``content/<game>/sprites/`` to replace them.

Usage:
    python tools/make_sample_art.py
"""

from __future__ import annotations

from pathlib import Path

from PIL import Image, ImageDraw, ImageFilter

ROOT = Path(__file__).resolve().parents[1]
SCENES = ROOT / "content" / "demo" / "scenes"
SPRITES = ROOT / "content" / "demo" / "sprites"
WIDTH, HEIGHT = 240, 320


def vertical_gradient(top: tuple[int, int, int], bottom: tuple[int, int, int]) -> Image.Image:
    image = Image.new("RGB", (WIDTH, HEIGHT))
    draw = ImageDraw.Draw(image)
    for y in range(HEIGHT):
        t = y / (HEIGHT - 1)
        color = tuple(int(top[i] + (bottom[i] - top[i]) * t) for i in range(3))
        draw.line((0, y, WIDTH, y), fill=color)
    return image


def vignette(image: Image.Image, strength: int = 110) -> Image.Image:
    mask = Image.new("L", (WIDTH, HEIGHT), 0)
    draw = ImageDraw.Draw(mask)
    draw.ellipse((-WIDTH * 0.25, -HEIGHT * 0.15, WIDTH * 1.25, HEIGHT * 1.15), fill=255)
    mask = mask.filter(ImageFilter.GaussianBlur(28))
    dark = Image.new("RGB", (WIDTH, HEIGHT), (0, 0, 0))
    return Image.composite(image, dark, mask.point(lambda v: 255 - (255 - v) * strength // 255))


def make_bg_hall() -> Image.Image:
    image = vertical_gradient((38, 42, 74), (10, 12, 24))
    draw = ImageDraw.Draw(image)
    # Three lit exhibit panels.
    for x, w, glow in ((26, 52, (214, 178, 96)), (94, 52, (150, 196, 214)), (162, 52, (206, 140, 150))):
        draw.rectangle((x, 54, x + w, 122), outline=glow, width=2)
        for step in range(6, 0, -1):
            draw.rectangle((x + step, 54 + step, x + w - step, 122 - step), outline=None,
                           fill=tuple(int(c * step / 9) for c in glow))
    # Floor line.
    draw.line((0, 176, WIDTH, 176), fill=(58, 60, 84), width=2)
    return vignette(image)


def make_bg_watch() -> Image.Image:
    image = vertical_gradient((64, 44, 28), (16, 10, 8))
    draw = ImageDraw.Draw(image)
    # A glowing pocket watch.
    cx, cy, r = WIDTH // 2, 96, 54
    for step in range(28, 0, -1):
        shade = int(210 * step / 28)
        draw.ellipse((cx - r - step, cy - r - step, cx + r + step, cy + r + step),
                     fill=(shade, int(shade * 0.78), int(shade * 0.36)))
    draw.ellipse((cx - r, cy - r, cx + r, cy + r), fill=(28, 24, 20), outline=(232, 200, 120), width=3)
    draw.line((cx, cy, cx, cy - r + 16), fill=(240, 214, 140), width=3)
    draw.line((cx, cy, cx + r - 26, cy + 12), fill=(240, 214, 140), width=3)
    return vignette(image)


def make_hero() -> Image.Image:
    """A simple character silhouette with an alpha channel."""
    w, h = 104, 168
    sprite = Image.new("RGBA", (w, h), (0, 0, 0, 0))
    draw = ImageDraw.Draw(sprite)
    skin = (232, 198, 170, 255)
    coat = (44, 58, 96, 255)
    coat_dark = (30, 40, 70, 255)
    hair = (36, 30, 34, 255)
    # Body / coat.
    draw.polygon([(22, h), (34, 74), (70, 74), (82, h)], fill=coat)
    draw.polygon([(50, 74), (56, 74), (66, h), (40, h)], fill=coat_dark)
    # Head.
    draw.ellipse((30, 24, 74, 78), fill=skin)
    draw.chord((28, 16, 76, 62), 180, 360, fill=hair)
    # Eyes.
    draw.ellipse((43, 48, 49, 55), fill=(28, 24, 28, 255))
    draw.ellipse((57, 48, 63, 55), fill=(28, 24, 28, 255))
    # Arms.
    draw.polygon([(20, h), (28, 86), (38, 88), (34, h)], fill=coat_dark)
    draw.polygon([(84, h), (78, 86), (68, 88), (72, h)], fill=coat_dark)
    return sprite


def main() -> None:
    SCENES.mkdir(parents=True, exist_ok=True)
    SPRITES.mkdir(parents=True, exist_ok=True)
    make_bg_hall().save(SCENES / "bg_hall.png")
    make_bg_watch().save(SCENES / "bg_watch.png")
    make_hero().save(SPRITES / "hero.png")
    print("wrote", SCENES / "bg_hall.png")
    print("wrote", SCENES / "bg_watch.png")
    print("wrote", SPRITES / "hero.png")


if __name__ == "__main__":
    main()
