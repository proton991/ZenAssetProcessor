"""Exercise optional array cooking, strict limits, batch reuse and source-free reads."""
from concurrent.futures import ThreadPoolExecutor
import shutil
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


def directory_cache(executable, directory, scene_tests):
    root = directory / "model-cache"
    source = directory / "model.gltf"
    alternate = directory / "other" / "MODEL.gltf"
    alternate.parent.mkdir()
    fixture(source)
    fixture(alternate)

    def cook(path, cache=root, *options):
        result = invoke(executable, "cook", path, "--layout", "directory", "--output", cache, *options)
        return Path(json.loads(result.stdout)["output"])

    def parts(manifest):
        return json.loads(invoke(executable, "inspect", manifest).stdout)["parts"]

    exports = directory / "existing-exports"
    original = exports / source.stem / "approved.zenasset"
    original.parent.mkdir(parents=True)
    invoke(executable, "cook", source, "--output", original)
    claimed = cook(source, exports)
    assert claimed.parent != original.parent and list(original.parent.iterdir()) == [original]
    assert cook(source, exports) == claimed
    invoke(executable, "validate", original)
    original.parent.rename(exports / "archived")
    assert cook(source, exports) == claimed and not original.parent.exists()

    snapshot = claimed.with_name("approved.ZENASSET")
    shutil.copyfile(claimed, snapshot)
    edited = json.loads(source.read_text())
    edited["images"][0]["name"] = "revised"
    source.write_text(json.dumps(edited))
    cook(source, exports)
    invoke(executable, "validate", snapshot)
    fixture(source)

    with ThreadPoolExecutor(max_workers=4) as pool:
        paths = list(pool.map(cook, [source, alternate, source, alternate]))
    assert paths[0] == paths[2] and paths[1] == paths[3] and paths[0].parent != paths[1].parent
    for model, path in zip((source, alternate), paths):
        assert Path(json.loads((path.parent / "source.json").read_text(encoding="utf8"))["source"]) == model.resolve()
        invoke(executable, "validate", path)
    zstd = cook(source, root, "--compression", "zstd")
    assert zstd != paths[0] and zstd.parent == paths[0].parent
    # Exit after parts and the staged manifest have reached disk, before the commit.
    committed = zstd.read_bytes()
    invoke(scene_tests, "--interrupt-directory", zstd, expected=73)
    assert zstd.read_bytes() == committed
    invoke(executable, "validate", zstd)
    assert cook(source, root, "--compression", "zstd") == zstd

    relative = next(part["path"] for part in parts(zstd) if part["path"].startswith("textures/"))
    damaged = zstd.parent / relative
    data = damaged.read_bytes()
    damaged.write_bytes(bytes([data[0] ^ 1]) + data[1:])
    assert relative in invoke(executable, "validate", zstd, expected=3).stderr
    cook(source, root, "--compression", "zstd")
    assert damaged.read_bytes() == data
    damaged.unlink()
    assert relative in invoke(executable, "validate", zstd, expected=3).stderr
    cook(source, root, "--compression", "zstd")
    invoke(executable, "validate", zstd)

    desktop = ("--profile", "desktop", "--texture-mipmaps", "on", "--atlas", "array", "--texture-compression", "bc7")
    packed = cook(source, root, *desktop)
    before = parts(packed)
    document = json.loads(source.read_text())
    prefix, encoded = document["buffers"][0]["uri"].split(",")
    buffer = bytearray(base64.b64decode(encoded))
    struct.pack_into("<f", buffer, 36, 2.5)
    document["buffers"][0]["uri"] = prefix + "," + base64.b64encode(buffer).decode()
    source.write_text(json.dumps(document))
    assert cook(source, root, *desktop) == packed
    rebuilt = cook(source, directory / "clean", *desktop)
    after = parts(packed)
    assert [p for p in before if p["path"].startswith("textures/")] != [p for p in after if p["path"].startswith("textures/")]
    assert after == parts(rebuilt) and packed.read_bytes() == rebuilt.read_bytes()
    live_parts = {part["path"] for manifest in packed.parent.glob("*.zenasset") for part in parts(manifest)}
    stored_parts = {part.relative_to(packed.parent).as_posix() for part in packed.parent.glob("*/*.zenpart")}
    assert stored_parts == live_parts, "Recooks must retain exactly the parts used by all variants"
    # A vertex alpha change can switch a masked material from cooked to authored pixels.
    # Keep the compressed policy and image files fixed across the change.
    alpha = directory / "masked.gltf"
    fixture(alpha)
    material = json.loads(alpha.read_text())
    for entry in material["materials"]:
        entry["alphaMode"] = "MASK"
    material["buffers"].append({"uri": "", "byteLength": 48})
    material["bufferViews"].append({"buffer": 1, "byteOffset": 0, "byteLength": 48})
    material["accessors"].append({"bufferView": 3, "componentType": 5126, "count": 3, "type": "VEC4"})
    for primitive in material["meshes"][0]["primitives"]:
        primitive["attributes"]["COLOR_0"] = 3
    def write_alpha(value):
        rgba = struct.pack("<12f", *(1., 1., 1., value) * 3)
        material["buffers"][1]["uri"] = "data:application/octet-stream;base64," + base64.b64encode(rgba).decode()
        alpha.write_text(json.dumps(material))
    write_alpha(1.)
    masked = cook(alpha, root, *desktop)
    original = parts(masked)
    write_alpha(.25)
    assert cook(alpha, root, *desktop) == masked
    clean_masked = cook(alpha, directory / "clean-masked", *desktop)
    images = lambda records: {record["path"] for record in records if record["path"].startswith("textures/")}
    assert images(original) != images(parts(masked))
    assert masked.read_bytes() == clean_masked.read_bytes() and parts(masked) == parts(clean_masked)
    material["buffers"].append({"uri": "", "byteLength": 48})
    material["bufferViews"].append({"buffer": 2, "byteOffset": 0, "byteLength": 48})
    material["accessors"].append({"bufferView": 4, "componentType": 5126, "count": 3, "type": "VEC4"})
    for primitive in material["meshes"][0]["primitives"]:
        primitive["targets"] = [{"COLOR_0": 4}]
    material["meshes"][0]["weights"] = [1.]
    def write_morph(delta):
        values = struct.pack("<12f", *(0., 0., 0., delta) * 3)
        material["buffers"][2]["uri"] = "data:application/octet-stream;base64," + base64.b64encode(values).decode()
        write_alpha(1.)
    write_morph(0.)
    cook(alpha, root, *desktop)
    neutral = parts(masked)
    write_morph(-.25)
    cook(alpha, root, *desktop)
    rebuilt_morph = cook(alpha, directory / "clean-morph", *desktop)
    assert images(neutral) != images(parts(masked))
    assert masked.read_bytes() == rebuilt_morph.read_bytes() and parts(masked) == parts(rebuilt_morph)
    moved = directory / "moved"
    shutil.move(str(packed.parent), moved)
    source.unlink()
    invoke(executable, "validate", moved / packed.name, "--read-workers", 4)


