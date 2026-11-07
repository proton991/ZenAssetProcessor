"""Create an explicit five-model assembly build manifest, without editing sources."""

import argparse
import json
import os
from pathlib import Path

from multi_model_baseline import MODELS


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, required=True)
    parser.add_argument(
        "--candles", type=Path, required=True, help="Selectively extracted candle glTF"
    )
    parser.add_argument("--manifest", type=Path, required=True)
    parser.add_argument("--output-dir", type=Path, required=True)
    parser.add_argument(
        "--reference",
        action="store_true",
        help="Lossless RGBA baseline; not the runtime BC profile",
    )
    parser.add_argument("--quality", type=int, choices=range(5), default=4)
    args = parser.parse_args()
    path = args.manifest.resolve()
    sources = {key: args.root.resolve() / relative for key, relative in MODELS.items()}
    sources["candles"] = args.candles.resolve()
    if not all(source.is_file() for source in sources.values()):
        parser.error(
            "Every selected glTF must exist; extract the candle glTF/BIN first"
        )
    jobs = []
    for key, source in sources.items():
        job = {
            "key": key,
            "source": os.path.relpath(source, path.parent).replace(os.sep, "/"),
            "profile": "reference" if args.reference else "desktop",
            "compression": {
                "transport": "zstd",
                "zstandardLevel": 3,
                "meshoptimizer": True,
            },
        }
        if not args.reference:
            job["processing"] = {
                "textures": {
                    "generateMipmaps": True,
                    "compressBC7": True,
                    "compressBC5Normals": True,
                    "compressBC4Occlusion": True,
                    "bc7Quality": args.quality,
                }
            }
        jobs.append(job)
    document = {
        "version": 2,
        "outputDirectory": str(args.output_dir.resolve()),
        "assembly": "sponza.zenscene",
        "name": "Sponza main and all four add-ons",
        "jobs": jobs,
        "instances": [
            {"key": key, "model": key, "submitLights": key != "candles"}
            for key in sources
        ],
    }
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(document, indent=2) + "\n", encoding="utf-8")
    print(path)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
