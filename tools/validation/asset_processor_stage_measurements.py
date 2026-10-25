"""Actual-pipeline private stage measurements with ordinary/instrumented parity.

Prepare and repeated timing are separate phases. Full output comparisons run in
separate processes. Import uses one worker plus its coordinator; recorded scopes
are coordinator wall intervals, never sums of parallel worker durations.
"""
import argparse
from datetime import datetime, timezone
import json
from pathlib import Path
import shutil
import statistics
import struct
import sys

from asset_processor_baseline import identity
from asset_processor_codec_measurements import check_embedded_glb, records, run, write
from asset_processor_atlas_workloads import input_files

PROFILES = ("raw", "zstd", "mesh", "mesh_zstd")
TIMED_PROFILES = ("raw", "mesh_zstd")
MODES = ("plain", "instrumented")
MODELS = ("DragonAttenuation", "DamagedHelmet", "Fox")


def one(log, prefix):
    values = records(log, prefix)
    if len(values) != 1:
        raise ValueError(f"Expected one {prefix} in {log}")
    return values[0]


def execute(document, output, mode, args, name, sampling=False):
    log = output / "logs" / f"{name}-{mode}.log"
    result = run(Path(document["probes"][mode]["path"]), args, log, output, document["fingerprint"])
    if sampling:
        result.update(one(log, "SAMPLE "))
        result["metrics"] = one(log, "METRICS ")
        result["diagnostics"] = one(log, "DIAGNOSTICS ")
        result["counts"] = one(log, "COUNTS ")
        if result["metrics"]["instrumented"] != (mode == "instrumented"):
            raise ValueError("Probe mode does not match its production compilation")
        for name, row in result["metrics"]["stages"].items():
            if not name or not 0 <= row["exclusive_ns"] <= row["inclusive_ns"]:
                raise ValueError("Invalid stage label or nested interval")
        if mode == "plain" and (any(row["calls"] or row["inclusive_ns"] or row["exclusive_ns"]
                                     for row in result["metrics"]["stages"].values())
                                or any(result["metrics"]["counters"].values())):
            raise ValueError("Ordinary production path unexpectedly recorded timing")
    return result


def check_identities(document):
    entries = document["sources"] + document["original_inputs"] + document["copied_inputs"]
    entries += list(document["probes"].values())
    entries += [case["input"] for case in document["cases"] if case["kind"] != "source"]
    for entry in entries:
        if identity(Path(entry["path"]))["sha256"] != entry["sha256"]:
            raise ValueError(f"Frozen input changed: {entry['path']}")


def counter_check(case, result):
    if not result["metrics"]["instrumented"]:
        return
    stages, counters = result["metrics"]["stages"], result["metrics"]["counters"]
    if case["kind"] == "source":
        for stage in ("source_acquire_parse", "source_convert", "imported_normalize"):
            if stages[stage]["calls"] == 0:
                raise ValueError(f"Missing source boundary: {case['name']} / {stage}")
        if result["counts"]["images"] and stages["source_images_io_decode_wait"]["calls"] == 0:
            raise ValueError("Images were not separately measured")
        if not result["total_known"] or result["processed_files"] != result["total_files"]:
            raise ValueError("Source physical-file accounting did not complete")
    else:
        chunks = case["chunks"]
        if case["kind"] == "cook-write":
            if stages["cook"]["calls"] != 1 or stages["write"]["calls"] != 1:
                raise ValueError("Missing whole Cook/Write boundary")
            if counters["mesh_encoded_resources"] != chunks["mesh_resources"] or counters["transport_encoded_resources"] != chunks["zstd_resources"]:
                raise ValueError("Actual encoding counters disagree with resource directory")
            # Writer first reserves the prefix, then rewrites its header/directory.
            if counters["written_bytes"] != case["expected_package"]["bytes"] + 64 + 64 * chunks["resources"]:
                raise ValueError("Actual write calls disagree with final package/prefix bytes")
            if stages["publish"]["calls"] != 1 or stages["file_flush"]["calls"] != 1:
                raise ValueError("Publication/flush boundary was not measured")
            if case["policy"] == "textures" and (counters["texture_candidates"] == 0 or counters["hdr_candidates"] == 0):
                raise ValueError("Original workload did not measure both texture and HDR encoding")
        else:
            if counters["mesh_decoded_resources"] != chunks["mesh_resources"] or counters["transport_decoded_resources"] != chunks["zstd_resources"]:
                raise ValueError("Actual decoding counters disagree with resource directory")
            if counters["read_bytes"] != 64 + 64 * chunks["resources"] + chunks["stored_bytes"]:
                raise ValueError("Actual read calls disagree with package resource bytes")
            if stages["read"]["calls"] != 1 or stages["scene_validate"]["calls"] != 1:
                raise ValueError("Missing whole Read/validation boundary")


