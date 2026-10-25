"""Render the audited quantization packages against exact frozen M0 references.

No new desktop image threshold is inferred: the existing exact reference limit
is the conservative rendered gate. CPU bounds and image results stay separate.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
from asset_processor_paths import ENGINE_ROOT, VALIDATION_TOOLS

import asset_processor_baseline as baseline
import asset_processor_visual_parity as visual

ROOT = ENGINE_ROOT


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--renderer", required=True, type=Path)
    parser.add_argument("--cpu-report", required=True, type=Path)
    parser.add_argument("--reference-dir", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    for name in ("renderer", "cpu_report", "reference_dir", "output"):
        setattr(args, name, getattr(args, name).resolve())
    cpu = json.loads(args.cpu_report.read_text())
    if not cpu["passed"] or not cpu["actually_changed_vertices"]:
        raise ValueError("A successful CPU audit with actual changed positions is required")
    policy_path = VALIDATION_TOOLS / "asset_processor_quality.json"
    policy = json.loads(policy_path.read_text())
    threshold = policy["reference_profile"]["rendered_same_device_fixed_inputs_max_channel_error_8bit"]
    args.output.mkdir(parents=True, exist_ok=False)
    config = ROOT / "Data/engine.cfg"
    lock = config.with_name(config.name + ".gltf-smoke.lock")
    descriptor = os.open(lock, os.O_CREAT | os.O_EXCL | os.O_WRONLY)
    os.close(descriptor)
    original = config.read_bytes() if config.exists() else None
    if original is not None:
        (args.output / "original-engine.cfg").write_bytes(original)
    last = original
    os.environ.update(VK_LAYER_VALIDATE_SYNC="1", VK_LOADER_LAYERS_DISABLE="~implicit~", DISABLE_RTSS_LAYER="1")
    report = {"renderer": baseline.identity(args.renderer), "cpu_report": baseline.identity(args.cpu_report),
              "policy": baseline.identity(policy_path), "script": baseline.identity(Path(__file__).resolve()),
              "rendered_max_channel_error": threshold,
              "threshold_note": "Existing exact reference threshold applied conservatively; no desktop render tolerance added.",
              "source_cache": "disabled explicitly", "position_bits": cpu["position_bits"], "results": []}
    try:
        for cpu_case in cpu["results"]:
            case = cpu_case["case"]
            settings = (args.reference_dir / f"{case}-source.cfg").read_bytes()
            source = Path(cpu_case["source"]["path"])
            if baseline.identity(source) != cpu_case["source"]:
                raise RuntimeError("Source changed since CPU audit")
            paths = [("source", source)] + [(name, Path(cpu_case["packages"][name]["path"]))
                                            for name in ("reference", "desktop")]
            row = {"case": case, "source": cpu_case["source"], "cpu_audit": cpu_case["audit"], "captures": []}
            for name, path in paths:
                if name != "source" and baseline.identity(path) != cpu_case["packages"][name]:
                    raise RuntimeError("Package changed since CPU audit")
                if (config.read_bytes() if config.exists() else None) != last:
                    raise RuntimeError("Configuration changed externally; refusing to overwrite it")
                last = settings + f"\ndefault_model_path={path.as_posix()}\n".encode("utf-8")
                row["captures"].append(visual.run_capture(args, last, f"{case}-{name}", config))
            if all(capture["passed"] for capture in row["captures"]):
                source_image = args.output / f"{case}-source.ppm"
                reference_image = args.output / f"{case}-reference.ppm"
                row["source_frozen"] = visual.compare(source_image, args.reference_dir / f"{case}-source.ppm")
                row["source_reference"] = visual.compare(source_image, reference_image)
                row["reference_quantized"] = visual.compare(reference_image, args.output / f"{case}-desktop.ppm")
                row["reference_gate_passed"] = all(row[key]["same_dimensions"] and row[key]["visible"]
                                                      and row[key]["max_channel_error"] <= threshold
                                                      for key in ("source_frozen", "source_reference"))
                row["quantized_gate_passed"] = (row["reference_quantized"]["same_dimensions"]
                                                 and row["reference_quantized"]["visible"]
                                                 and row["reference_quantized"]["max_channel_error"] <= threshold)
                row["passed"] = row["reference_gate_passed"] and row["quantized_gate_passed"]
            report["results"].append(row)
            baseline.write(args.output / "results.json", report)
            print(f"{case}: reference={row.get('reference_gate_passed')} quantized={row.get('quantized_gate_passed')} "
                  f"metrics={row.get('reference_quantized')}", flush=True)
    finally:
        if (config.read_bytes() if config.exists() else None) == last:
            if original is None:
                config.unlink(missing_ok=True)
            else:
                config.write_bytes(original)
        else:
            report["restore_warning"] = "External configuration edit preserved"
        report["configuration_restored"] = (config.read_bytes() if config.exists() else None) == original
        lock.unlink(missing_ok=True)
        report["renderer_unchanged"] = baseline.identity(args.renderer) == report["renderer"]
        report["cpu_report_unchanged"] = baseline.identity(args.cpu_report) == report["cpu_report"]
        report["reference_gate_passed"] = (len(report["results"]) == len(visual.CASES)
                                            and all(row.get("reference_gate_passed") for row in report["results"]))
        report["quantized_gate_passed"] = (len(report["results"]) == len(visual.CASES)
                                            and all(row.get("quantized_gate_passed") for row in report["results"]))
        report["passed"] = (report["reference_gate_passed"] and report["quantized_gate_passed"]
                            and report["configuration_restored"] and report["renderer_unchanged"]
                            and report["cpu_report_unchanged"])
        baseline.write(args.output / "results.json", report)
    return 0 if report["passed"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
