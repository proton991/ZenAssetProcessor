"""Compare source and v3 publication against the seven frozen M0 visual references.

Uses the exact reference configuration and fixed-step view. The result concerns
these views/times, not general material conformance or all animation frames.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
from asset_processor_paths import ENGINE_ROOT
import re
import sys

import asset_processor_baseline as baseline
import smoke_gltf_rendering as smoke

ROOT = ENGINE_ROOT
CASES = [f"visual-{index}" for index in range(5)] + [f"fbx-visual-{index}" for index in range(2)]


def pixels(path):
    data = path.read_bytes()
    header = re.match(rb"P6\s+(\d+)\s+(\d+)\s+255\s", data)
    if not header:
        raise ValueError(f"Unsupported capture {path}")
    width, height = map(int, header.groups())
    data = data[header.end():]
    if len(data) != width * height * 3:
        raise ValueError(f"Invalid capture dimensions {path}")
    return width, height, data


def compare(first, second):
    aw, ah, a = pixels(first)
    bw, bh, b = pixels(second)
    same_size = (aw, ah) == (bw, bh)
    differences = [abs(x - y) for x, y in zip(a, b)]
    return {"same_dimensions": same_size, "different_channels": sum(value != 0 for value in differences),
            "max_channel_error": max(differences),
            "rmse_8bit": (sum(value * value for value in differences) / len(differences)) ** 0.5,
            "visible": max(a) > 0 and max(b) > 0,
            "identical": same_size and a == b}


def run_capture(args, settings, tag, config, mode=2, frames=3, disable_rt=True):
    config.write_bytes(settings)
    (args.output / f"{tag}.cfg").write_bytes(settings)
    capture = args.output / f"{tag}.ppm"
    command = [args.renderer, "--enable-validation", "--no-ui", "--fixed-step", "--vsync=0",
               f"--frames={frames}", "--width=640", "--height=480", f"--mode={mode}", "--gpu-memory-stats", "--no-asset-cache",
               f"--capture={capture}"]
    if disable_rt:
        command.append("--disable-rt")
    if getattr(args, "deterministic_gi_capture", False):
        command.append("--deterministic-gi-capture")
    result = {"label": tag, "run": baseline.run(command, args.output / f"{tag}.log", ROOT, 180),
              "configuration_sha256": hashlib.sha256(settings).hexdigest()}
    log = Path(result["run"]["log"]).read_text(encoding="utf-8", errors="replace")
    result["validation_errors"] = [line for line in log.splitlines()
                                   if "[error]" in line or "VUID-" in line or "SYNC-HAZARD" in line]
    result["publication_log"] = [line for line in log.splitlines() if "published in " in line]
    result["gpu_memory_log"] = [line for line in log.splitlines() if "GPU memory VMA:" in line]
    result["gi_provider_log"] = [line for line in log.splitlines() if "Hybrid GI selected " in line]
    if capture.exists():
        result["image"] = smoke.capture_statistics(capture)
        result["identity"] = baseline.identity(capture)
    result["passed"] = result["run"]["exit_code"] == 0 and not result["validation_errors"] and "image" in result
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--renderer", type=Path, required=True)
    parser.add_argument("--processor", type=Path, required=True)
    parser.add_argument("--reference-dir", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--compression", choices=("none", "zstd"), default="none")
    parser.add_argument("--mesh-compression", choices=("none", "meshopt"), default="none")
    args = parser.parse_args()
    for name in ("renderer", "processor", "reference_dir", "output"):
        setattr(args, name, getattr(args, name).resolve())
    args.output.mkdir(parents=True, exist_ok=False)
    config = ROOT / "Data/engine.cfg"
    lock = config.with_name(config.name + ".gltf-smoke.lock")
    lock_descriptor = os.open(lock, os.O_CREAT | os.O_EXCL | os.O_WRONLY)
    os.close(lock_descriptor)
    original = config.read_bytes() if config.exists() else None
    if original is not None:
        (args.output / "original-engine.cfg").write_bytes(original)
    last = original
    os.environ.update(VK_LAYER_VALIDATE_SYNC="1", VK_LOADER_LAYERS_DISABLE="~implicit~", DISABLE_RTSS_LAYER="1")
    report = {"renderer": baseline.identity(args.renderer), "processor": baseline.identity(args.processor),
              "scope": "Seven fixed M0 views at the same fixed-step animation time; direct source, v3, frozen reference.",
              "compression": args.compression,
              "mesh_compression": args.mesh_compression,
              "source_cache": "Explicit --no-asset-cache on both publication runs", "results": []}
    try:
        for case in CASES:
            settings = (args.reference_dir / f"{case}-source.cfg").read_bytes()
            model_lines = [line.split("=", 1)[1].strip() for line in settings.decode("utf-8-sig").splitlines()
                           if line.split("=", 1)[0].strip() == "default_model_path"]
            model = Path(model_lines[-1])
            package = args.output / f"{case}.zenasset"
            result = {"case": case, "source": baseline.identity(model), "captures": []}
            command = [args.processor, "cook", model, "--output", package, "--workers", "2"]
            if args.compression != "none":
                command += ["--compression", args.compression]
            if args.mesh_compression != "none":
                command += ["--mesh-compression", args.mesh_compression]
            result["cook"] = baseline.run(command,
                                          args.output / f"{case}-cook.log", ROOT)
            if result["cook"]["exit_code"] == 0:
                result["package"] = baseline.identity(package)
                for label, asset in (("source", model), ("v3", package)):
                    current = config.read_bytes() if config.exists() else None
                    if current != last:
                        raise RuntimeError("Configuration changed externally; refusing to overwrite it")
                    last = settings + f"\ndefault_model_path={asset.as_posix()}\n".encode("utf-8")
                    capture = run_capture(args, last, f"{case}-{label}", config)
                    result["captures"].append(capture)
                if all(capture["passed"] for capture in result["captures"]):
                    source_image = args.output / f"{case}-source.ppm"
                    result["source_v3"] = compare(source_image, args.output / f"{case}-v3.ppm")
                    result["source_frozen"] = compare(source_image, args.reference_dir / f"{case}-source.ppm")
                    result["source_v3_passed"] = result["source_v3"]["identical"] and result["source_v3"]["visible"]
                    result["frozen_passed"] = result["source_frozen"]["identical"] and result["source_frozen"]["visible"]
                    result["passed"] = result["source_v3_passed"] and result["frozen_passed"]
            report["results"].append(result)
            baseline.write(args.output / "results.json", report)
            print(f"{case}: {'PASS' if result.get('passed') else 'FAIL'}; "
                  f"source/v3={result.get('source_v3', {})}; frozen={result.get('source_frozen', {})}", flush=True)
    finally:
        current = config.read_bytes() if config.exists() else None
        if current == last:
            if original is None:
                config.unlink(missing_ok=True)
            else:
                config.write_bytes(original)
        else:
            report["restore_warning"] = "Configuration changed externally; current bytes were preserved"
        report["configuration_restored"] = (config.read_bytes() if config.exists() else None) == original
        lock.unlink(missing_ok=True)
        report["renderer_unchanged"] = baseline.identity(args.renderer) == report["renderer"]
        report["processor_unchanged"] = baseline.identity(args.processor) == report["processor"]
        report["passed"] = (len(report["results"]) == len(CASES)
                            and all(item.get("passed") for item in report["results"])
                            and report["renderer_unchanged"] and report["processor_unchanged"]
                            and report["configuration_restored"])
        baseline.write(args.output / "results.json", report)
    return 0 if report["passed"] else 1


if __name__ == "__main__":
    sys.exit(main())
