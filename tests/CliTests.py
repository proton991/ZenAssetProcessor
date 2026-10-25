"""Exercise the installed-style CLI contract, including source-free packages."""

import hashlib
import json
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import zlib


def invoke(executable, *arguments, expected=0):
    result = subprocess.run(
        [str(executable), *map(str, arguments)], capture_output=True, text=True, encoding="utf-8"
    )
    if result.returncode != expected:
        raise AssertionError(
            f"{arguments}: exit {result.returncode}, expected {expected}\n{result.stdout}\n{result.stderr}"
        )
    return result


def solid_png(width, height):
    def chunk(kind, payload):
        return struct.pack(">I", len(payload)) + kind + payload + struct.pack(">I", zlib.crc32(kind + payload))
    header = struct.pack(">2I5B", width, height, 8, 6, 0, 0, 0)
    pixels = b"".join(b"\0" + bytes((128, 64, 32, 255)) * width for _ in range(height))
    return b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", header) + chunk(b"IDAT", zlib.compress(pixels)) + chunk(b"IEND", b"")


def main():
    executable = Path(sys.argv[1]).resolve()
    invoke(executable, "--help")
    invoke(executable, "--version")
    invoke(executable, "cook", expected=2)
    with tempfile.TemporaryDirectory(prefix="zenasset-cli-") as temporary:
        directory = Path(temporary)
        source = directory / "삼각형.gltf"
        buffer = directory / "geometry.bin"
        image = directory / "texture.png"
        image.write_bytes(solid_png(32, 32))
        package = directory / "삼각형.zenasset"
        buffer.write_bytes(struct.pack("<15f3H", 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 1, 0, 1, 2))
        source.write_text(
            json.dumps(
                {
                    "asset": {"version": "2.0"},
                    "buffers": [{"uri": buffer.name, "byteLength": 66}],
                    "bufferViews": [
                        {"buffer": 0, "byteOffset": 0, "byteLength": 36},
                        {"buffer": 0, "byteOffset": 60, "byteLength": 6},
                        {"buffer": 0, "byteOffset": 36, "byteLength": 24},
                    ],
                    "accessors": [
                        {"bufferView": 0, "componentType": 5126, "count": 3, "type": "VEC3", "min": [0, 0, 0], "max": [1, 1, 0]},
                        {"bufferView": 1, "componentType": 5123, "count": 3, "type": "SCALAR"},
                        {"bufferView": 2, "componentType": 5126, "count": 3, "type": "VEC2"},
                    ],
                    "images": [{"uri": image.name}],
                    "textures": [{"source": 0}],
                    "materials": [{"pbrMetallicRoughness": {"baseColorTexture": {"index": 0}}}],
                    "meshes": [{"primitives": [{"attributes": {"POSITION": 0, "TEXCOORD_0": 2}, "indices": 1, "material": 0}]}],
                    "nodes": [{"mesh": 0}],
                    "scenes": [{"nodes": [0]}],
                    "scene": 0,
                }
            ),
            encoding="utf-8",
        )
        source_hash = hashlib.sha256(source.read_bytes()).hexdigest()
        invoke(executable, "cook", source, "-o", source, expected=2)
        assert hashlib.sha256(source.read_bytes()).hexdigest() == source_hash
        cooked = json.loads(invoke(executable, "cook", source, "--output", package, "--workers", 2).stdout)
        assert cooked["meshes"] == 1 and cooked["vertices"] == 3
        inspected = json.loads(invoke(executable, "inspect", package).stdout)
        assert inspected["schema"] == 3 and len(inspected["resources"]) >= 4
        package_hash = hashlib.sha256(package.read_bytes()).hexdigest()
        compressed = directory / "compressed.zenasset"
        invoke(executable, "cook", source, "-o", compressed, "--compression", "zstd", "--zstd-level", 3)
        compressed_directory = json.loads(invoke(executable, "inspect", compressed).stdout)
        assert any(resource["codec"] == 1 for resource in compressed_directory["resources"])
        mesh_packages = []
        for transport in ("none", "zstd"):
            mesh_package = directory / f"mesh-{transport}.zenasset"
            invoke(
                executable, "cook", source, "-o", mesh_package,
                "--mesh-compression", "meshopt", "--compression", transport,
            )
            mesh_directory = json.loads(invoke(executable, "inspect", mesh_package).stdout)
            assert any(resource["encoding"] == 1 for resource in mesh_directory["resources"])
            mesh_packages.append(mesh_package)
        invoke(executable, "validate", compressed, "--codec-budget", 1, expected=3)
        invoke(executable, "cook", source, "-o", package, "--mesh-compression", "invalid", expected=2)
        invoke(executable, "cook", source, "-o", package, "--vertex-fetch", "on", expected=2)
        invoke(executable, "cook", source, "-o", package, "--profile", "desktop", "--position-bits", 25, expected=2)
        invoke(executable, "cook", source, "-o", package, "--profile", "desktop", "--position-max-error", "nan", expected=2)
        invoke(executable, "cook", source, "-o", package, "--texture-mipmaps", "on", expected=2)
        invoke(executable, "cook", source, "-o", package, "--texture-compression", "bc7", expected=2)
        for option in ("--normal-bc5", "--occlusion-bc4", "--environment-bc6h"):
            invoke(executable, "cook", source, "-o", package, option, "on", expected=2)
            invoke(executable, "cook", source, "-o", package, "--profile", "desktop", option, "invalid", expected=2)
        for option, value in (
            ("--texture-compression", "invalid"), ("--texture-mipmaps", "invalid"),
            ("--texture-quality", 5), ("--texture-max-dimension", 0), ("--color-min-psnr", "nan"),
            ("--color-max-rmse", 1.1), ("--scalar-max-error", -1), ("--normal-rms-degrees", 6),
            ("--normal-max-degrees", 181), ("--alpha-coverage-error", "inf"),
            ("--hdr-relative-rmse", "nan"),
        ):
            invoke(executable, "cook", source, "-o", package, "--profile", "desktop", option, value, expected=2)
        processed = directory / "processed.zenasset"
        invoke(
            executable, "cook", source, "-o", processed, "--profile", "desktop",
            "--vertex-fetch", "on", "--position-bits", 16,
            "--compression", "zstd", "--mesh-compression", "meshopt",
        )
        texture_package = directory / "textures.zenasset"
        invoke(
            executable, "cook", source, "-o", texture_package, "--profile", "desktop",
            "--texture-mipmaps", "on", "--texture-compression", "bc7", "--texture-quality", 4,
            "--texture-max-dimension", 4096, "--color-min-psnr", 40, "--color-max-rmse", 0.01,
            "--scalar-max-error", 0.03, "--normal-rms-degrees", 1, "--normal-max-degrees", 5,
            "--alpha-coverage-error", 0.01,
        )
        assert texture_package.stat().st_size < package.stat().st_size
        invoke(executable, "cook", source, "-o", package, "--compression", "zstd", "--zstd-level", 0, expected=2)
        invoke(executable, "cook", source, "-o", package, "--compression", "zstd", "--encoder-budget", 1, expected=3)
        assert hashlib.sha256(package.read_bytes()).hexdigest() == package_hash
        invoke(executable, "cook", source, "--output", package, "--workers", "overflow", expected=2)
        assert hashlib.sha256(package.read_bytes()).hexdigest() == package_hash
        bad_source = directory / "invalid.gltf"
        bad_source.write_text("not JSON", encoding="utf-8")
        invoke(executable, "cook", bad_source, "--output", package, expected=3)
        assert hashlib.sha256(package.read_bytes()).hexdigest() == package_hash
        source.unlink()
        buffer.unlink()
        image.unlink()
        validated = json.loads(invoke(executable, "validate", package).stdout)
        assert validated["valid"] and validated["vertices"] == 3
        assert json.loads(invoke(executable, "validate", compressed).stdout) == validated
        for mesh_package in mesh_packages:
            assert json.loads(invoke(executable, "validate", mesh_package).stdout) == validated
        assert json.loads(invoke(executable, "validate", processed).stdout) == validated
        assert json.loads(invoke(executable, "validate", texture_package).stdout) == validated
        invoke(executable, "validate", package, "--allocation-budget", 1, expected=3)
        invoke(executable, "inspect", package, "--max-file-bytes", 10, expected=3)
        corrupt = bytearray(package.read_bytes())
        corrupt[-1] ^= 1
        package.write_bytes(corrupt)
        invoke(executable, "validate", package, expected=3)
    print("CLI cook/inspect/validate, Unicode, source-free load, bounds and publication failures passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
