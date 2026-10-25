"""Render audited texture packages with exact reference and descriptive lossy metrics.

No desktop frame threshold exists in asset_processor_quality.json. Completion
requires clean captures, exact source/reference agreement, passing independent
image/mip gates and stable inputs. Lossy frame metrics require separate review.
"""
import argparse
import hashlib
import json
import math
import os
from pathlib import Path
from asset_processor_paths import ENGINE_ROOT

import numpy as np
from PIL import Image

import asset_processor_baseline as baseline
import asset_processor_visual_parity as visual

ROOT = ENGINE_ROOT


def shader_snapshot():
    return [baseline.identity(path) for directory in (ROOT / "Data/Shaders", ROOT / "Data/SpvShaders")
            for path in sorted(directory.rglob("*")) if path.is_file()]


def measure(first, second, difference_path):
    result = visual.compare(first, second)
    width, height, left = visual.pixels(first)
    _, _, right = visual.pixels(second)
    a = np.frombuffer(left, dtype=np.uint8).reshape(height, width, 3)
    b = np.frombuffer(right, dtype=np.uint8).reshape(height, width, 3)
    delta = np.abs(a.astype(np.int16) - b.astype(np.int16))
    result["different_pixel_fraction"] = float(np.mean(np.any(delta != 0, axis=2)))
    result["pixels_over_1_fraction"] = float(np.mean(np.max(delta, axis=2) > 1))
    result["pixels_over_8_fraction"] = float(np.mean(np.max(delta, axis=2) > 8))
    result["psnr_db"] = 20 * math.log10(255 / result["rmse_8bit"]) if result["rmse_8bit"] else None
    Image.fromarray(np.minimum(delta * 8, 255).astype(np.uint8)).save(difference_path)
    result["difference_8x"] = str(difference_path)
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--renderer", type=Path, required=True)
    parser.add_argument("--cpu-report", type=Path, required=True)
    parser.add_argument("--reference-dir", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--focused-front", action="store_true", help="Only focused scenes, explicit normalized front camera")
    parser.add_argument("--repeat-gi-reference", action="store_true", help="Capture each raw GI package twice to test repeatability")
    parser.add_argument("--deterministic-gi-capture", action="store_true", help="Use exact learned-guide sums in a compatible renderer; costs extra GPU time")
    parser.add_argument("--compression-profile", choices=("bc7", "bc45"), default="bc7")
    parser.add_argument("--gi-cases", nargs="*", default=["alpha", "shared-role"])
    args = parser.parse_args()
    for name in ("renderer", "cpu_report", "reference_dir", "output"):
        setattr(args, name, getattr(args, name).resolve())
    cpu = json.loads(args.cpu_report.read_text())
    if not cpu["passed"]:
        raise ValueError("Successful independent texture audit is required")
    args.output.mkdir(parents=True, exist_ok=False)
    config = ROOT / "Data/engine.cfg"
    lock = config.with_name(config.name + ".gltf-smoke.lock")
    descriptor = os.open(lock, os.O_CREAT | os.O_EXCL | os.O_WRONLY)
    os.close(descriptor)
    original = config.read_bytes() if config.exists() else None
    last = original
    if original is not None:
        (args.output / "original-engine.cfg").write_bytes(original)
    os.environ.update(VK_LAYER_VALIDATE_SYNC="1", VK_LOADER_LAYERS_DISABLE="~implicit~", DISABLE_RTSS_LAYER="1")
    report = dict(renderer=baseline.identity(args.renderer), cpu_report=baseline.identity(args.cpu_report),
                  script=baseline.identity(Path(__file__)), policy=cpu["policy"], shaders=shader_snapshot(),
                  acceptance="Exact source/raw/frozen reference; mandatory independent per-image/mip limits; lossy frame deltas are descriptive and require visual review.",
                  compression_profile=args.compression_profile, deterministic_gi_capture=args.deterministic_gi_capture,
                  lossy_frame_threshold=None, visual_review="pending", results=[])
    runs = [(case, "raster") for case in cpu["results"] if not args.focused_front or not case["frozen_reference"]]
    runs += [(case, backend) for case in cpu["results"] if case["case"] in args.gi_cases
             for backend in ("compute", "hardware")]
    try:
        for case, backend in runs:
            name = f"{case['case']}-{backend}"
            source = Path(case["source"]["path"])
            settings_path = Path(case["configuration"]["path"])
            if baseline.identity(source) != case["source"] or baseline.identity(settings_path) != case["configuration"]:
                raise ValueError("Source or capture configuration changed after CPU audit")
            settings = settings_path.read_bytes()
            if args.focused_front:
                settings += b"\ncamera_position=0,0,1.2\n"
            if backend != "raster":
                settings += (f"\nvoxel_gi_backend={backend}\nvoxel_gi_ray_provider={'hardware' if backend == 'hardware' else 'voxel'}\n").encode()
            row = dict(case=case["case"], backend=backend, captures=[])
            paths = [("source", source)] + [(profile, Path(case["packages"][profile]["path"]))
                                            for profile in ("reference", "mips", args.compression_profile)]
            if backend != "raster" and args.repeat_gi_reference:
                paths.append(("reference-repeat", Path(case["packages"]["reference"]["path"])))
            for profile, path in paths:
                package_profile = "reference" if profile == "reference-repeat" else profile
                if profile != "source" and baseline.identity(path) != case["packages"][package_profile]:
                    raise ValueError("Audited package changed")
                if (config.read_bytes() if config.exists() else None) != last:
                    raise RuntimeError("Configuration changed externally; refusing to overwrite it")
                last = settings + f"\ndefault_model_path={path.as_posix()}\n".encode()
                row["captures"].append(visual.run_capture(args, last, f"{name}-{profile}", config,
                                                         mode=2 if backend == "raster" else 3,
                                                         frames=3 if backend == "raster" else 16,
                                                         disable_rt=backend != "hardware"))
            if all(capture["passed"] for capture in row["captures"]):
                source_image = args.output / f"{name}-source.ppm"
                reference_image = args.output / f"{name}-reference.ppm"
                cooked_image = args.output / f"{name}-{args.compression_profile}.ppm"
                row["source_reference"] = visual.compare(source_image, reference_image)
                row["reference_gate_passed"] = row["source_reference"]["identical"] and row["source_reference"]["visible"]
                if backend != "raster" and args.repeat_gi_reference:
                    row["reference_repeat"] = visual.compare(reference_image, args.output / f"{name}-reference-repeat.ppm")
                    row["reference_gate_passed"] &= row["reference_repeat"]["identical"]
                if backend == "raster" and case["frozen_reference"]:
                    row["source_frozen"] = visual.compare(source_image, args.reference_dir / f"{case['case']}-source.ppm")
                    row["reference_gate_passed"] &= row["source_frozen"]["identical"]
                reference_key, mip_key = f"reference_{args.compression_profile}", f"mips_{args.compression_profile}"
                row[reference_key] = measure(reference_image, cooked_image, args.output / f"{name}-reference-{args.compression_profile}-difference.png")
                row[mip_key] = measure(args.output / f"{name}-mips.ppm", cooked_image, args.output / f"{name}-mips-{args.compression_profile}-difference.png")
                row["captures_valid"] = all(row[key]["same_dimensions"] and row[key]["visible"]
                                            for key in (reference_key, mip_key))
                row["passed"] = row["reference_gate_passed"] and row["captures_valid"]
            report["results"].append(row)
            baseline.write(args.output / "results.json", report)
            print(f"{name}: reference={row.get('reference_gate_passed')} lossy={row.get('reference_' + args.compression_profile)}", flush=True)
    finally:
        current = config.read_bytes() if config.exists() else None
        if current == last:
            if original is None:
                config.unlink(missing_ok=True)
            else:
                config.write_bytes(original)
        else:
            report["restore_warning"] = "External configuration edit preserved"
        report["configuration_restored"] = (config.read_bytes() if config.exists() else None) == original
        lock.unlink(missing_ok=True)
        report["renderer_unchanged"] = report["renderer"] == baseline.identity(args.renderer)
        report["cpu_report_unchanged"] = report["cpu_report"] == baseline.identity(args.cpu_report)
        report["shaders_unchanged"] = report["shaders"] == shader_snapshot()
        report["automated_checks_passed"] = len(report["results"]) == len(runs) and all(row.get("passed") for row in report["results"]) and all(report[key] for key in ("configuration_restored", "renderer_unchanged", "cpu_report_unchanged", "shaders_unchanged"))
        baseline.write(args.output / "results.json", report)
    return 0 if report["automated_checks_passed"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
