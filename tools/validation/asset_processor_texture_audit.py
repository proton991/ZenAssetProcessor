"""Cook raw reference/mip-only/BC7 packages and independently audit texture payloads.

Requires NumPy and Pillow (its BC7 decoder is independent of the cooking codec).
Only validated raw v3 resources are parsed. No transport or geometry encoding is
enabled, so each measured difference belongs to the explicit texture policy.
"""
import argparse
import base64
import json
import math
from pathlib import Path
from asset_processor_paths import PROCESSOR_ROOT, VALIDATION_TOOLS
import struct

import numpy as np
from PIL import Image
import PIL

import asset_processor_baseline as baseline
from asset_processor_normal_sampling import mip_pair_quality
from asset_processor_quantization import Fields, package, INVALID
import asset_processor_visual_parity as visual
import smoke_gltf_rendering as smoke

ROOT = PROCESSOR_ROOT
POLICY = VALIDATION_TOOLS / "asset_processor_quality.json"
FEATURE_ROLES = ["data", "color", "color", "color", "data", "data", "normal", "color",
                 "data", "data", "data", "data", "data", "data", "data", "color", "data"]
FORMAT_NAMES = {3: "RGBA8Unorm", 4: "RGBA8Srgb", 7: "BC4Unorm", 9: "BC5Unorm", 13: "BC7Unorm", 14: "BC7Srgb"}


def bytes_field(fields):
    count = fields.unpack("Q")
    data = fields.data[fields.offset:fields.offset + count]
    fields.skip(count)
    return data


def scene_images(resources):
    images, textures, bindings = {}, {}, {}
    for (kind, index), data in resources.items():
        f = Fields(data)
        if kind == 13:
            name = f.string()
            width, height, fmt = f.unpack("3I")
            base = bytes_field(f)
            mips = [bytes_field(f) for _ in range(f.unpack("Q"))]
            f.finish()
            if mips and mips[0] != base:
                raise ValueError("Base and mip[0] disagree")
            images[index] = dict(name=name, width=width, height=height, format=fmt, base=base,
                                 mips=mips or [base], authored_mip_count=len(mips),
                                 owned_bytes=len(base) + sum(map(len, mips)))
        elif kind == 4:
            name = f.string()
            image, sampler, linear_source, fmt = f.unpack("4I")
            flat = f.unpack("B")
            interpretation = f.unpack("I") if len(data) - f.offset == 4 else 0
            f.finish()
            textures[index] = dict(name=name, image=image, sampler=sampler, linear_source=linear_source,
                                   format=fmt, flat=flat, interpretation=interpretation)
        elif kind == 3:
            f.string()
            alpha_mode = f.unpack("I")
            f.skip(1)
            cutoff, metallic, roughness, normal_scale, occlusion = f.unpack("5f")
            f.skip(5 * 52 + 33 * 4)
            features = []
            for slot in range(17):
                texture = f.unpack("I")
                f.skip(4 + 52)
                scale = f.unpack("f")
                features.append((texture, scale))
            base_factor = f.unpack("4f")
            f.skip(16)
            core = f.unpack("5I")
            f.skip(24)
            spec_gloss, diffuse = f.unpack("2I")
            diffuse_factor = f.unpack("4f")
            f.skip(12 + 3 + 4)
            f.finish()
            uses = [(slot, tex, role, slot == 0, base_factor[3], normal_scale if slot == 2 else 1)
                    for slot, (tex, role) in enumerate(zip(core, ["color", "data", "normal", "data", "color"]))]
            uses += [(slot + 5, tex, FEATURE_ROLES[slot], slot == 2, diffuse_factor[3], scale)
                     for slot, (tex, scale) in enumerate(features)]
            uses += [(22, spec_gloss, "color", False, 1, 1), (23, diffuse, "color", True, diffuse_factor[3], 1)]
            for slot, texture, role, opacity, factor, scale in uses:
                if texture != INVALID:
                    bindings[index, slot] = dict(texture=texture, role=role,
                                                 alpha_mask=opacity and alpha_mode == 1 and factor > 0,
                                                 exact_alpha=opacity and alpha_mode == 2,
                                                 cutoff=cutoff / factor if factor > 0 else cutoff,
                                                 normal_scale=scale)
    return images, textures, bindings


