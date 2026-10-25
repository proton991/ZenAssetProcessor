"""Independent BC4/BC5 package audit; raw storage isolates texture processing.

The decoder implements the endpoint/index interpolation described at
https://learn.microsoft.com/en-us/windows/win32/direct3d10/d3d10-graphics-programming-guide-resources-block-compression
and does not call the processor's bcdec implementation. Every source is copied
into this run's output, then hidden before Reader validation and roundtrip.
Existing BC7 results are never rewritten.
"""
import argparse
import json
import math
from pathlib import Path
from asset_processor_paths import PROCESSOR_ROOT
import shutil
import struct

import numpy as np
from PIL import Image

import asset_processor_baseline as baseline
from asset_processor_quantization import Fields, package
import asset_processor_texture_audit as textures

ROOT = PROCESSOR_ROOT
DECODER_SPEC = "https://learn.microsoft.com/en-us/windows/win32/direct3d10/d3d10-graphics-programming-guide-resources-block-compression"


def decode_bc45(image, level):
    """Return unquantized normalized R or RG; padding is decoded then cropped."""
    width, height = max(1, image["width"] >> level), max(1, image["height"] >> level)
    channels = 1 if image["format"] == 7 else 2
    blocks_wide, blocks_high = (width + 3) // 4, (height + 3) // 4
    blocks = np.frombuffer(image["mips"][level], dtype=np.uint8).reshape(-1, channels, 8)
    result = np.empty((height, width, channels), dtype=np.float64)
    for channel in range(channels):
        data = blocks[:, channel, :].astype(np.uint64)
        first, second = data[:, 0].astype(np.float64) / 255, data[:, 1].astype(np.float64) / 255
        palette = np.empty((len(data), 8), dtype=np.float64)
        palette[:, 0], palette[:, 1] = first, second
        for index in range(2, 8):
            seven = ((8 - index) * first + (index - 1) * second) / 7
            five = ((6 - index) * first + (index - 1) * second) / 5 if index < 6 else float(index == 7)
            palette[:, index] = np.where(first > second, seven, five)
        packed = np.sum(data[:, 2:] << (np.arange(6, dtype=np.uint64) * 8), axis=1, dtype=np.uint64)
        indices = (packed[:, None] >> (np.arange(16, dtype=np.uint64) * 3)) & 7
        decoded = np.take_along_axis(palette, indices.astype(np.intp), axis=1)
        tiled = decoded.reshape(blocks_high, blocks_wide, 4, 4).transpose(0, 2, 1, 3)
        result[:, :, channel] = tiled.reshape(blocks_high * 4, blocks_wide * 4)[:height, :width]
    return result


def decoder_self_test():
    indices = [0, 1, 2, 3, 4, 5, 6, 7] * 2
    packed = sum(value << (3 * index) for index, value in enumerate(indices)).to_bytes(6, "little")
    for endpoints, expected in (((255, 0), [1, 0, 6/7, 5/7, 4/7, 3/7, 2/7, 1/7]),
                                ((0, 255), [0, 1, 1/5, 2/5, 3/5, 4/5, 0, 1])):
        image = dict(width=4, height=4, format=7, mips=[bytes(endpoints) + packed])
        np.testing.assert_allclose(decode_bc45(image, 0).ravel(), expected * 2, rtol=0, atol=1e-15)
    image = dict(width=3, height=2, format=9, mips=[bytes([255, 0]) + packed + bytes([0, 255]) + packed])
    values = decode_bc45(image, 0)
    assert values.shape == (2, 3, 2) and values[0, 0].tolist() == [1, 0]


def material_without_texture_ids(data):
    result, fields = bytearray(data), Fields(data)
    fields.string()
    fields.skip(4 + 1 + 5 * 4 + 5 * 52 + 33 * 4)
    for _ in range(17):
        result[fields.offset:fields.offset + 4] = bytes(4)
        fields.skip(4 + 4 + 52 + 4)
    fields.skip(16 + 16)
    result[fields.offset:fields.offset + 20] = bytes(20)
    fields.skip(20 + 24)
    result[fields.offset:fields.offset + 8] = bytes(8)
    fields.skip(8 + 16 + 12 + 3 + 4)
    fields.finish()
    return bytes(result)


