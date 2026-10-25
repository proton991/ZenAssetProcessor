"""Natural-source atlas eligibility and matched page-stage measurements.

The fixed corpus preserves authored samplers/UVs. Array comparisons share one
role-aware mip-prepared canonical input; spatial comparisons use authored input.
Only branches producing actual pages are eligible for repeated measurements.
"""
import argparse
from datetime import datetime, timezone
import json
from pathlib import Path
import shutil
import sys
from urllib.parse import unquote, urlsplit

from asset_processor_baseline import identity
from asset_processor_codec_measurements import records, run, write
from asset_processor_atlas_measurements import summary

NAMES = ("ChronographWatch", "PlaysetLightTest", "DiffuseTransmissionPlant", "USDShaderBallForGltf")


def input_files(source):
    document = json.loads(source.read_text(encoding="utf-8"))
    files = {source}
    for entry in document.get("buffers", []) + document.get("images", []):
        uri = entry.get("uri", "")
        if uri and urlsplit(uri).scheme != "data":
            if urlsplit(uri).scheme:
                raise ValueError("Network source dependencies are not allowed")
            path = (source.parent / unquote(uri)).resolve()
            if not path.is_relative_to(source.parent.resolve()) or not path.is_file():
                raise ValueError(f"Unexpected or missing dependency: {path}")
            files.add(path)
    return sorted(files)


def check_identities(document):
    entries = document["sources"] + document["original_inputs"] + document["copied_inputs"] + [document["probe"]]
    entries += json.loads(Path(document["sdk_identity_file"]["path"]).read_text(encoding="utf-8"))
    for entry in entries:
        if identity(Path(entry["path"]))["sha256"] != entry["sha256"]:
            raise ValueError(f"Recorded input/SDK/probe changed: {entry['path']}")


def prepare(args, root, output):
    if not all((args.probe, args.sdk, args.models, args.fingerprint)):
        raise ValueError("Preparation requires --probe, --sdk, --models and --fingerprint")
    output.mkdir(parents=True, exist_ok=False)
    logs = output / "logs"
    logs.mkdir()
    probe = output / args.probe.name
    shutil.copy2(args.probe.resolve(), probe)
    source_paths = [
        root / "tools/evaluation/atlas_workloads/main.cpp",
        root / "tools/evaluation/atlas_workloads/CMakeLists.txt",
        root / "tests/SceneValueComparison.h", Path(__file__).resolve(),
        root / "tools/validation/asset_processor_atlas_measurements.py",
        root / "tools/validation/asset_processor_codec_measurements.py",
        root / "tools/validation/asset_processor_codec_benchmark.py",
        root / "tools/validation/asset_processor_baseline.py",
        root / "tools/validation/asset_processor_paths.py"
    ]
    for path in source_paths:
        destination = output / "sources" / path.relative_to(root)
        destination.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(path, destination)
    sdk = args.sdk.resolve()
    write(output / "sdk-identities.json", [identity(p) for p in sorted(sdk.rglob("*")) if p.is_file()])
    document = {
        "started_utc": datetime.now(timezone.utc).isoformat(), "prepare_invocation": sys.argv,
        "probe": identity(probe), "sources": [identity(p) for p in source_paths],
        "sdk": str(sdk), "sdk_identity_file": identity(output / "sdk-identities.json"),
        "fingerprint": args.fingerprint, "original_inputs": [], "copied_inputs": [], "models": [], "cases": [],
        "method": {
            "corpus": list(NAMES), "import_workers": 2,
            "array_common": "One role-aware CookScene(generateMipmaps=true) from unmodified import; time/outcome recorded separately",
            "spatial_common": "Imported image/mip/sampler state retained; generateMipmaps=false",
            "timed_input": "Identical finalized canonical input package for each off/on pair",
            "timed_cook": "Page-only CookScene; excludes source import and shared mip preparation",
            "common_output": "Zstandard3, no mesh encoding/BC/quantization/fetch; default page limits2048dimension/256layers/64pages/8192images",
            "preservation": "Authored samplers, UVs, scene fields and logical per-texture mip bytes compared before/after atlas; all outputs roundtrip full public scene",
            "eligibility": "No sampler/UV/source edits; zero-page branches are eligibility evidence only",
            "timing": "Fresh process per sample; separate input ReadScene/CookScene/WriteScene clocks;7alternating rounds plus warmups",
            "memory": "Lifetime OS peaks include retained canonical input, cooked output and writer scratch; equality references are separate",
            "cache": "Warm OS cache; no engine asset cache; writes non-durable; CPU frequencies/background state uncontrolled",
            "limits": "No renderer/GPU/binding/draw/frame benefit claim; no profitability threshold is defined"
        }
    }
    for name in NAMES:
        source = args.models.resolve() / name / "glTF" / f"{name}.gltf"
        directory = output / name
        inputs = directory / "source"
        inputs.mkdir(parents=True)
        for path in input_files(source):
            destination = inputs / path.relative_to(source.parent)
            destination.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(path, destination)
            document["original_inputs"].append(identity(path))
            document["copied_inputs"].append(identity(destination))
        copied = inputs / source.name
        log = logs / f"{name}-prepare.log"
        process = run(probe, ["prepare", copied, directory], log, output, args.fingerprint)
        imported, common, branches = records(log, "IMPORTED "), records(log, "COMMON "), records(log, "BRANCH ")
        if len(imported) != 1 or len(common) != 2 or len(branches) != 4:
            raise ValueError("Incomplete natural-asset preflight")
        document["models"].append({"name": name, "source": identity(source), "copied_source": identity(copied),
                                   "preflight": process, "imported": imported[0], "common": common})
        for kind in ("array", "spatial"):
            case = {"name": f"{name}-{kind}", "model": name, "kind": kind, "directory": str(directory),
                    "input": identity(directory / f"{kind}-input.zenasset"),
                    "common": next(r for r in common if r["kind"] == kind), "modes": {}}
            for mode in ("off", "on"):
                case["modes"][mode] = {
                    "expected": next(r for r in branches if r["kind"] == kind and r["mode"] == mode),
                    "expected_package": identity(directory / f"{kind}-expected-{mode}.zenasset"), "runs": []
                }
            case["eligible_for_timing"] = case["modes"]["on"]["expected"]["pages"] > 0
            document["cases"].append(case)
            entry = case["modes"]["on"]["expected"]
            print(f"{case['name']}: {entry['pages']} pages, {entry['mapped_textures']} mapped textures, "
                  f"{len(entry['diagnostics'])} exclusions/diagnostics; timed={case['eligible_for_timing']}", flush=True)
        write(output / "results.json", document)
    check_identities(document)
    document["preflight_passed"] = True
    document["prepared_utc"] = datetime.now(timezone.utc).isoformat()
    document["eligible_cases"] = [c["name"] for c in document["cases"] if c["eligible_for_timing"]]
    write(output / "results.json", document)