def image_summary(images):
    rows = []
    for index, image in images.items():
        width, height = image["width"], image["height"]
        mips = []
        for level, data in enumerate(image["mips"]):
            block_bytes = {7: 8, 9: 16, 13: 16, 14: 16}.get(image["format"])
            expected = ((width + 3) // 4) * ((height + 3) // 4) * block_bytes if block_bytes else width * height * 4
            if len(data) != expected:
                raise ValueError("Mip footprint disagrees with image format")
            mips.append(dict(level=level, width=width, height=height, bytes=len(data)))
            width, height = max(1, width // 2), max(1, height // 2)
        rows.append(dict(index=index, name=image["name"], format=FORMAT_NAMES[image["format"]],
                         width=image["width"], height=image["height"], mips=mips,
                         complete_chain=mips[-1]["width"] == 1 and mips[-1]["height"] == 1,
                         owned_pixel_bytes=image["owned_bytes"], gpu_mip_bytes=sum(len(m) for m in image["mips"])))
    return dict(images=rows, bc7_images=sum(image["format"] in (13, 14) for image in images.values()),
                bc4_images=sum(image["format"] == 7 for image in images.values()),
                bc5_images=sum(image["format"] == 9 for image in images.values()),
                rgba_images=sum(image["format"] in (3, 4) for image in images.values()),
                unique_image_owned_bytes=sum(row["owned_pixel_bytes"] for row in rows),
                unique_image_gpu_mip_bytes=sum(row["gpu_mip_bytes"] for row in rows))


def rgba(image, level):
    width, height = max(1, image["width"] >> level), max(1, image["height"] >> level)
    data = image["mips"][level]
    if image["format"] in (13, 14):
        # Pillow's decoder requires block-padded dimensions; discard only padded edge texels.
        padded_width, padded_height = (width + 3) // 4 * 4, (height + 3) // 4 * 4
        decoded = Image.frombytes("RGBA", (padded_width, padded_height), data, "bcn", (7, "BC7"))
        return np.asarray(decoded)[:height, :width].copy()
    return np.frombuffer(data, dtype=np.uint8).reshape(height, width, 4)


def linear_rgb(values, srgb):
    rgb = values[..., :3].astype(np.float64) / 255
    return np.where(rgb <= 0.04045, rgb / 12.92, ((rgb + 0.055) / 1.055) ** 2.4) if srgb else rgb


def scene_samplers(resources):
    result = {}
    for (kind, index), data in resources.items():
        if kind == 15:
            fields = Fields(data)
            fields.string()
            minimum, maximum, mip = fields.unpack("3I")
            use_mips = bool(fields.unpack("B"))
            wrap_s, wrap_t = fields.unpack("2I")
            fields.finish()
            result[index] = dict(linear=minimum != 0 or maximum != 0, min_linear=minimum != 0, mip_linear=mip != 0,
                                 repeat_s=wrap_s in (0, INVALID), repeat_t=wrap_t in (0, INVALID),
                                 border=wrap_s == 3 or wrap_t == 3, use_mips=use_mips)
    return result


def filtered_normal_quality(expected, decoded, xy, srgb, sampler):
    """Independent bilinear-channel sampling, then shader-style normal reconstruction."""
    result = dict(filtered_normal_rms_degrees=0.0, filtered_normal_max_degrees=0.0,
                  filtered_normal_phases=0)
    if sampler.get("linear", True):
        left = linear_rgb(expected, srgb)
        right = decoded if xy else linear_rgb(decoded, srgb)
        columns = np.arange(left.shape[1]) + 1
        rows = np.arange(left.shape[0]) + 1
        columns[-1] = 0 if sampler.get("repeat_s", True) else left.shape[1]-1
        rows[-1] = 0 if sampler.get("repeat_t", True) else left.shape[0]-1
        for fx in (0.0, .25, .5, .75):
            for fy in (0.0, .25, .5, .75):
                if fx == fy == 0:
                    continue
                samples = []
                for image in (left, right):
                    top = image*(1-fx) + image[:,columns]*fx
                    bottom = image[rows]*(1-fx) + image[rows][:,columns]*fx
                    samples.append((top*(1-fy) + bottom*fy)*2-1)
                a,b = samples
                if xy:
                    b = np.concatenate((b, np.sqrt(np.maximum(0,1-np.sum(b*b,axis=2)))[...,None]),axis=2)
                la,lb = np.linalg.norm(a,axis=2),np.linalg.norm(b,axis=2)
                defined = (la > 1e-12) & (lb > 1e-12)
                cosine = np.divide(np.sum(a*b,axis=2),la*lb,out=np.zeros_like(la),where=defined)
                angles = np.where(defined,np.degrees(np.arccos(np.clip(cosine,-1,1))),180.0)
                angles[np.all(a==b,axis=2) & defined] = 0
                result["filtered_normal_rms_degrees"] = max(result["filtered_normal_rms_degrees"],float(np.sqrt(np.mean(angles*angles))))
                result["filtered_normal_max_degrees"] = max(result["filtered_normal_max_degrees"],float(np.max(angles)))
                result["filtered_normal_phases"] += 1
    return result


def mip_quality(expected, actual, use, srgb, limits, base_coverage, sampler=None):
    left, right = linear_rgb(expected, srgb), linear_rgb(actual, srgb)
    squared = np.square(left - right)
    rmse = float(np.sqrt(np.mean(squared)))
    alpha = float(np.max(np.abs(expected[..., 3].astype(np.int16) - actual[..., 3].astype(np.int16)))) / 255
    row = dict(linear_rgb_rmse=rmse, linear_rgb_psnr_db=-20 * math.log10(rmse) if rmse else None,
               scalar_max_error=max(alpha, float(np.max(np.abs(left - right)))) if use["role"] == "data" else alpha,
               exact_alpha=bool(np.array_equal(expected[..., 3], actual[..., 3])))
    passed = row["scalar_max_error"] <= limits["scalar_channel_max_absolute_error_unit_range"] + 1e-12
    if use["role"] == "color":
        passed &= rmse <= limits["linear_rgb_rmse_max_unit_range"] and (rmse == 0 or row["linear_rgb_psnr_db"] >= limits["linear_rgb_psnr_min_db"])
    if use["role"] == "normal":
        left, right = left * 2 - 1, right * 2 - 1
        length_left, length_right = np.linalg.norm(left, axis=2), np.linalg.norm(right, axis=2)
        if np.any(length_left == 0) or np.any(length_right == 0):
            angle = np.full(length_left.shape, 180.0)
        else:
            cosine = np.sum(left * right, axis=2) / (length_left * length_right)
            angle = np.degrees(np.arccos(np.clip(cosine, -1, 1)))
            angle[np.all(left == right, axis=2)] = 0
        row.update(normal_rms_degrees=float(np.sqrt(np.mean(angle * angle))), normal_max_degrees=float(np.max(angle)))
        passed &= row["normal_rms_degrees"] <= limits["normal_rms_angular_error_max_degrees"] and row["normal_max_degrees"] <= limits["normal_max_angular_error_degrees"]
        row.update(filtered_normal_quality(expected, actual, False, srgb, sampler or {}))
        passed &= row["filtered_normal_rms_degrees"] <= limits["normal_rms_angular_error_max_degrees"] and row["filtered_normal_max_degrees"] <= limits["normal_max_angular_error_degrees"]
    if use["alpha_mask"]:
        coverage = float(np.mean(actual[..., 3] / 255 >= use["cutoff"]))
        expected_coverage = float(np.mean(expected[..., 3] / 255 >= use["cutoff"]))
        row.update(alpha_cutoff=use["cutoff"], alpha_coverage=coverage,
                   alpha_coverage_encoding_error=abs(coverage - expected_coverage),
                   alpha_coverage_base_error=abs(coverage - base_coverage))
        passed &= row["alpha_coverage_encoding_error"] <= limits["alpha_coverage_max_absolute_fraction_error_per_mip"]
        passed &= row["alpha_coverage_base_error"] <= limits["alpha_coverage_max_absolute_fraction_error_per_mip"]
    if use["exact_alpha"]:
        passed &= row["exact_alpha"]
    row["passed"] = bool(passed)
    return row


def audit(reference_path, mip_path, bc7_path, limits):
    reference, mip_resources, cooked = package(reference_path), package(mip_path), package(bc7_path)
    before, before_textures, before_bindings = scene_images(reference)
    expected, expected_textures, expected_bindings = scene_images(mip_resources)
    images, textures, bindings = scene_images(cooked)
    samplers = scene_samplers(cooked)
    unchanged = all(data == cooked.get(key) for key, data in mip_resources.items() if key[0] not in (1, 4, 9, 13))
    if bindings != expected_bindings or set(textures) != set(expected_textures):
        raise ValueError("Mip-only and BC7 processing changed binding identities differently")
    for index, texture in textures.items():
        if {k: v for k, v in texture.items() if k not in ("image", "format")} != {k: v for k, v in expected_textures[index].items() if k not in ("image", "format")}:
            raise ValueError("BC7 changed texture descriptor semantics")
    quality, retained, seen = [], [], set()
    for binding, use in bindings.items():
        texture = textures[use["texture"]]
        image = images[texture["image"]]
        original_texture = before_textures[before_bindings[binding]["texture"]]
        original = before[original_texture["image"]]
        if image["format"] not in (13, 14):
            same = image["base"] == original["base"] and image["mips"] == original["mips"]
            retained.append(dict(material=binding[0], slot=binding[1], image=texture["image"], original_pixels_retained=same))
        else:
            key = (texture["image"], use["role"], use["alpha_mask"], use["exact_alpha"], use["cutoff"], texture["format"], texture["sampler"])
            if key in seen:
                continue
            seen.add(key)
            control = expected[expected_textures[use["texture"]]["image"]]
            if len(image["mips"]) != len(control["mips"]):
                raise ValueError("Accepted compressed image changed the generated mip count")
            base_coverage = float(np.mean(rgba(control, 0)[..., 3] / 255 >= use["cutoff"]))
            rows = [mip_quality(rgba(control, level), rgba(image, level), use, texture["format"] == 14, limits, base_coverage,
                                samplers.get(texture["sampler"], {}))
                    for level in range(len(image["mips"]))]
            if use["role"] == "normal":
                srgb = texture["format"] == 14
                for level in range(1, len(image["mips"])):
                    measured = mip_pair_quality(linear_rgb(rgba(control, level - 1), srgb), linear_rgb(rgba(control, level), srgb),
                                                linear_rgb(rgba(image, level - 1), srgb), linear_rgb(rgba(image, level), srgb),
                                                False, samplers.get(texture["sampler"], {}))
                    rows[level].update(measured)
                    rows[level]["passed"] &= measured["mip_normal_rms_degrees"] <= limits["normal_rms_angular_error_max_degrees"] and measured["mip_normal_max_degrees"] <= limits["normal_max_angular_error_degrees"]
            quality.append(dict(image=texture["image"], texture=use["texture"], role=use["role"],
                                material=binding[0], slot=binding[1], mips=rows))
    summary = image_summary(images)
    summary.update(reference=image_summary(before), mip_control=image_summary(expected), quality=quality,
                   fallback_bindings=retained, non_texture_payloads_unchanged=unchanged,
                   passed=unchanged and all(row["passed"] for image in quality for row in image["mips"])
                   and all(row["original_pixels_retained"] for row in retained))
    return summary


def shared_fixture(directory):
    directory.mkdir()
    pixels = np.zeros((64, 64, 4), dtype=np.uint8)
    pixels[:] = [128, 128, 255, 255]
    Image.fromarray(pixels).save(directory / "shared.png")
    positions, normals, uv, indices = [], [], [], []
    for panel in range(3):
        start = len(positions)
        center = (panel - 1) * 2.2
        positions += [[center - 1, -1, 0], [center + 1, -1, 0], [center + 1, 1, 0], [center - 1, 1, 0]]
        normals += [[0, 0, 1]] * 4
        uv += [[0, 0], [1, 0], [1, 1], [0, 1]]
        indices += [start, start + 1, start + 2, start, start + 2, start + 3]
    arrays = [np.asarray(positions, dtype="<f4").tobytes(), np.asarray(normals, dtype="<f4").tobytes(),
              np.asarray(uv, dtype="<f4").tobytes(), np.asarray(indices, dtype="<u2").tobytes()]
    data = b"".join(arrays)
    views, offset = [], 0
    for value in arrays:
        views.append(dict(buffer=0, byteOffset=offset, byteLength=len(value)))
        offset += len(value)
    accessors = [dict(bufferView=0, componentType=5126, count=12, type="VEC3", min=[-3.2, -1, 0], max=[3.2, 1, 0]),
                 dict(bufferView=1, componentType=5126, count=12, type="VEC3"),
                 dict(bufferView=2, componentType=5126, count=12, type="VEC2")]
    accessors += [dict(bufferView=3, byteOffset=panel * 12, componentType=5123, count=6, type="SCALAR") for panel in range(3)]
    materials = [dict(name="shared color", pbrMetallicRoughness=dict(baseColorTexture=dict(index=0), metallicFactor=0)),
                 dict(name="shared scalar", pbrMetallicRoughness=dict(metallicRoughnessTexture=dict(index=0))),
                 dict(name="shared normal", normalTexture=dict(index=0), pbrMetallicRoughness=dict(metallicFactor=0))]
    document = dict(asset=dict(version="2.0"), scene=0, scenes=[dict(nodes=[0])], nodes=[dict(mesh=0)],
                    meshes=[dict(primitives=[dict(attributes=dict(POSITION=0, NORMAL=1, TEXCOORD_0=2), indices=panel + 3, material=panel)
                                             for panel in range(3)])], materials=materials,
                    textures=[dict(source=0, sampler=0)], images=[dict(uri="shared.png")],
                    samplers=[dict(minFilter=9987, magFilter=9729)], accessors=accessors, bufferViews=views,
                    buffers=[dict(uri="data:application/octet-stream;base64," + base64.b64encode(data).decode(), byteLength=len(data))])
    path = directory / "shared-role.gltf"
    path.write_text(json.dumps(document), encoding="utf-8")
    return path


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--processor", type=Path, required=True)
    parser.add_argument("--reference-dir", type=Path, required=True)
    parser.add_argument("--assets", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    for name in ("processor", "reference_dir", "assets", "output"):
        setattr(args, name, getattr(args, name).resolve())
    args.output.mkdir(parents=True, exist_ok=False)
    cases = []
    for name in visual.CASES:
        settings = (args.reference_dir / f"{name}-source.cfg").read_bytes()
        source = Path([line.split("=", 1)[1].strip() for line in settings.decode("utf-8-sig").splitlines()
                       if line.split("=", 1)[0].strip() == "default_model_path"][-1])
        cases.append((name, source, settings, True))
    focused = [("alpha", args.assets / "Models/AlphaBlendModeTest/glTF/AlphaBlendModeTest.gltf"),
               ("normal", args.assets / "Models/NormalTangentTest/glTF/NormalTangentTest.gltf"),
               ("clearcoat", args.assets / "Models/ClearCoatTest/glTF/ClearCoatTest.gltf"),
               ("shared-role", shared_fixture(args.output / "shared-fixture"))]
    template = (args.reference_dir / "visual-0-source.cfg").read_bytes()
    cases += [(name, path, smoke.configuration_for_asset(template, path), False) for name, path in focused]
    policy = json.loads(POLICY.read_text())
    report = dict(processor=baseline.identity(args.processor), policy=baseline.identity(POLICY),
                  decoder=f"Pillow {PIL.__version__} BC7 decoder", script=baseline.identity(Path(__file__)), results=[])
    baseline.capture_dependency_manifest([source for _, source, _, _ in cases if source.suffix.lower() in (".gltf", ".glb")],
                                         args.output / "dependencies.json")
    for name, source, settings, frozen in cases:
        (args.output / f"{name}.cfg").write_bytes(settings)
        row = dict(case=name, source=baseline.identity(source), configuration=baseline.identity(args.output / f"{name}.cfg"),
                   frozen_reference=frozen, packages={}, runs={})
        for profile, flags in (("reference", []), ("mips", ["--profile", "desktop", "--texture-mipmaps", "on"]),
                               ("bc7", ["--profile", "desktop", "--texture-mipmaps", "on", "--texture-compression", "bc7"])):
            path = args.output / f"{name}-{profile}.zenasset"
            row["runs"][profile] = baseline.run([args.processor, "cook", source, "--output", path, "--workers", "2"] + flags,
                                                args.output / f"{name}-{profile}-cook.log", ROOT, 600)
            if row["runs"][profile]["exit_code"] != 0:
                raise RuntimeError(f"Cook failed: {name}/{profile}")
            validation = baseline.run([args.processor, "validate", path], args.output / f"{name}-{profile}-validate.log", ROOT)
            if validation["exit_code"] != 0:
                raise RuntimeError(f"Package failed validation: {name}/{profile}")
            row["packages"][profile] = baseline.identity(path)
        row["audit"] = audit(*(Path(row["packages"][key]["path"]) for key in ("reference", "mips", "bc7")),
                             policy["desktop_profile"]["texture_encoding"])
        row["passed"] = row["audit"]["passed"]
        report["results"].append(row)
        baseline.write(args.output / "results.json", report)
        print(f"{name}: {'PASS' if row['passed'] else 'FAIL'}, BC7={row['audit']['bc7_images']} RGBA={row['audit']['rgba_images']}", flush=True)
    report["processor_unchanged"] = report["processor"] == baseline.identity(args.processor)
    report["passed"] = report["processor_unchanged"] and all(row["passed"] for row in report["results"]) and sum(row["audit"]["bc7_images"] for row in report["results"]) > 0
    baseline.write(args.output / "results.json", report)
    return 0 if report["passed"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
