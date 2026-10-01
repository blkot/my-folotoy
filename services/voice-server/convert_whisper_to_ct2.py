"""Convert a cached HuggingFace Whisper model into CTranslate2 format.

Why this exists: the 3 GB ``faster-whisper-large-v3`` download stalls at zero
bytes on this network (large files go through the Xet transfer domain, which is
unreachable), while ``openai/whisper-large-v3-turbo`` is already fully cached.
CTranslate2 can convert it locally, so we get a fast local model with no
download at all.

Run with an environment that has torch + transformers (e.g. ComfyUI_311):
    python convert_whisper_to_ct2.py <hf-model-id> <output-dir>
"""

from __future__ import annotations

import sys
import time
from pathlib import Path


def main() -> int:
    if len(sys.argv) < 3:
        print(__doc__)
        return 2

    model_id = sys.argv[1]
    output_dir = Path(sys.argv[2])
    output_dir.mkdir(parents=True, exist_ok=True)

    import torch  # noqa: PLC0415
    import transformers  # noqa: PLC0415
    from ctranslate2.converters import TransformersConverter  # noqa: PLC0415

    print(f"torch        : {torch.__version__} (cuda={torch.cuda.is_available()})")
    print(f"transformers : {transformers.__version__}")
    print(f"source       : {model_id}")
    print(f"target       : {output_dir}")

    started = time.monotonic()
    converter = TransformersConverter(model_id)
    # quantization="float16" keeps GPU inference fast and halves the file size.
    # force=True because mkdir above already created the directory; CTranslate2
    # refuses to write into an existing one otherwise.
    converter.convert(str(output_dir), quantization="float16", force=True)
    print(f"converted in {time.monotonic() - started:.1f} s")

    for path in sorted(output_dir.iterdir()):
        size = path.stat().st_size / 1e6
        print(f"  {path.name:32s} {size:10.1f} MB")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
