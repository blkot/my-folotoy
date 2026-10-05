#!/usr/bin/env python3
"""Generate the machine-readable firmware index published on gh-pages.

Why this exists: the web page reads a manifest per firmware, but phone-side
tooling (Termux scripts, a future app) needs one stable URL that lists every
firmware with enough metadata to flash it without guessing:

    offset 0 + sha256 + size   ->  esptool-compatible write_flash
    chip_family                ->  refuses to flash the wrong chip
    version / notes            ->  lets a human decide whether to update

Everything is derived from the published binaries, so the index cannot drift
from what is actually downloadable. Run from the release job after the
artifacts are downloaded into ./release.

Usage:
    python tools/make_registry.py release site
"""

from __future__ import annotations

import argparse
import datetime
import hashlib
import json
import os
import pathlib
import re

# 本仓库所有固件都是这个目标;写死在这里,避免每个 bin 都去解析 app_desc。
CHIP_FAMILY = "ESP32-C3"
TARGET = "esp32c3"
FLASH_SIZE_BYTES = 8 * 1024 * 1024

# 合并镜像从 0x0 整片写入(bootloader + 分区表 + app 都在里面)。
OFFSET = 0

DISPLAY_NAMES = {
    "voice-bot": "语音 bot",
    "visual-novel": "视觉小说",
}

DISPLAY_NOTES = {
    "voice-bot": "按住「确定」说话,识别、回答、朗读都在 PC 上完成。",
    "visual-novel": "剧情与画面从服务器拉取,按键推进对话。",
}


def sha256_of(path: pathlib.Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for block in iter(lambda: handle.read(1 << 20), b""):
            digest.update(block)
    return digest.hexdigest()


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("release_dir", type=pathlib.Path,
                        help="directory holding the *-full.bin artifacts")
    parser.add_argument("output_dir", type=pathlib.Path,
                        help="directory to write registry.json into")
    args = parser.parse_args()

    tag = os.environ.get("GITHUB_REF_NAME", "v0.0.0")
    repo = os.environ.get("GITHUB_REPOSITORY", "blkot/my-folotoy")
    version = tag.removeprefix("v")

    firmwares = []
    for binary in sorted(args.release_dir.glob("*-full.bin")):
        firmware_id = binary.name.removesuffix("-full.bin")
        data = binary.stat().st_size
        firmwares.append({
            "id": firmware_id,
            "name": DISPLAY_NAMES.get(firmware_id, firmware_id),
            "version": version,
            "git_tag": tag,
            "channel": "stable",
            "target": TARGET,
            "chip_family": CHIP_FAMILY,
            "flash_size_bytes": FLASH_SIZE_BYTES,
            "offset": OFFSET,
            "size_bytes": data,
            "sha256": sha256_of(binary),
            "url": f"https://github.com/{repo}/releases/download/{tag}/{binary.name}",
            "release_url": f"https://github.com/{repo}/releases/tag/{tag}",
            "min_companion_version": None,
            "notes": DISPLAY_NOTES.get(firmware_id, ""),
        })

    if not firmwares:
        print(f"no *-full.bin found in {args.release_dir}", flush=True)
        return 1

    registry = {
        "schema": 1,
        "channel": "stable",
        "generated_at": datetime.datetime.now(datetime.timezone.utc)
                                 .replace(microsecond=0).isoformat(),
        "firmwares": firmwares,
    }

    args.output_dir.mkdir(parents=True, exist_ok=True)
    out = args.output_dir / "registry.json"
    out.write_text(json.dumps(registry, indent=2, ensure_ascii=False) + "\n",
                   encoding="utf-8")

    for item in firmwares:
        print(f"{item['id']:<16} {item['size_bytes']:>9} B  {item['sha256'][:16]}...")
    print(f"written: {out}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
