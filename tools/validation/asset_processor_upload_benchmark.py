"""Measure isolated native graphics-queue uploads, not production async staging.

Three fresh processes/devices and five warm fresh-resource iterations are measured
per package. OS file caches are neither flushed nor controlled. Validation runs
are separate. VMA suballocations, backing-block commitment, and payload bytes are
distinct fields; they must not be summed or called physical resident VRAM.
"""
import argparse
import base64
import json
import math
from pathlib import Path
from asset_processor_paths import ENGINE_ROOT
import shutil
import statistics
import struct
import zlib

import asset_processor_baseline as baseline

ROOT = ENGINE_ROOT


def write(path, value):
    path.write_text(json.dumps(value, indent=2) + "\n", encoding="utf-8")


def png_chunk(kind, data):
    return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data))


def fixture(directory, role, size=1024):
    """A single consumed role avoids role-split allocation confounds."""
    directory.mkdir()
    pixels = bytearray()
    for y in range(size):
        pixels.append(0)
        for x in range(size):
            if role == "normal":
                rgba = [128, 128, 255, 255]
            elif role == "scalar":
                rgba = [192, 0, 0, 255]
            else:
                rgba = [128, 64, 32, 255]
            pixels.extend(rgba)
    png = b"\x89PNG\r\n\x1a\n"
    png += png_chunk(b"IHDR", struct.pack(">IIBBBBB", size, size, 8, 6, 0, 0, 0))
    png += png_chunk(b"IDAT", zlib.compress(pixels)) + png_chunk(b"IEND", b"")
    (directory / "image.png").write_bytes(png)
    geometry = struct.pack("<9f9f6f3H", -1, -1, 0, 1, -1, 0, 0, 1, 0,
                           0, 0, 1, 0, 0, 1, 0, 0, 1, 0, 0, 1, 0, .5, 1, 0, 1, 2)
    material = dict(pbrMetallicRoughness=dict(metallicFactor=0))
    if role == "normal":
        material["normalTexture"] = dict(index=0, scale=1)
    elif role == "scalar":
        material["occlusionTexture"] = dict(index=0)
    else:
        material["pbrMetallicRoughness"]["baseColorTexture"] = dict(index=0)
    document = dict(asset=dict(version="2.0"), scene=0, scenes=[dict(nodes=[0])], nodes=[dict(mesh=0)],
        meshes=[dict(primitives=[dict(attributes=dict(POSITION=0, NORMAL=1, TEXCOORD_0=2), indices=3, material=0)])],
        materials=[material], textures=[dict(source=0, sampler=0)], images=[dict(uri="image.png")],
        samplers=[dict(minFilter=9987, magFilter=9729)],
        bufferViews=[dict(buffer=0, byteOffset=o, byteLength=n) for o, n in ((0, 36), (36, 36), (72, 24), (96, 6))],
        accessors=[dict(bufferView=0, componentType=5126, count=3, type="VEC3", min=[-1, -1, 0], max=[1, 1, 0]),
                   dict(bufferView=1, componentType=5126, count=3, type="VEC3"),
                   dict(bufferView=2, componentType=5126, count=3, type="VEC2"),
                   dict(bufferView=3, componentType=5123, count=3, type="SCALAR")],
        buffers=[dict(uri="data:application/octet-stream;base64," + base64.b64encode(geometry).decode(), byteLength=len(geometry))])
    source = directory / "fixture.gltf"
    write(source, document)
    return source


def read_rows(path):
    result = {"run": [], "resource": [], "sample": []}
    for line in path.read_text(encoding="utf-8").splitlines():
        prefix, data = line.split(" ", 1)
        kind = prefix.removeprefix("ZEN_UPLOAD_").lower()
        result[kind].append(json.loads(data))
    assert len(result["run"]) == 1
    return result


def resource_shape(rows):
    return [(r["kind"], r.get("index"), r.get("width"), r.get("height"), r.get("mips"), r.get("layers"))
            for r in rows if "format" in r]


def check_accounting(rows):
    for sample in rows["sample"]:
        resources = [r for r in rows["resource"] if r["iteration"] == sample["iteration"]]
        textures = [r for r in resources if "format" in r]
        geometry = [r for r in resources if "format" not in r]
        assert len(textures) == sample["texture_count"]
        for prefix, values in (("texture", textures), ("geometry", geometry)):
            for field in ("payload_bytes", "allocation_bytes"):
                assert sum(r[field] for r in values) == sample[f"{prefix}_{field}"]
        assert sum(r["staging_allocation_bytes"] for r in resources) == sample["explicit_staging_peak_allocation_bytes"]
        assert sample["validation_errors"] == 0
        assert all(math.isfinite(sample[k]) and sample[k] >= 0 for k in
                   ("graphics_texture_upload_gpu_us", "graphics_geometry_upload_gpu_us"))


def invoke(exe, package, path, iterations, warmup, validation):
    command = [exe, package, f"--iterations={iterations}", f"--warmup={warmup}",
               f"--validation={int(validation)}", f"--output={path.with_suffix('.jsonl')}"]
    run = baseline.run(command, path.with_suffix(".log"), ROOT, timeout=300)
    assert run["exit_code"] == 0, run
    text = path.with_suffix(".log").read_text(encoding="utf-8", errors="replace")
    assert "Vulkan validation:" not in text and "VUID-" not in text
    assert "VMA Total device memory leaked: 0 bytes" in text and "[OK] No memory leaks detected" in text
    rows = read_rows(path.with_suffix(".jsonl"))
    check_accounting(rows)
    assert sum(not r["warmup"] for r in rows["sample"]) == iterations
    return dict(process=run, **rows)