def verify_packages(document, output, expected, packages, label):
    result = execute(document, output, "plain", ["verify", expected, *packages], label)
    if one(Path(result["log"]), "VERIFIED ") != {"packages": len(packages)}:
        raise ValueError("Full-scene comparison count is incomplete")
    return result


def preflight_source(document, output, source, name, timed):
    directory = output / name
    directory.mkdir(exist_ok=True)
    case = {"name": name + "-source", "kind": "source", "source": str(source), "timed": timed,
            "directory": str(directory), "modes": {}}
    for mode in MODES:
        destination = directory / f"source-{mode}.zenasset"
        result = execute(document, output, mode, ["source", source, destination], name + "-source-preflight", True)
        result["package"] = identity(destination)
        counter_check(case, result)
        case["modes"][mode] = {"preflight": result, "runs": []}
    expected = case["modes"]["plain"]["preflight"]
    observed = case["modes"]["instrumented"]["preflight"]
    if expected["package"]["sha256"] != observed["package"]["sha256"] or expected["diagnostics"] != observed["diagnostics"]:
        raise ValueError("Source instrumentation changed scene or diagnostics")
    case["expected_package"] = expected["package"]
    case["expected_counts"] = expected["counts"]
    case["expected_diagnostics"] = expected["diagnostics"]
    case["postflight"] = verify_packages(document, output, expected["package"]["path"], [observed["package"]["path"]], name + "-source-verify")
    document["cases"].append(case)
    return Path(expected["package"]["path"])


def preflight_pipeline(document, output, input_path, name, policy, profiles):
    directory = output / name
    directory.mkdir(exist_ok=True)
    for profile in profiles:
        case = {"name": name + "-" + profile + "-cook-write", "kind": "cook-write", "policy": policy,
                "profile": profile, "input": identity(input_path), "directory": str(directory),
                "timed": profile in TIMED_PROFILES, "modes": {}}
        for mode in MODES:
            destination = directory / f"{profile}-{mode}-expected.zenasset"
            result = execute(document, output, mode, ["cook-write", policy, profile, input_path, destination], case["name"] + "-preflight", True)
            result["package"] = identity(destination)
            case["modes"][mode] = {"preflight": result, "runs": []}
        expected = case["modes"]["plain"]["preflight"]
        observed = case["modes"]["instrumented"]["preflight"]
        if expected["package"]["sha256"] != observed["package"]["sha256"] or expected["diagnostics"] != observed["diagnostics"]:
            raise ValueError("Cook/write instrumentation changed output or diagnostics")
        audit = execute(document, output, "plain", ["audit", expected["package"]["path"]], case["name"] + "-audit")
        case["chunks"] = one(Path(audit["log"]), "CHUNKS ")
        case["audit"] = audit
        case["expected_package"] = expected["package"]
        case["expected_counts"] = expected["counts"]
        case["expected_diagnostics"] = expected["diagnostics"]
        if profile in ("zstd", "mesh_zstd") and case["chunks"]["zstd_resources"] == 0:
            raise ValueError("Zstandard profile did not use Zstandard")
        if profile in ("mesh", "mesh_zstd") and case["chunks"]["mesh_resources"] == 0:
            raise ValueError("Mesh profile did not use a mesh encoding")
        if policy == "textures" and any(expected["counts"][key] == 0 for key in ("bc4_images", "bc5_images", "bc7_images", "bc6h_cubes")):
            raise ValueError("Original texture workload did not produce all required accepted block formats")
        counter_check(case, observed)
        case["postflight"] = verify_packages(document, output, expected["package"]["path"], [observed["package"]["path"]], case["name"] + "-verify")
        document["cases"].append(case)
        read_case = {"name": name + "-" + profile + "-read", "kind": "read", "input": expected["package"],
                     "chunks": case["chunks"], "timed": case["timed"], "expected_counts": case["expected_counts"],
                     "expected_diagnostics": case["expected_diagnostics"], "modes": {}}
        for mode in MODES:
            result = execute(document, output, mode, ["read", read_case["input"]["path"]], read_case["name"] + "-preflight", True)
            counter_check(read_case, result)
            if result["counts"] != read_case["expected_counts"] or result["diagnostics"] != read_case["expected_diagnostics"]:
                raise ValueError("Read instrumentation changed scene counts/diagnostics")
            read_case["modes"][mode] = {"preflight": result, "runs": []}
        read_case["decoded_snapshots"] = {}
        for mode in MODES:
            destination = directory / f"{profile}-{mode}-decoded.zenasset"
            result = execute(document, output, mode, ["read-snapshot", read_case["input"]["path"], destination],
                             read_case["name"] + "-snapshot", True)
            counter_check(read_case, result)
            result["package"] = identity(destination)
            read_case["decoded_snapshots"][mode] = result
        snapshots = read_case["decoded_snapshots"]
        if snapshots["plain"]["package"]["sha256"] != snapshots["instrumented"]["package"]["sha256"]:
            raise ValueError("Ordinary/instrumented decoded canonical snapshots differ")
        read_case["snapshot_comparison"] = verify_packages(document, output, snapshots["plain"]["package"]["path"],
            [snapshots["instrumented"]["package"]["path"]], read_case["name"] + "-snapshot-verify")
        document["cases"].append(read_case)


