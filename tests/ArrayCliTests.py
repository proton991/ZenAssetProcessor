"""Exercise optional array cooking, strict limits, batch reuse and source-free reads."""
import base64
import json
from pathlib import Path
import struct
import sys
import tempfile
import zlib

from CliTests import invoke


def png(color):
    def chunk(kind, data):
        return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data))
    header = struct.pack(">2I5B", 16, 16, 8, 6, 0, 0, 0)
    data = (b"\0" + bytes(color) * 16) * 16
    return b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", header) + chunk(b"IDAT", zlib.compress(data)) + chunk(b"IEND", b"")


def fixture(path):
    positions = struct.pack("<9f", 0, 0, 0, 1, 0, 0, 0, 1, 0)
    coordinates = struct.pack("<6f", 0.1, 0.2, 0.9, 0.2, 0.4, 0.9)
    indices = struct.pack("<3H", 0, 1, 2)
    buffer = positions + coordinates + indices
    document = {
        "asset": {"version": "2.0"},
        "buffers": [{"uri": "data:application/octet-stream;base64," + base64.b64encode(buffer).decode(), "byteLength": len(buffer)}],
        "bufferViews": [
            {"buffer": 0, "byteOffset": 0, "byteLength": len(positions)},
            {"buffer": 0, "byteOffset": len(positions), "byteLength": len(coordinates)},
            {"buffer": 0, "byteOffset": len(positions + coordinates), "byteLength": len(indices)},
        ],
        "accessors": [
            {"bufferView": 0, "componentType": 5126, "count": 3, "type": "VEC3", "min": [0, 0, 0], "max": [1, 1, 0]},
            {"bufferView": 1, "componentType": 5126, "count": 3, "type": "VEC2"},
            {"bufferView": 2, "componentType": 5123, "count": 3, "type": "SCALAR"},
        ],
        "images": [{"uri": "data:image/png;base64," + base64.b64encode(png((40 + image * 70, 128, 160, 255))).decode()}
                   for image in range(3)],
        "samplers": [{"minFilter": 9987, "magFilter": 9729, "wrapS": 33071, "wrapT": 33071}],
        "textures": [{"source": image, "sampler": 0} for image in range(3)],
        "materials": [{"pbrMetallicRoughness": {"baseColorTexture": {"index": image}}} for image in range(3)],
        "meshes": [{"primitives": [{"attributes": {"POSITION": 0, "TEXCOORD_0": 1}, "indices": 2, "material": image}
                                    for image in range(3)]}],
        "nodes": [{"mesh": 0}], "scenes": [{"nodes": [0]}], "scene": 0,
    }
    path.write_text(json.dumps(document), encoding="utf-8")


def main():
    executable = Path(sys.argv[1]).resolve()
    with tempfile.TemporaryDirectory(prefix="zenasset-array-cli-") as temporary:
        directory = Path(temporary)
        source = directory / "three-images.gltf"
        fixture(source)
        plain = directory / "standalone.zenasset"
        packed = directory / "array.zenasset"
        common = ("--profile", "desktop", "--texture-mipmaps", "on", "--texture-compression", "bc7")
        invoke(executable, "cook", source, "-o", plain, *common, "--atlas", "off")
        invoke(executable, "cook", source, "-o", packed, *common, "--atlas", "array",
               "--atlas-max-dimension", 512, "--atlas-max-layers", 4, "--atlas-max-pages", 8, "--atlas-max-images", 32)
        reference = json.loads(invoke(executable, "validate", plain).stdout)
        result = json.loads(invoke(executable, "validate", packed).stdout)
        assert result["images"] == reference["images"] - 2, (reference, result)
        for field in ("nodes", "meshes", "vertices", "indices", "materials", "animations", "skins"):
            assert result[field] == reference[field], field
        resources = json.loads(invoke(executable, "inspect", packed).stdout)["resources"]
        assert sum(resource["type"] == 13 and resource["version"] == 2 for resource in resources) == 1
        assert any(resource["type"] == 4 and resource["version"] == 3 for resource in resources)
        assert any(resource["type"] == 9 and resource["version"] == 7 for resource in resources)
        before = packed.read_bytes()
        for option, value in (("--atlas", "on"), ("--atlas-max-dimension", 0), ("--atlas-max-dimension", 16385),
                              ("--atlas-max-layers", 1), ("--atlas-max-layers", 2049), ("--atlas-max-pages", 0),
                              ("--atlas-max-images", 0), ("--atlas-max-images", 4294967296)):
            invoke(executable, "cook", source, "-o", packed, *common, option, value, expected=2)
            assert packed.read_bytes() == before
        invoke(executable, "cook", source, "-o", packed, "--atlas", "array", expected=2)
        assert packed.read_bytes() == before

        manifest = directory / "build.json"
        settings = {"generateMipmaps": True, "compressBC7": True, "arrayPages": True,
                    "arrayMaxDimension": 512, "arrayMaxLayers": 4, "arrayMaxPages": 8, "arrayMaxImages": 32}
        document = {"version": 1, "outputDirectory": "batch", "jobs": [
            {"source": source.name, "output": "array.zenasset", "profile": "desktop", "processing": {"textures": settings}}
        ]}
        manifest.write_text(json.dumps(document), encoding="utf-8")
        built = json.loads(invoke(executable, "build", manifest).stdout)
        assert built["jobs"][0]["disposition"] == "built"
        reused = json.loads(invoke(executable, "build", manifest).stdout)
        assert reused["jobs"][0]["disposition"] == "reused"
        settings["arrayMaxLayers"] = 2
        manifest.write_text(json.dumps(document), encoding="utf-8")
        recooked = json.loads(invoke(executable, "build", manifest).stdout)
        assert recooked["jobs"][0]["disposition"] == "built"
        split = json.loads(invoke(executable, "validate", directory / "batch/array.zenasset").stdout)
        assert split["images"] == reference["images"] - 1
        source.rename(directory / "source-unavailable.gltf")
        invoke(executable, "validate", packed)
        invoke(executable, "validate", directory / "batch/array.zenasset")
    print("Array CLI, manifest reuse/invalidation, source-free reads and strict-limit checks passed")


if __name__ == "__main__":
    main()