def summarize(runs):
    samples = [s for run in runs for s in run["sample"] if not s["warmup"]]
    result = dict(samples=len(samples))
    for key, value in samples[0].items():
        if isinstance(value, (int, float)) and key not in ("iteration", "warmup"):
            values = [s[key] for s in samples]
            result[key] = dict(min=min(values), median=statistics.median(values), max=max(values))
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--benchmark", type=Path, required=True)
    parser.add_argument("--processor", type=Path, required=True)
    parser.add_argument("--hdr-source", type=Path, required=True)
    parser.add_argument("--role-split-source", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    for name in vars(args):
        setattr(args, name, getattr(args, name).resolve())
    args.output.mkdir(parents=True, exist_ok=False)
    binaries = args.output / "binaries"
    binaries.mkdir()
    benchmark = binaries / args.benchmark.name
    shutil.copy2(args.benchmark, benchmark)
    for dll in args.benchmark.parent.glob("*.dll"):
        shutil.copy2(dll, binaries / dll.name)
    frozen_script = args.output / Path(__file__).name
    shutil.copy2(Path(__file__), frozen_script)
    report = dict(method=__doc__, benchmark=baseline.identity(benchmark), processor=baseline.identity(args.processor),
        script=baseline.identity(frozen_script), process_sampler=baseline.identity(Path(baseline.__file__)), cases=[], passed=False,
        cold_definition="fresh process/device; OS file cache and driver caches uncontrolled",
        warm_definition="one unreported warmup then five new-resource iterations in one process/device",
        allocation_definition="VMA allocation info.size, including per-resource alignment; not physical residency or driver-private allocation",
        staging_definition="one explicit coherent host-write staging buffer per destination, all live at upload peak; not production pooled staging",
        gpu_definition="separate graphics-queue timestamp scopes: texture copies+transitions and canonical geometry copies+barrier; each submitted/completed independently")
    cases = [dict(name="controlled-bc7", source=fixture(args.output / "color-source", "color"), flag="--texture-compression", value="bc7", formats=[145, 146]),
             dict(name="controlled-bc4", source=fixture(args.output / "scalar-source", "scalar"), flag="--occlusion-bc4", value="on", formats=[139]),
             dict(name="controlled-bc5", source=fixture(args.output / "normal-source", "normal"), flag="--normal-bc5", value="on", formats=[141]),
             dict(name="environment-bc6h", source=args.hdr_source, flag="--environment-bc6h", value="on", formats=[143]),
             dict(name="real-bc4-role-split", source=args.role_split_source, flag="--occlusion-bc4", value="on", formats=[139])]
    baseline.capture_dependency_manifest([c["source"] for c in cases], args.output / "source-dependencies.json")
    for case in cases:
        directory = args.output / case["name"]
        directory.mkdir()
        result = dict(name=case["name"], source=baseline.identity(case["source"]), variants={})
        for variant in ("raw", "cooked"):
            package = directory / f"{variant}.zenasset"
            command = [args.processor, "cook", case["source"], "--output", package, "--profile", "desktop",
                       "--compression", "none", "--texture-mipmaps", "on"]
            if variant == "cooked":
                command += [case["flag"], case["value"]]
            cooked = baseline.run(command, directory / f"{variant}-cook.log", ROOT, timeout=600)
            assert cooked["exit_code"] == 0, cooked
            validation = invoke(benchmark, package, directory / f"{variant}-validation", 1, 0, True)
            result["variants"][variant] = dict(package=baseline.identity(package), cook=cooked, validation=validation, cold=[], warm=[])
        raw_resources = result["variants"]["raw"]["validation"]["resource"]
        cooked_resources = result["variants"]["cooked"]["validation"]["resource"]
        result["matching_resource_shapes"] = resource_shape(raw_resources) == resource_shape(cooked_resources)
        result["matching_canonical_geometry"] = [
            (r["kind"], r["payload_bytes"], r["allocation_bytes"])
            for r in raw_resources if "format" not in r] == [
            (r["kind"], r["payload_bytes"], r["allocation_bytes"])
            for r in cooked_resources if "format" not in r]
        assert result["matching_canonical_geometry"], case["name"]
        if case["name"] != "real-bc4-role-split":
            assert result["matching_resource_shapes"], case["name"]
        assert any(r.get("format") in case["formats"] for r in cooked_resources), "Compression fell back: " + case["name"]
        # Alternate raw/cooked fresh processes to reduce order bias; each run has its own device.
        for index in range(3):
            for variant in (("raw", "cooked") if index % 2 == 0 else ("cooked", "raw")):
                entry = result["variants"][variant]
                entry["cold"].append(invoke(benchmark, entry["package"]["path"], directory / f"{variant}-cold-{index}", 1, 0, False))
        for variant in ("raw", "cooked"):
            entry = result["variants"][variant]
            entry["warm"].append(invoke(benchmark, entry["package"]["path"], directory / f"{variant}-warm", 5, 1, False))
            entry["summary"] = dict(cold=summarize(entry["cold"]), warm=summarize(entry["warm"]))
        report["cases"].append(result)
        write(args.output / "results.json", report)
        print(case["name"], "passed", flush=True)
    report["benchmark_unchanged"] = baseline.identity(benchmark)["sha256"] == report["benchmark"]["sha256"]
    report["processor_unchanged"] = baseline.identity(args.processor)["sha256"] == report["processor"]["sha256"]
    report["passed"] = report["benchmark_unchanged"] and report["processor_unchanged"]
    write(args.output / "results.json", report)


if __name__ == "__main__":
    main()