def prepare(args, root, output):
    if not all((args.plain, args.instrumented, args.models, args.fingerprint, args.source_list)):
        raise ValueError("Preparation requires both probes, --models, --fingerprint and --source-list")
    output.mkdir(parents=True, exist_ok=False)
    (output / "logs").mkdir()
    document = {"prepare_invocation": sys.argv, "started_utc": datetime.now(timezone.utc).isoformat(),
                "fingerprint": args.fingerprint, "probes": {}, "sources": [], "original_inputs": [],
                "copied_inputs": [], "cases": [], "method": {
                    "import_workers": "One worker plus coordinator; no inline executor exists",
                    "scope_times": "Inclusive coordinator wall; exclusive subtracts nested same-thread intervals only; no worker sums",
                    "source_parse": "Acquisition, normalization and parser work including parser-owned I/O",
                    "image_decode": "Separate coordinator image-load/decode/drain intervals, includes scheduling and I/O",
                    "resource_checksum": "Payload hashes only; directory/header hashes remain in Serialize/PackageOpen",
                    "transport_decode": "Actual bounded streaming path, setup/allocation/frame checks minus separately scoped reads/checksums",
                    "deserialize": "Resource lookup/limits/object construction/deallocation excluding nested package/codec intervals",
                    "texture_encode": "Actual candidate encode plus required qualification; mip/page work remains processing",
                    "publication": "Actual atomic replacement call; ordinary non-durable writes",
                    "memory": "Absolute process lifetime peaks; retained input/cooked output and untimed source-result publication included",
                    "comparison": "Reference scenes only in separate verification processes; all output package SHA-256 compared",
                    "cache": "Warm OS cache, no engine asset cache, no OS cache flush; uncontrolled CPU clocks/background host",
                    "limits": "No pure parser CPU, physical disk latency, adapt/upload or GPU claims"}}
    for mode in MODES:
        original = getattr(args, mode).resolve()
        destination = output / (mode + ".exe")
        shutil.copy2(original, destination)
        document["probes"][mode] = identity(destination)
    sources = {Path(line).resolve() for line in args.source_list.read_text().splitlines() if line.strip()}
    sources.update((root / "tools/evaluation/stage_measurements").glob("*"))
    sources.update(root / name for name in ("tests/SceneValueComparison.h", "tools/validation/asset_processor_stage_measurements.py",
        "tools/validation/asset_processor_baseline.py", "tools/validation/asset_processor_codec_measurements.py",
        "tools/validation/asset_processor_codec_benchmark.py", "tools/validation/asset_processor_atlas_workloads.py",
        "tools/validation/asset_processor_atlas_measurements.py", "tools/validation/asset_processor_paths.py"))
    for source in sorted(sources):
        if source.is_file():
            destination = output / "sources" / source.relative_to(root)
            destination.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(source, destination)
            document["sources"].append(identity(source))
    for name in MODELS:
        source = args.models.resolve() / name / "glTF-Binary" / (name + ".glb")
        check_embedded_glb(source)
        directory = output / name
        directory.mkdir()
        copied = directory / source.name
        shutil.copy2(source, copied)
        document["original_inputs"].append(identity(source))
        document["copied_inputs"].append(identity(copied))
        canonical = preflight_source(document, output, copied, name, True)
        preflight_pipeline(document, output, canonical, name, "geometry", PROFILES)
        write(output / "results.json", document)
        print(f"Natural preflight passed: {name}", flush=True)
    fixture = output / "OriginalTextures" / "input.zenasset"
    fixture.parent.mkdir()
    execute(document, output, "plain", ["fixture", fixture], "original-fixture")
    preflight_pipeline(document, output, fixture, "OriginalTextures", "textures", ("mesh_zstd",))
    # Small format coverage cases qualify the boundaries, not performance.
    assets = root / "tests/integration/zenengine/CommonTest/Assets"
    controls = [("GltfControl", assets / "texture_roles.gltf"),
                ("FbxControl", assets / "FBX/combined_morph_skin_ascii.fbx")]
    for name, source in controls:
        directory = output / name / "source"
        directory.mkdir(parents=True)
        files = input_files(source) if source.suffix == ".gltf" else [source]
        for entry in files:
            copied = directory / entry.relative_to(source.parent)
            copied.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(entry, copied)
            document["original_inputs"].append(identity(entry))
            document["copied_inputs"].append(identity(copied))
        preflight_source(document, output, directory / source.name, name, False)
    directory = output / "ObjControl" / "source"
    directory.mkdir(parents=True)
    (directory / "original.obj").write_text("mtllib original.mtl\nv 0 0 0\nv 1 0 0\nv 0 1 0\nvt 0 0\nvt 1 0\nvt 0 1\nusemtl original\nf 1/1 2/2 3/3\n")
    (directory / "original.mtl").write_text("newmtl original\nKd 1 1 1\nmap_Kd original.tga\n")
    (directory / "original.tga").write_bytes(struct.pack("<BBBHHBHHHHBB", 0, 0, 2, 0, 0, 0, 0, 0, 4, 4, 32, 32) + bytes((64, 128, 192, 255)) * 16)
    document["copied_inputs"].extend(identity(path) for path in sorted(directory.iterdir()))
    preflight_source(document, output, directory / "original.obj", "ObjControl", False)
    check_identities(document)
    document["preflight_passed"] = True
    document["prepared_utc"] = datetime.now(timezone.utc).isoformat()
    write(output / "results.json", document)
    print("All ordinary/instrumented preflights, format counters and full comparisons passed", flush=True)