def quality(expected, actual, fmt, limits, sampler=None):
    if fmt == 7:
        error = float(np.max(np.abs(expected[:, :, 0].astype(np.float64) / 255 - actual[:, :, 0])))
        return dict(consumed_r_max_error=error,
                    passed=error <= limits["scalar_channel_max_absolute_error_unit_range"] + 1e-12)
    original = expected[:, :, :3].astype(np.float64) / 255 * 2 - 1
    original /= np.linalg.norm(original, axis=2, keepdims=True)
    xy = actual * 2 - 1
    restored = np.concatenate((xy, np.sqrt(np.maximum(0, 1 - np.sum(xy * xy, axis=2)))[:, :, None]), axis=2)
    restored /= np.linalg.norm(restored, axis=2, keepdims=True)
    angles = np.degrees(np.arccos(np.clip(np.sum(original * restored, axis=2), -1, 1)))
    rms, maximum = float(np.sqrt(np.mean(angles * angles))), float(np.max(angles))
    positive = bool(np.all(original[:, :, 2] >= 0) and np.all(restored[:, :, 2] >= 0))
    filtered = textures.filtered_normal_quality(expected, actual, True, False, sampler or {})
    return dict(normal_rms_degrees=rms, normal_max_degrees=maximum, positive_z=positive, **filtered,
                passed=positive and rms <= limits["normal_rms_angular_error_max_degrees"]
                and maximum <= limits["normal_max_angular_error_degrees"]
                and filtered["filtered_normal_rms_degrees"] <= limits["normal_rms_angular_error_max_degrees"]
                and filtered["filtered_normal_max_degrees"] <= limits["normal_max_angular_error_degrees"])


