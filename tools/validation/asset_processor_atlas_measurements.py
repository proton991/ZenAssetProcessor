"""Measure matched off/on atlas CookScene and WriteScene stages on original fixtures.

Array scenarios preserve complete authored mip chains; spatial scenarios preserve
one authored level. Each pair uses the same canonical input and Zstandard level 3.
Correctness/reference storage is kept in separate preflight/verification processes.
"""
import argparse
from datetime import datetime, timezone
import json
import os
from pathlib import Path
import platform
import shutil
import statistics
import sys

from asset_processor_baseline import identity
from asset_processor_codec_measurements import records, run, write

SCENARIOS = (("array", 128), ("array", 512), ("spatial", 128), ("spatial", 512))
METRICS = ("input_read_seconds", "cook_seconds", "write_seconds", "wall_seconds",
           "cpu_seconds", "peak_working_set_bytes", "peak_private_commit_bytes")


def summary(samples):
    return {key: {"min": min(row[key] for row in samples),
                  "median": statistics.median(row[key] for row in samples),
                  "max": max(row[key] for row in samples)}
            for key in METRICS if all(key in row for row in samples)}


def sample(probe, case, mode, label, logs, output, fingerprint):
    destination = Path(case["directory"]) / f"{mode}-{label}.zenasset"
    log = logs / f"{case['name']}-{mode}-{label}.log"
    result = run(probe, ["measure", case["kind"], str(case["size"]), mode,
                        case["input"]["path"], destination], log, output, fingerprint)
    stage = records(log, "ATLAS_SAMPLE ")
    if len(stage) != 1 or any(stage[0][name] <= 0 for name in ("input_read_seconds", "cook_seconds", "write_seconds")):
        raise ValueError(f"Missing stage timings: {log}")
    result.update(stage[0])
    expected = case["modes"][mode]["expected"]
    for key in ("images", "textures", "materials", "array_pages", "spatial_bindings", "base_vector_bytes", "mip_vector_bytes", "diagnostics"):
        if result[key] != expected[key]:
            raise ValueError(f"Measured page statistics differ from preflight: {log}")
    result["package"] = identity(destination)
    if result["package"]["sha256"] != case["modes"][mode]["expected_package"]["sha256"]:
        raise ValueError(f"Measured serialization is not deterministic: {destination}")
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--probe", type=Path, required=True)
    parser.add_argument("--sdk", type=Path, required=True)
    parser.add_argument("--fingerprint", required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--runs", type=int, default=7)
    args = parser.parse_args()
    if sys.platform != "win32" or args.runs < 7:
        parser.error("Windows and at least seven measured repetitions are required")
    root = Path(__file__).resolve().parents[2]
    output, sdk = args.output.resolve(), args.sdk.resolve()
    if not output.is_relative_to((root / "build/m6-atlas-measurements").resolve()):
        parser.error("Outputs must stay inside processor/build/m6-atlas-measurements")
    output.mkdir(parents=True, exist_ok=False)
    logs = output / "logs"
    logs.mkdir()
    source_paths = [
        root / "tools/evaluation/atlas_measurements/main.cpp",
        root / "tools/evaluation/atlas_measurements/CMakeLists.txt",
        root / "tests/SceneValueComparison.h", Path(__file__).resolve(),
        root / "tools/validation/asset_processor_codec_measurements.py",
        root / "tools/validation/asset_processor_codec_benchmark.py",
        root / "tools/validation/asset_processor_baseline.py",
        root / "tools/validation/asset_processor_paths.py"
    ]
    source_ids = [identity(path) for path in source_paths]
    for path in source_paths:
        destination = output / "sources" / path.relative_to(root)
        destination.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(path, destination)
    probe = output / args.probe.name
    shutil.copy2(args.probe.resolve(), probe)
    sdk_ids = [identity(path) for path in sorted(sdk.rglob("*")) if path.is_file()]
    write(output / "sdk-identities.json", sdk_ids)
    document = {
        "started_utc": datetime.now(timezone.utc).isoformat(), "invocation": sys.argv,
        "probe": identity(probe), "fingerprint": args.fingerprint, "sdk": str(sdk),
        "sdk_identity_file": identity(output / "sdk-identities.json"), "sources": source_ids,
        "host": {"platform": platform.platform(), "machine": platform.machine(),
                 "cpu": os.environ.get("PROCESSOR_IDENTIFIER"), "logical_cpus": os.cpu_count()},
        "method": {
            "inputs": "Original deterministic canonical fixtures: 36 RGBA8Srgb images/materials, two shared-mesh instances, alternate authored UV transforms",
            "array": "Complete authored mip chain, off vs array, maximum 12 layers per page",
            "spatial": "One authored power-of-two level, off vs spatial, 2048 maximum page extent",
            "common_policy": "No geometry transform, image re-encoding or mip generation; Zstandard level 3, mesh compression disabled, non-durable publication",
            "stages": "Separate clocks around ReadScene(input), CookScene, and WriteScene; write includes serialization, compression, temporary file and publication",
            "process": "Fresh process per sample; input scene retained during cooking/writing; no equality reference or output-read scene retained",
            "memory": "Absolute lifetime OS peak working set/private commit includes input load, cooking, write, runtime; no per-stage memory attribution",
            "correctness": "Preflight repeated-cook equality, byte preservation and page counts; all measured/warmup packages fully read/compared in separate postflight processes",
            "cache": "Warm OS cache after fixture creation/read; no engine asset cache, no disk flush or durable-write guarantee",
            "order": "Cases rotate each round; off/on order reverses every round; exact execution order retained",
            "repetitions": args.runs, "baseline": "Same executable no-operation mode each round, absolute overhead not subtracted",
            "limits": "Synthetic resource workload, not natural-scene profitability; no GPU, renderer, binding/draw/frame, image-quality, mip-generation or BC encoder claim"
        }, "cases": [], "baseline": {"runs": []}, "execution_order": []
    }
    for kind, size in SCENARIOS:
        name = f"{kind}-{size}"
        directory = output / name
        directory.mkdir()
        case = {"name": name, "kind": kind, "size": size, "directory": str(directory), "modes": {}}
        log = logs / f"{name}-prepare.log"
        case["preflight"] = run(probe, ["prepare", kind, str(size), directory], log, output, args.fingerprint)
        expected = records(log, "EXPECTED ")
        fixture = records(log, "FIXTURE ")
        if len(expected) != 2 or len(fixture) != 1:
            raise ValueError("Incomplete preflight")
        case["fixture"] = fixture[0]
        case["input"] = identity(directory / "input.zenasset")
        case["recipe"] = identity(directory / "source.json")
        for mode in ("off", "on"):
            case["modes"][mode] = {
                "expected": next(row for row in expected if row["mode"] == mode),
                "expected_package": identity(directory / f"expected-{mode}.zenasset"), "runs": []
            }
        if case["modes"]["on"]["expected"]["images"] >= case["fixture"]["images"]:
            raise ValueError("Atlas-on did not create actual pages")
        document["cases"].append(case)
        write(output / "results.json", document)
        print(f"{name}: actual pages, full preservation/roundtrip and deterministic preflight pass", flush=True)
    document["baseline"]["warmup"] = run(probe, ["baseline"], logs / "baseline-warmup.log", output, args.fingerprint)
    for case in document["cases"]:
        for mode in ("off", "on"):
            case["modes"][mode]["warmup"] = sample(probe, case, mode, "warmup", logs, output, args.fingerprint)
    for iteration in range(args.runs):
        document["baseline"]["runs"].append(run(probe, ["baseline"], logs / f"baseline-{iteration}.log", output, args.fingerprint))
        shift = iteration % len(document["cases"])
        cases = document["cases"][shift:] + document["cases"][:shift]
        for case in cases:
            modes = ("off", "on") if iteration % 2 == 0 else ("on", "off")
            for mode in modes:
                result = sample(probe, case, mode, str(iteration), logs, output, args.fingerprint)
                result["round"] = iteration
                case["modes"][mode]["runs"].append(result)
                document["execution_order"].append({"round": iteration, "case": case["name"], "mode": mode, "pid": result["pid"]})
        write(output / "results.json", document)
        print(f"Measured atlas round {iteration + 1}/{args.runs} complete", flush=True)
    for case in document["cases"]:
        for mode, item in case["modes"].items():
            packages = [row["package"]["path"] for row in [item["warmup"], *item["runs"]]]
            log = logs / f"{case['name']}-{mode}-verify.log"
            item["postflight"] = run(probe, ["verify", item["expected_package"]["path"], *packages], log, output, args.fingerprint)
            checked = records(log, "VERIFIED ")
            if len(checked) != 1 or checked[0]["packages"] != args.runs + 1:
                raise ValueError("Incomplete measured-package verification")
            item["summary"] = summary(item["runs"])
        if identity(Path(case["input"]["path"]))["sha256"] != case["input"]["sha256"]:
            raise ValueError("Input fixture changed")
    document["baseline"]["summary"] = summary(document["baseline"]["runs"])
    for entry in source_ids + sdk_ids + [document["probe"]]:
        if identity(Path(entry["path"]))["sha256"] != entry["sha256"]:
            raise ValueError(f"Source/SDK/binary changed: {entry['path']}")
    document["finished_utc"] = datetime.now(timezone.utc).isoformat()
    document["passed"] = True
    document["identities_unchanged"] = True
    write(output / "results.json", document)
    print(f"All {len(SCENARIOS) * 2 * (args.runs + 1)} warmup/measured packages pass full scene equality and deterministic bytes", flush=True)


if __name__ == "__main__":
    main()