def sample(document, case, mode, label, output):
    probe = Path(document["probe"]["path"])
    log = output / "logs" / f"{case['name']}-{mode}-{label}.log"
    destination = Path(case["directory"]) / f"{case['kind']}-{mode}-{label}.zenasset"
    result = run(probe, ["measure", case["kind"], mode, case["input"]["path"], destination],
                 log, output, document["fingerprint"])
    rows = records(log, "WORKLOAD_SAMPLE ")
    if len(rows) != 1 or any(rows[0][n] <= 0 for n in ("input_read_seconds", "cook_seconds", "write_seconds")):
        raise ValueError("Missing natural workload stage timing")
    result.update(rows[0])
    expected = case["modes"][mode]["expected"]
    for key in ("images", "textures", "materials", "pages", "mapped_textures", "base_vector_bytes", "mip_vector_bytes"):
        if result[key] != expected[key]:
            raise ValueError(f"Unexpected measured resource counts: {key}")
    result["package"] = identity(destination)
    if result["package"]["sha256"] != case["modes"][mode]["expected_package"]["sha256"]:
        raise ValueError("Measured output is not deterministic")
    return result


def measure(args, output):
    document = json.loads((output / "results.json").read_text(encoding="utf-8"))
    if not document.get("preflight_passed") or document.get("timing_started_utc"):
        raise ValueError("Need successful preflight with no prior timing campaign")
    check_identities(document)
    cases = [c for c in document["cases"] if c["eligible_for_timing"]]
    if not cases:
        raise ValueError("No actual-page cases; retain eligibility-only evidence")
    document["measure_invocation"] = sys.argv
    document["timing_started_utc"] = datetime.now(timezone.utc).isoformat()
    document["repetitions"] = args.runs
    document["execution_order"] = []
    document["baseline"] = {"runs": []}
    probe = Path(document["probe"]["path"])
    logs = output / "logs"
    document["baseline"]["warmup"] = run(probe, ["baseline"], logs / "baseline-warmup.log", output, document["fingerprint"])
    for case in cases:
        for mode in ("off", "on"):
            case["modes"][mode]["warmup"] = sample(document, case, mode, "warmup", output)
    for iteration in range(args.runs):
        document["baseline"]["runs"].append(run(probe, ["baseline"], logs / f"baseline-{iteration}.log",
                                                output, document["fingerprint"]))
        shift = iteration % len(cases)
        for case in cases[shift:] + cases[:shift]:
            for mode in (("off", "on") if iteration % 2 == 0 else ("on", "off")):
                result = sample(document, case, mode, str(iteration), output)
                result["round"] = iteration
                case["modes"][mode]["runs"].append(result)
                document["execution_order"].append({"round": iteration, "case": case["name"], "mode": mode, "pid": result["pid"]})
        write(output / "results.json", document)
        print(f"Natural atlas measured round {iteration+1}/{args.runs} complete", flush=True)
    for case in cases:
        for mode, item in case["modes"].items():
            packages = [r["package"]["path"] for r in [item["warmup"], *item["runs"]]]
            log = logs / f"{case['name']}-{mode}-verify.log"
            item["postflight"] = run(probe, ["verify", item["expected_package"]["path"], *packages],
                                    log, output, document["fingerprint"])
            if records(log, "VERIFIED ") != [{"packages": args.runs + 1}]:
                raise ValueError("Incomplete measured package comparison")
            item["summary"] = summary(item["runs"])
    document["baseline"]["summary"] = summary(document["baseline"]["runs"])
    check_identities(document)
    for case in document["cases"]:
        if identity(Path(case["input"]["path"]))["sha256"] != case["input"]["sha256"]:
            raise ValueError("Prepared canonical input changed")
    document["finished_utc"] = datetime.now(timezone.utc).isoformat()
    document["passed"] = True
    document["identities_unchanged"] = True
    write(output / "results.json", document)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("phase", choices=("prepare", "measure"))
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--probe", type=Path)
    parser.add_argument("--sdk", type=Path)
    parser.add_argument("--models", type=Path)
    parser.add_argument("--fingerprint")
    parser.add_argument("--runs", type=int, default=7)
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[2]
    output = args.output.resolve()
    if sys.platform != "win32" or args.runs < 7 or not output.is_relative_to((root / "build/m6-atlas-workloads").resolve()):
        parser.error("Windows, >=7 rounds, and processor/build/m6-atlas-workloads output are required")
    if args.phase == "prepare":
        prepare(args, root, output)
    else:
        measure(args, output)


if __name__ == "__main__":
    main()

