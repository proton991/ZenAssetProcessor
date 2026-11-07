"""Measure one authored 4K texture per role before admitting a complete BC cook.

Outputs are generated glTF fixtures and bounded processor measurements. Images
are referenced without resizing or alteration. No renderer or GPU test is run.
"""

import argparse
import base64
import json
import os
from pathlib import Path
import shutil
import struct

from asset_processor_baseline import identity
from multi_model_baseline import measure, write


def fixture(path, image, role):
    vertices = struct.pack(
        "<24f", 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 1, 0, 0, 1, 0, 0, 1, 0, 0, 1
    )
    blob = vertices + struct.pack("<3H", 0, 1, 2)
    material = {"pbrMetallicRoughness": {"metallicFactor": 0}}
    if role == "color":
        material["pbrMetallicRoughness"]["baseColorTexture"] = {"index": 0}
    else:
        material["normalTexture" if role == "normal" else "occlusionTexture"] = {
            "index": 0
        }
    document = {
        "asset": {"version": "2.0"},
        "scene": 0,
        "scenes": [{"nodes": [0]}],
        "nodes": [{"mesh": 0}],
        "buffers": [
            {
                "byteLength": len(blob),
                "uri": "data:application/octet-stream;base64,"
                + base64.b64encode(blob).decode(),
            }
        ],
        "bufferViews": [
            {"buffer": 0, "byteOffset": offset, "byteLength": size}
            for offset, size in [(0, 36), (36, 36), (72, 24), (96, 6)]
        ],
        "accessors": [
            {
                "bufferView": 0,
                "componentType": 5126,
                "count": 3,
                "type": "VEC3",
                "min": [0, 0, 0],
                "max": [1, 1, 0],
            },
            {"bufferView": 1, "componentType": 5126, "count": 3, "type": "VEC3"},
            {"bufferView": 2, "componentType": 5126, "count": 3, "type": "VEC2"},
            {"bufferView": 3, "componentType": 5123, "count": 3, "type": "SCALAR"},
        ],
        "meshes": [
            {
                "primitives": [
                    {
                        "attributes": {"POSITION": 0, "NORMAL": 1, "TEXCOORD_0": 2},
                        "indices": 3,
                        "material": 0,
                    }
                ]
            }
        ],
        "images": [{"uri": os.path.relpath(image, path.parent).replace(os.sep, "/")}],
        "textures": [{"source": 0, "sampler": 0}],
        "samplers": [{"minFilter": 9987, "magFilter": 9729}],
        "materials": [material],
    }
    write(path, document)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, required=True)
    parser.add_argument("--compiler", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--timeout", type=float, default=60)
    args = parser.parse_args()
    output = args.output.resolve()
    workspace = Path(__file__).resolve().parents[2]
    if not output.is_relative_to(workspace / "build") or args.timeout <= 0:
        parser.error("Use a new output below processor/build and a positive timeout")
    output.mkdir(parents=True, exist_ok=False)
    compiler = output / args.compiler.name
    shutil.copy2(args.compiler.resolve(), compiler)
    images = {
        "color": "arch_stone_wall_01_BaseColor.png",
        "normal": "arch_stone_wall_01_Normal.png",
        "scalar": "arch_stone_wall_01_Roughnessarch_stone_wall_01_Metalness.png",
    }
    results = {
        "compiler": identity(compiler),
        "samples": [],
        "method": "Fresh processes; warm/uncontrolled filesystem cache; full 4K; explicit mips; default quality thresholds; scalar uses packed input R as isolated occlusion fixture",
    }
    for role, filename in images.items():
        source = output / (role + ".gltf")
        image = args.root.resolve() / "textures" / filename
        fixture(source, image, role)
        for quality in (range(5) if role == "color" else [4]):
            package = output / f"{role}-q{quality}.zenasset"
            command = [
                compiler,
                "cook",
                source,
                "--output",
                package,
                "--profile",
                "desktop",
                "--workers",
                "2",
                "--texture-mipmaps",
                "on",
                "--texture-compression",
                "bc7",
                "--normal-bc5",
                "on",
                "--occlusion-bc4",
                "on",
                "--texture-quality",
                str(quality),
                "--compression",
                "zstd",
                "--zstd-level",
                "3",
            ]
            result = measure(
                command,
                output / f"{role}-q{quality}.log",
                output,
                args.timeout,
                4 << 30,
            )
            result.update(role=role, quality=quality, source_image=identity(image))
            if result["successful"]:
                result["package"] = identity(package)
            results["samples"].append(result)
            write(output / "results.json", results)
            print(json.dumps(result), flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