def many_image_directory_cache(executable, directory):
    source = directory / "many-images.gltf"
    fixture(source)
    document = json.loads(source.read_text())
    image = document["images"][0]
    document["images"] = [dict(image, name=f"image-{index}") for index in range(520)]
    document["textures"] = [{"source": index} for index in range(len(document["images"]))]
    source.write_text(json.dumps(document))

    # Previous and new generations overlap during a recook. The relative output
    # must reuse geometry even above the default Windows CRT stream limit.
    args = ("cook", source.name, "--layout", "directory", "--output", "cache", "--workers", 4)
    first = json.loads(invoke(executable, *args, cwd=directory).stdout)
    manifest = directory / first["output"]
    assert len(list((manifest.parent / "textures").glob("*.zenpart"))) == 520
    invoke(executable, "validate", manifest, "--read-workers", 4)
    second = json.loads(invoke(executable, *args, cwd=directory).stdout)
    assert second["geometry_reused"] and second["part_bytes_written"] == 0
    document["images"][0]["name"] = "renamed"
    source.write_text(json.dumps(document))
    invoke(executable, *args, cwd=directory)
    live_images = {part["path"] for part in json.loads(invoke(executable, "inspect", manifest).stdout)["parts"]
                   if part["path"].startswith("textures/")}
    assert live_images == {p.relative_to(manifest.parent).as_posix() for p in manifest.parent.glob("textures/*.zenpart")}


def main():
    executable = Path(sys.argv[1]).resolve()
    with tempfile.TemporaryDirectory(prefix="zenasset-array-cli-") as temporary:
        directory = Path(temporary)
        directory_cache(executable, directory, Path(sys.argv[2]).resolve())
        many_image_directory_cache(executable, directory)
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
        assert sum(resource["type"] == 13 and resource["version"] == 3 for resource in resources) == 1
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