def sample(document, output, case, mode, label):
    if case["kind"] == "source":
        destination = Path(case["directory"]) / (f"measured-source-{mode}-{label}.zenasset")
        arguments = ["source", case["source"], destination]
    elif case["kind"] == "cook-write":
        destination = Path(case["directory"]) / (f"{case['profile']}-{mode}-{label}.zenasset")
        arguments = ["cook-write", case["policy"], case["profile"], case["input"]["path"], destination]
    else:
        destination = None
        arguments = ["read", case["input"]["path"]]
    result = execute(document, output, mode, arguments, case["name"] + "-" + label, True)
    counter_check(case, result)
    if result["counts"] != case["expected_counts"] or result["diagnostics"] != case["expected_diagnostics"]:
        raise ValueError("Measured scene count/diagnostic drift")
    if destination:
        result["package"] = identity(destination)
        if result["package"]["sha256"] != case["expected_package"]["sha256"]:
            raise ValueError("Measured output differs from validated expected package")
    return result


def min_median_max(values):
    return {"min": min(values), "median": statistics.median(values), "max": max(values)}


def summarize(samples):
    fields = ("source_seconds", "cook_seconds", "write_seconds", "read_seconds", "wall_seconds", "cpu_seconds",
              "peak_working_set_bytes", "peak_private_commit_bytes")
    result = {key: min_median_max([row[key] for row in samples]) for key in fields if all(key in row for row in samples)}
    if "metrics" in samples[0]:
        result["stages"] = {stage: {field: min_median_max([row["metrics"]["stages"][stage][field] for row in samples])
                                    for field in ("calls", "inclusive_ns", "exclusive_ns")}
                            for stage in samples[0]["metrics"]["stages"]}
    return result


