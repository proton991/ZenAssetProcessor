"""Fixed-input HDR environment captures with exact raw-reference gates.

BC6H frame metrics are descriptive. The independent per-face/mip CPU gate must
pass before capturing. Preserve user configuration and shader/GI inputs.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
from asset_processor_paths import ENGINE_ROOT

import asset_processor_baseline as baseline
import asset_processor_visual_parity as visual
from asset_processor_texture_visual import measure, shader_snapshot
import smoke_gltf_rendering as smoke

ROOT = ENGINE_ROOT


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--renderer", type=Path, required=True)
    parser.add_argument("--cpu-report", type=Path, required=True)
    parser.add_argument("--orientation-report", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    for name in ("renderer", "cpu_report", "orientation_report", "output"):
        setattr(args, name, getattr(args, name).resolve())
    cpu = json.loads(args.cpu_report.read_text())
    orientation = json.loads(args.orientation_report.read_text())
    if not cpu["passed"] or not orientation["passed"]:
        raise ValueError("Independent HDR audit must pass before rendering")
    environment = next(case for case in cpu["cases"] if case["name"] == "EnvironmentTest")
    source = args.cpu_report.parent / "source/EnvironmentTest.gltf"
    args.output.mkdir(parents=True, exist_ok=False)
    config = ROOT / "Data/engine.cfg"
    lock = config.with_name(config.name + ".gltf-smoke.lock")
    descriptor = os.open(lock, os.O_CREAT | os.O_EXCL | os.O_WRONLY)
    os.close(descriptor)
    original = config.read_bytes()
    last = original
    (args.output / "original-engine.cfg").write_bytes(original)
    report = dict(renderer=baseline.identity(args.renderer), source=baseline.identity(source),
                  cpu_report=baseline.identity(args.cpu_report), orientation_report=baseline.identity(args.orientation_report),
                  script=baseline.identity(Path(__file__)), shaders=shader_snapshot(),
                  original_configuration_sha256=hashlib.sha256(original).hexdigest(),
                  acceptance="Exact source/raw and identical-raw repeat gates; per-face/mip CPU limits mandatory; lossy rendered deltas descriptive.",
                  lossy_frame_threshold=None, visual_review="pending", results=[])
    runs = [(environment, backend, source) for backend in ("raster", "compute", "hardware")]
    runs.append((orientation["cases"][0], "raster", None))
    os.environ.update(VK_LAYER_VALIDATE_SYNC="1", VK_LOADER_LAYERS_DISABLE="~implicit~", DISABLE_RTSS_LAYER="1")
    try:
        for case, backend, direct in runs:
            name = f"{case['name']}-{backend}"
            settings = smoke.configuration_for_asset(original, Path(case["packages"]["reference"]["path"]), skybox_visible=True)
            if backend != "raster":
                settings += (f"\nvoxel_gi_backend={backend}\nvoxel_gi_ray_provider={'hardware' if backend == 'hardware' else 'voxel'}\n").encode()
            row = dict(case=case["name"], backend=backend, captures=[], has_direct_source=direct is not None)
            paths = [("source", direct)] if direct is not None else []
            paths += [(profile, Path(case["packages"][profile]["path"])) for profile in ("reference", "bc6h")]
            paths.append(("reference-repeat", Path(case["packages"]["reference"]["path"])))
            for profile, asset in paths:
                if profile != "source":
                    expected = case["packages"]["reference" if profile == "reference-repeat" else profile]
                    if baseline.identity(asset) != expected:
                        raise ValueError("Audited package identity changed")
                elif baseline.identity(asset) != report["source"]:
                    raise ValueError("Source identity changed")
                if config.read_bytes() != last:
                    raise RuntimeError("External configuration edit detected; refusing to overwrite it")
                last = settings + f"\ndefault_model_path={asset.as_posix()}\n".encode()
                row["captures"].append(visual.run_capture(args, last, f"{name}-{profile}", config,
                    mode=2 if backend == "raster" else 3, frames=3 if backend == "raster" else 16,
                    disable_rt=backend != "hardware"))
            if all(capture["passed"] for capture in row["captures"]):
                reference = args.output / f"{name}-reference.ppm"
                row["reference_repeat"] = visual.compare(reference, args.output / f"{name}-reference-repeat.ppm")
                row["reference_gate_passed"] = row["reference_repeat"]["identical"] and row["reference_repeat"]["visible"]
                if direct is not None:
                    row["source_reference"] = visual.compare(args.output / f"{name}-source.ppm", reference)
                    row["reference_gate_passed"] &= row["source_reference"]["identical"] and row["source_reference"]["visible"]
                row["reference_bc6h"] = measure(reference, args.output / f"{name}-bc6h.ppm",
                                               args.output / f"{name}-difference-8x.png")
                row["passed"] = row["reference_gate_passed"] and row["reference_bc6h"]["visible"]
            report["results"].append(row)
            baseline.write(args.output / "results.json", report)
            print(f"{name}: exact_reference={row.get('reference_gate_passed')}; lossy={row.get('reference_bc6h')}", flush=True)
    finally:
        if config.read_bytes() == last:
            config.write_bytes(original)
        else:
            report["restore_warning"] = "Preserved external configuration edit"
        report["configuration_restored"] = config.read_bytes() == original
        lock.unlink(missing_ok=True)
        report["renderer_unchanged"] = report["renderer"] == baseline.identity(args.renderer)
        report["shaders_unchanged"] = report["shaders"] == shader_snapshot()
        report["cpu_reports_unchanged"] = (report["cpu_report"] == baseline.identity(args.cpu_report)
            and report["orientation_report"] == baseline.identity(args.orientation_report))
        report["automated_checks_passed"] = len(report["results"]) == len(runs) and all(row.get("passed") for row in report["results"]) and all(
            report[key] for key in ("configuration_restored", "renderer_unchanged", "shaders_unchanged", "cpu_reports_unchanged"))
        baseline.write(args.output / "results.json", report)
    return 0 if report["automated_checks_passed"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