def allocated_mips(images, views):
    total = 0
    for view in views.values():
        image = images[view["image"]]
        if image["authored_mip_count"]:
            total += sum(map(len, image["mips"]))
        else:
            width, height = image["width"], image["height"]
            while True:
                total += width * height * 4
                if width == height == 1:
                    break
                width, height = max(1, width // 2), max(1, height // 2)
    return total


def audit(paths, limits):
    resources = {profile: package(path) for profile, path in paths.items()}
    source, source_views, source_uses = textures.scene_images(resources["reference"])
    control, control_views, control_uses = textures.scene_images(resources["mips"])
    images, views, uses = textures.scene_images(resources["bc45"])
    samplers = textures.scene_samplers(resources["bc45"])
    sampler_mips = {}
    for (kind, index), data in resources["bc45"].items():
        if kind == 15:
            fields = Fields(data)
            fields.string()
            fields.skip(12)
            sampler_mips[index] = bool(fields.unpack("B"))
            fields.skip(8)
            fields.finish()
    default_equal = resources["default"] == resources["reference"]
    unchanged = True
    for key, value in resources["reference"].items():
        if key[0] == 3:
            unchanged &= material_without_texture_ids(value) == material_without_texture_ids(resources["bc45"][key])
        elif key[0] not in (1, 4, 9, 13):
            unchanged &= value == resources["bc45"].get(key)
    if uses.keys() != source_uses.keys() or uses.keys() != control_uses.keys():
        raise ValueError("Binding membership changed")
    rows, retained, seen = [], [], set()
    descriptors_equal = True
    for binding, use in uses.items():
        view = views[use["texture"]]
        original_view = source_views[source_uses[binding]["texture"]]
        original = source[original_view["image"]]
        image = images[view["image"]]
        descriptors_equal &= all(view[key] == original_view[key] for key in ("name", "sampler", "linear_source", "flat"))
        descriptors_equal &= {k: v for k, v in use.items() if k != "texture"} == {k: v for k, v in source_uses[binding].items() if k != "texture"}
        if image["format"] not in (7, 9):
            same = image["base"] == original["base"] and image["mips"] == original["mips"]
            retained.append(dict(material=binding[0], slot=binding[1], image=view["image"], role=use["role"],
                                 original_pixels_retained=same, format=image["format"], interpretation=view["interpretation"]))
        else:
            role_valid = (image["format"] == 7 and binding[1] == 3 and view["interpretation"] == 0
                          or image["format"] == 9 and use["role"] == "normal" and view["interpretation"] == 1 and use["normal_scale"] == 1)
            descriptors_equal &= role_valid and view["format"] == image["format"] and original_view["format"] == 3
            key = view["image"], image["format"], view["sampler"]
            if key in seen:
                continue
            seen.add(key)
            expected = control[control_views[control_uses[binding]["texture"]]["image"]]
            requires_mips = sampler_mips.get(view["sampler"], True)
            complete = max(1, image["width"] >> (len(image["mips"]) - 1)) == 1 and max(1, image["height"] >> (len(image["mips"]) - 1)) == 1
            if len(image["mips"]) > len(expected["mips"]) or requires_mips and not complete:
                raise ValueError("Accepted BC4/5 image does not provide its sampler-required mip chain")
            mip_rows = []
            for level in range(len(image["mips"])):
                row = quality(textures.rgba(expected, level), decode_bc45(image, level), image["format"], limits,
                              samplers.get(view["sampler"], {}))
                row.update(level=level, width=max(1, image["width"] >> level), height=max(1, image["height"] >> level))
                if image["format"] == 9 and level > 0:
                    measured = textures.mip_pair_quality(textures.linear_rgb(textures.rgba(expected, level - 1), False),
                                                        textures.linear_rgb(textures.rgba(expected, level), False),
                                                        decode_bc45(image, level - 1), decode_bc45(image, level), True,
                                                        samplers.get(view["sampler"], {}))
                    row.update(measured)
                    row["passed"] &= measured["mip_normal_rms_degrees"] <= limits["normal_rms_angular_error_max_degrees"] and measured["mip_normal_max_degrees"] <= limits["normal_max_angular_error_degrees"]
                mip_rows.append(row)
            rows.append(dict(image=view["image"], texture=use["texture"], material=binding[0], slot=binding[1],
                             format=textures.FORMAT_NAMES[image["format"]], sampler_requires_mipmaps=requires_mips,
                             complete_chain=complete, mips=mip_rows))
    summary = textures.image_summary(images)
    summary.update(reference=textures.image_summary(source), mip_control=textures.image_summary(control),
                   default_reference_equal=default_equal, non_texture_payloads_unchanged=bool(unchanged),
                   binding_descriptors_preserved=bool(descriptors_equal), quality=rows, retained_bindings=retained,
                   reference_texture_gpu_mip_bytes=allocated_mips(source, source_views),
                   cooked_texture_gpu_mip_bytes=allocated_mips(images, views),
                   reference_texture_count=len(source_views), cooked_texture_count=len(views))
    summary["passed"] = (default_equal and unchanged and descriptors_equal and all(row["original_pixels_retained"] for row in retained)
                         and all(row["passed"] for image in rows for row in image["mips"]))
    return summary


def focused_fixture(directory, variant):
    path = textures.shared_fixture(directory)
    document = json.loads(path.read_text())
    width, height = (5, 7) if variant == "odd" else (64, 64)
    yy, xx = np.indices((height, width))
    pixels = np.zeros((height, width, 4), dtype=np.uint8)
    pixels[:, :, :3] = [128, 128, 255]
    pixels[:, :, 3] = (xx * 71 + yy * 13 + 17).astype(np.uint8)
    if variant == "negative-z":
        pixels[:, :, 2] = 127
    if variant == "odd":
        pixels[:, :, 0] = (124 + xx % 5).astype(np.uint8)
        pixels[:, :, 1] = (125 + yy % 7).astype(np.uint8)
    Image.fromarray(pixels).save(directory / "shared.png")
    for material in document["materials"]:
        material["pbrMetallicRoughness"].update(baseColorTexture=dict(index=0), metallicRoughnessTexture=dict(index=0))
        material["normalTexture"] = dict(index=0, scale=100 if variant == "scaled" else 1)
        material["occlusionTexture"] = dict(index=0)
        material["extensions"] = dict(KHR_materials_clearcoat=dict(clearcoatFactor=1,
                                     clearcoatNormalTexture=dict(index=0, scale=2 if variant == "scaled" else 1)))
    document["extensionsUsed"] = ["KHR_materials_clearcoat"]
    path.write_text(json.dumps(document), encoding="utf-8")
    return path


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--processor", type=Path, required=True)
    parser.add_argument("--roundtrip", type=Path, required=True)
    parser.add_argument("--bc7-evidence", type=Path, required=True)
    parser.add_argument("--assets", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    for name in ("processor", "roundtrip", "bc7_evidence", "assets", "output"):
        setattr(args, name, getattr(args, name).resolve())
    decoder_self_test()
    args.output.mkdir(parents=True, exist_ok=False)
    previous = json.loads(args.bc7_evidence.read_text())
    cases = [(row["case"], Path(row["source"]["path"])) for row in previous["results"] if row["case"] != "shared-role"]
    cases += [("occlusion", args.assets / "Models/CompareAmbientOcclusion/glTF/CompareAmbientOcclusion.gltf")]
    cases += [(f"shared-{variant}", focused_fixture(args.output / f"fixture-{variant}", variant))
              for variant in ("flat", "odd", "negative-z", "scaled")]
    policy = json.loads(textures.POLICY.read_text())["desktop_profile"]["texture_encoding"]
    report = dict(processor=baseline.identity(args.processor), roundtrip=baseline.identity(args.roundtrip),
                  policy=baseline.identity(textures.POLICY), script=baseline.identity(Path(__file__)), decoder_spec=DECODER_SPEC,
                  decoder="Independent NumPy BC4 UNORM endpoint/index interpolation; BC5 applies two independent channels and positive-Z reconstruction",
                  decoder_self_test_passed=True, measurements="Single warm-cache diagnostic runs; not a benchmark acceptance claim", results=[])
    for name, original in cases:
        source_dir = args.output / f"{name}-source"
        if original.suffix.lower() in (".gltf", ".glb"):
            shutil.copytree(original.parent, source_dir)
        else:
            source_dir.mkdir()
            shutil.copy2(original, source_dir / original.name)
        source = source_dir / original.name
        row = dict(case=name, source=baseline.identity(original), copied_source=baseline.identity(source), packages={}, runs={})
        paths = {}
        profiles = (("default", []), ("reference", ["--profile", "reference"]),
                    ("mips", ["--profile", "desktop", "--texture-mipmaps", "on"]),
                    ("bc45", ["--profile", "desktop", "--normal-bc5", "on", "--occlusion-bc4", "on"]))
        for profile, flags in profiles:
            path = args.output / f"{name}-{profile}.zenasset"
            row["runs"][profile] = baseline.run([args.processor, "cook", source, "--output", path, "--workers", "2"] + flags,
                                                args.output / f"{name}-{profile}-cook.log", ROOT)
            if row["runs"][profile]["exit_code"]:
                raise RuntimeError(f"Cook failed: {name}/{profile}")
            paths[profile] = path
            row["packages"][profile] = baseline.identity(path)
        hidden = args.output / f"{name}-source-hidden"
        if not source_dir.resolve().is_relative_to(args.output) or not hidden.resolve().is_relative_to(args.output):
            raise ValueError("Source hiding escaped the audit output directory")
        source_dir.rename(hidden)
        try:
            row["source_unavailable_during_read"] = not source.exists() and not source_dir.exists()
            row["validate"] = baseline.run([args.processor, "validate", paths["bc45"]], args.output / f"{name}-validate.log", args.output)
            loaded = args.output / f"{name}-loaded.zenasset"
            row["roundtrip"] = baseline.run([args.roundtrip, paths["bc45"], loaded], args.output / f"{name}-roundtrip.log", args.output)
            if row["validate"]["exit_code"] or row["roundtrip"]["exit_code"]:
                raise RuntimeError(f"Source-free Reader failed: {name}")
            row["loaded_package"] = baseline.identity(loaded)
            row["loaded_resources_equal"] = package(loaded) == package(paths["bc45"])
        finally:
            hidden.rename(source_dir)
        row["audit"] = audit(paths, policy)
        row["passed"] = row["source_unavailable_during_read"] and row["loaded_resources_equal"] and row["audit"]["passed"]
        report["results"].append(row)
        baseline.write(args.output / "results.json", report)
        print(f"{name}: {'PASS' if row['passed'] else 'FAIL'} BC5={row['audit']['bc5_images']} BC4={row['audit']['bc4_images']}", flush=True)
    report["processor_unchanged"] = report["processor"] == baseline.identity(args.processor)
    report["roundtrip_unchanged"] = report["roundtrip"] == baseline.identity(args.roundtrip)
    report["passed"] = (report["processor_unchanged"] and report["roundtrip_unchanged"] and all(row["passed"] for row in report["results"])
                        and all(sum(row["audit"][key] for row in report["results"]) > 0 for key in ("bc4_images", "bc5_images")))
    baseline.write(args.output / "results.json", report)
    return 0 if report["passed"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