def measure(args, output):
    document = json.loads((output / "results.json").read_text())
    if not document.get("preflight_passed") or document.get("timing_started_utc"):
        raise ValueError("Need complete preflight and a fresh timing campaign")
    check_identities(document)
    cases = [case for case in document["cases"] if case["timed"]]
    document["measure_invocation"] = sys.argv
    document["timing_started_utc"] = datetime.now(timezone.utc).isoformat()
    document["repetitions"] = args.runs
    document["execution_order"] = []
    document["baseline"] = {mode: {"runs": []} for mode in MODES}
    # Preserve failed warmup/round-zero evidence; never silently reuse log paths.
    write(output / "results.json", document)
    for mode in MODES:
        document["baseline"][mode]["warmup"] = execute(document, output, mode, ["baseline"], "baseline-warmup")
    for case in cases:
        for mode in MODES:
            case["modes"][mode]["warmup"] = sample(document, output, case, mode, "warmup")
    for iteration in range(args.runs):
        modes = MODES if iteration % 2 == 0 else tuple(reversed(MODES))
        for mode in modes:
            document["baseline"][mode]["runs"].append(execute(document, output, mode, ["baseline"], f"baseline-{iteration}"))
        shift = iteration % len(cases)
        for case in cases[shift:] + cases[:shift]:
            for mode in modes:
                result = sample(document, output, case, mode, str(iteration))
                result["round"] = iteration
                case["modes"][mode]["runs"].append(result)
                document["execution_order"].append({"round": iteration, "case": case["name"], "mode": mode, "pid": result["pid"]})
        write(output / "results.json", document)
        print(f"Stage measurement round {iteration+1}/{args.runs} complete", flush=True)
    for case in cases:
        for mode, item in case["modes"].items():
            item["summary"] = summarize(item["runs"])
            if case["kind"] != "read":
                packages = [row["package"]["path"] for row in [item["warmup"], *item["runs"]]]
                item["postflight"] = verify_packages(document, output, case["expected_package"]["path"], packages, case["name"] + "-" + mode + "-postflight")
        case["overhead_percent"] = {}
        for key in ("source_seconds", "cook_seconds", "write_seconds", "read_seconds", "wall_seconds"):
            plain, traced = case["modes"]["plain"]["summary"], case["modes"]["instrumented"]["summary"]
            if key in plain and plain[key]["median"] > 0:
                case["overhead_percent"][key] = 100 * (traced[key]["median"] / plain[key]["median"] - 1)
    for mode in MODES:
        document["baseline"][mode]["summary"] = summarize(document["baseline"][mode]["runs"])
    check_identities(document)
    document["passed"] = True
    document["identities_unchanged"] = True
    document["finished_utc"] = datetime.now(timezone.utc).isoformat()
    write(output / "results.json", document)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("phase", choices=("prepare", "measure"))
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--plain", type=Path)
    parser.add_argument("--instrumented", type=Path)
    parser.add_argument("--models", type=Path)
    parser.add_argument("--source-list", type=Path)
    parser.add_argument("--fingerprint")
    parser.add_argument("--runs", type=int, default=7)
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[2]
    output = args.output.resolve()
    if sys.platform != "win32" or args.runs < 7 or not output.is_relative_to((root / "build/stage-measurements").resolve()):
        parser.error("Windows, >=7 rounds, and processor/build/stage-measurements output required")
    if args.phase == "prepare":
        prepare(args, root, output)
    else:
        measure(args, output)


if __name__ == "__main__":
    main()
