"""Compare isolated uploads with array packing off/on; no async/frame-time claim.

Uses matched finalized mips, fixed raw/BC profiles and the original material
fixture at 128 and 1024 pixels. Its repeating material is opaque so alpha-coverage
fallback does not require unmeasured runtime mip blits. Three fresh devices and
three warm processes (five fresh-resource samples after one warmup each) per
package alternate off/on order. OS/driver caches remain uncontrolled.
"""
import argparse
import importlib.util
import json
import math
from pathlib import Path
from asset_processor_paths import PROCESSOR_ROOT, ENGINE_ROOT, INTEGRATION_ROOT
import shutil

import asset_processor_baseline as baseline
import asset_processor_upload_benchmark as upload

ROOT = ENGINE_ROOT


def write(path, value):
    path.write_text(json.dumps(value, indent=2) + "\n", encoding="utf-8")


def check_array_accounting(run, packed, size):
    levels = size.bit_length()
    for sample in run["sample"]:
        textures = [r for r in run["resource"] if r["iteration"] == sample["iteration"] and "format" in r]
        assert sample["texture_count"] == (9 if packed else 12)
        assert sample["native_view_count"] == (15 if packed else 12)
        assert sample["copy_region_count"] == (4 if packed else 7) * levels + 5
        assert sum(r["native_view_count"] for r in textures) == sample["native_view_count"]
        assert sum(r["copy_region_count"] for r in textures) == sample["copy_region_count"]
        assert sum(r["layers"] > 1 for r in textures) == (3 if packed else 0)
        assert all(r["layers"] in (1, 2) for r in textures)
        assert all(r["device_local"] for r in textures)


def summarize(runs):
    result = upload.summarize(runs)
    samples = [s for run in runs for s in run["sample"] if not s["warmup"]]
    for key in result:
        if key != "samples":
            values = sorted(s[key] for s in samples)
            result[key]["p95_nearest_rank"] = values[math.ceil(len(values) * .95) - 1]
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--benchmark", type=Path, required=True)
    parser.add_argument("--processor", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    for name in vars(args):
        setattr(args, name, getattr(args, name).resolve())
    args.output.mkdir(parents=True, exist_ok=False)
    binaries = args.output / "binaries"
    binaries.mkdir()
    benchmark, processor = binaries / args.benchmark.name, binaries / args.processor.name
    for source, target in ((args.benchmark, benchmark), (args.processor, processor)):
        shutil.copy2(source, target)
        for dll in source.parent.glob("*.dll"):
            shutil.copy2(dll, binaries / dll.name)
    generator = INTEGRATION_ROOT / "Editor/Fixtures/generate_array_scene.py"
    spec = importlib.util.spec_from_file_location("array_fixture", generator)
    fixture = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(fixture)
    report = dict(method=__doc__, benchmark=baseline.identity(benchmark), processor=baseline.identity(processor),
                  sources=[], cases=[], passed=False)
    for source in (Path(__file__), Path(upload.__file__), Path(baseline.__file__), generator,
                   INTEGRATION_ROOT / "CommonTest/SceneUploadBenchmark.cpp"):
        target = args.output / "sources" / source.relative_to(PROCESSOR_ROOT)
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(source, target)
        report["sources"].append(baseline.identity(target))
    write(args.output / "results.json", report)
    for size in (128, 1024):
        directory = args.output / str(size)
        directory.mkdir()
        source = directory / "scene.gltf"
        fixture.generate(size, source, alpha_mask=False)
        for profile in ("raw", "bc"):
            case = dict(size=size, profile=profile, source=baseline.identity(source), variants={})
            for variant in ("off", "array"):
                package = directory / f"{profile}-{variant}.zenasset"
                command = [processor, "cook", source, "--output", package, "--profile", "desktop",
                           "--texture-mipmaps", "on", "--atlas", variant]
                if profile == "bc":
                    command += ["--texture-compression", "bc7", "--normal-bc5", "on", "--occlusion-bc4", "on"]
                cook = baseline.run(command, directory / f"{profile}-{variant}-cook.log", ROOT)
                assert cook["exit_code"] == 0, cook
                validation = upload.invoke(benchmark, package, directory / f"{profile}-{variant}-validation", 1, 0, True)
                check_array_accounting(validation, variant == "array", size)
                formats = {r["format"] for r in validation["resource"] if r["kind"] == "scene_texture"}
                assert ({139, 141} <= formats and bool({145, 146} & formats)) if profile == "bc" else formats <= {37, 43}
                case["variants"][variant] = dict(package=baseline.identity(package), cook=cook,
                                                  validation=validation, cold=[], warm=[])
            left = case["variants"]["off"]["validation"]["sample"][0]
            right = case["variants"]["array"]["validation"]["sample"][0]
            for field in ("texture_payload_bytes", "geometry_payload_bytes", "geometry_allocation_bytes"):
                assert left[field] == right[field], (size, profile, field)
            for repeat in range(3):
                for variant in (("off", "array") if repeat % 2 == 0 else ("array", "off")):
                    entry = case["variants"][variant]
                    for kind, iterations, warmup in (("cold", 1, 0), ("warm", 5, 1)):
                        run = upload.invoke(benchmark, Path(entry["package"]["path"]),
                                            directory / f"{profile}-{variant}-{kind}-{repeat}", iterations, warmup, False)
                        check_array_accounting(run, variant == "array", size)
                        entry[kind].append(run)
            for entry in case["variants"].values():
                entry["summary"] = {kind: summarize(entry[kind]) for kind in ("cold", "warm")}
            report["cases"].append(case)
            write(args.output / "results.json", report)
            print(f"{size} {profile}: validated; 3 fresh and 15 warm samples per layout", flush=True)
    report["passed"] = all(baseline.identity(path)["sha256"] == report[key]["sha256"]
                           for path, key in ((benchmark, "benchmark"), (processor, "processor")))
    assert report["passed"]
    write(args.output / "results.json", report)


if __name__ == "__main__":
    main()
