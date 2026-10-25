"""Matched lossless-codec package measurements using the installed SDK probe.

Preparation/full-scene equality runs are separate from every measured process.
ReadScene elapsed time includes file I/O, checksums, transport/semantic decoding,
object construction and validation; it is not pure decompression time.
"""
import argparse
from datetime import datetime, timezone
import json
import os
from pathlib import Path
import platform
import shutil
import statistics
import struct
import sys
from urllib.parse import urlsplit

from asset_processor_baseline import identity
from asset_processor_codec_benchmark import measured

VARIANTS = ("raw", "zstd", "mesh", "mesh_zstd")
METRICS = ("wall_seconds", "cpu_seconds", "peak_working_set_bytes",
           "peak_private_commit_bytes", "read_seconds")


def records(log, prefix):
    return [json.loads(line[len(prefix):]) for line in log.read_text(encoding="utf-8").splitlines()
            if line.startswith(prefix)]


def summarize(runs):
    return {key: {"min": min(row[key] for row in runs),
                  "median": statistics.median(row[key] for row in runs),
                  "max": max(row[key] for row in runs)}
            for key in METRICS if all(key in row for row in runs)}


def write(path, document):
    path.write_text(json.dumps(document, indent=2) + "\n", encoding="utf-8")


def run(probe, arguments, log, output, fingerprint):
    result = measured([probe, *arguments], log, output)
    sdk = records(log, "SDK ")
    if len(sdk) != 1 or sdk[0]["fingerprint"] != fingerprint:
        raise ValueError(f"SDK fingerprint mismatch: {log}")
    for name in ("peak_working_set_bytes", "peak_private_commit_bytes"):
        if result.get(name, 0) <= 0:
            raise ValueError(f"Missing Windows process peak: {log}")
    samples = records(log, "SAMPLE ")
    if arguments[0] == "read":
        if len(samples) != 1 or samples[0]["read_seconds"] <= 0:
            raise ValueError(f"Missing ReadScene timing: {log}")
        result.update(samples[0])
    return result


def check_embedded_glb(path):
    with path.open("rb") as stream:
        header = stream.read(20)
        magic, version, total, length, chunk_type = struct.unpack("<5I", header)
        if magic != 0x46546C67 or version != 2 or total != path.stat().st_size or chunk_type != 0x4E4F534A:
            raise ValueError(f"Expected complete self-contained GLB: {path}")
        scene = json.loads(stream.read(length))
    for item in scene.get("buffers", []) + scene.get("images", []):
        uri = item.get("uri", "")
        if uri and urlsplit(uri).scheme != "data":
            raise ValueError(f"GLB has external dependency: {uri}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--probe", type=Path, required=True)
    parser.add_argument("--sdk", type=Path, required=True)
    parser.add_argument("--fingerprint", required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--model", nargs=2, action="append", metavar=("NAME", "GLB"), required=True)
    parser.add_argument("--runs", type=int, default=7)
    args = parser.parse_args()
    if sys.platform != "win32" or args.runs < 7:
        parser.error("Windows and at least seven measured repetitions are required")
    probe, sdk, output = args.probe.resolve(), args.sdk.resolve(), args.output.resolve()
    processor = Path(__file__).resolve().parents[2]
    required_parent = (processor / "build/m4-codec-measurements").resolve()
    if not output.is_relative_to(required_parent):
        parser.error("Output must be inside processor/build/m4-codec-measurements")
    output.mkdir(parents=True, exist_ok=False)
    logs = output / "logs"
    logs.mkdir()
    snapshot = output / "sources"
    snapshot.mkdir()
    source_paths = [processor / "tools/evaluation/codec_measurements/main.cpp",
                    processor / "tools/evaluation/codec_measurements/CMakeLists.txt",
                    processor / "tests/SceneValueComparison.h",
                    Path(__file__).resolve(),
                    processor / "tools/validation/asset_processor_codec_benchmark.py",
                    processor / "tools/validation/asset_processor_baseline.py",
                    processor / "tools/validation/asset_processor_paths.py"]
    source_ids = [identity(path) for path in source_paths]
    for path in source_paths:
        destination = snapshot / path.relative_to(processor)
        destination.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(path, destination)
    sdk_ids = [identity(path) for path in sorted(sdk.rglob("*")) if path.is_file()]
    write(output / "sdk-identities.json", sdk_ids)
    shutil.copy2(probe, output / probe.name)
    probe = output / probe.name
    document = {
        "started_utc": datetime.now(timezone.utc).isoformat(), "invocation": sys.argv,
        "host": {"platform": platform.platform(), "machine": platform.machine(),
                 "cpu": os.environ.get("PROCESSOR_IDENTIFIER"), "logical_cpus": os.cpu_count()},
        "probe": identity(probe), "sdk": str(sdk), "fingerprint": args.fingerprint,
        "sdk_identity_file": identity(output / "sdk-identities.json"), "sources": source_ids,
        "method": {
            "operation": "One complete ReadScene(path) per fresh process; no asset cache",
            "read_seconds": "File open/read, checksum, transport and semantic decode, construction, validation; excludes startup and scene destruction",
            "process_seconds": "Whole process startup, read, output, destruction and exit",
            "os_cache": "Warm OS file cache: full copy/preflight and one unreported warmup per package; no OS cache flush",
            "memory": "GetProcessMemoryInfo OS lifetime PeakWorkingSetSize/PeakPagefileUsage; 1 ms polling plus query after process exit",
            "comparison": "Full public SceneValueComparison only in separate prepare/verify processes",
            "baseline": "Same executable baseline mode; no scene/package opened, no subtraction from reported absolute peaks",
            "order": "Variant order rotates by round, reversing on odd rounds; scenes interleave; fresh baseline each round",
            "repetitions": args.runs, "import_workers": 2, "codec_threads": 1,
            "zstandard_level": 3, "processing": "No CookScene, quantization, image compression, mip generation or fetch optimization",
            "limitations": "Uncontrolled OS/CPU cache and clock state; lifetime CPU times may quantize for short processes; no GPU/engine work; no pure decoder isolation"
        }, "cases": [], "baseline": {"runs": []}, "execution_order": []
    }
    for name, source_text in args.model:
        if not name.replace("_", "").replace("-", "").isalnum() or any(c["name"] == name for c in document["cases"]):
            raise ValueError("Names must be unique simple identifiers")
        source = Path(source_text).resolve()
        check_embedded_glb(source)
        directory = output / name
        directory.mkdir()
        copied = directory / "source.glb"
        shutil.copy2(source, copied)
        case = {"name": name, "source": identity(source), "copied_input": identity(copied), "variants": {}}
        case["prepare"] = run(probe, ["prepare", copied, directory], logs / f"{name}-prepare.log", output, args.fingerprint)
        scenes = records(logs / f"{name}-prepare.log", "SCENE ")
        chunks = records(logs / f"{name}-prepare.log", "CHUNKS ")
        if len(scenes) != 1 or {entry["variant"] for entry in chunks} != set(VARIANTS):
            raise ValueError("Incomplete correctness preflight")
        case["scene"] = scenes[0]
        absent = copied.with_suffix(".glb.absent")
        copied.rename(absent)
        case["retained_source"] = identity(absent)
        if copied.exists():
            raise ValueError("Recorded source path was not removed")
        case["source_absent_verify"] = run(probe, ["verify", directory], logs / f"{name}-verify.log", output, args.fingerprint)
        for variant in VARIANTS:
            package = directory / f"{variant}.zenasset"
            audit = next(row for row in chunks if row["variant"] == variant)
            case["variants"][variant] = {
                "package": identity(package), "directory": audit,
                "stored_resource_bytes": sum(row["stored_bytes"] for row in audit["resources"]),
                "transport_decoded_resource_bytes": sum(row["transport_decoded_bytes"] for row in audit["resources"]),
                "runs": []
            }
        document["cases"].append(case)
        write(output / "results.json", document)
        print(f"{name}: imported/source-free full-scene equality and actual encodings pass", flush=True)
    document["baseline"]["warmup"] = run(probe, ["baseline"], logs / "baseline-warmup.log", output, args.fingerprint)
    for case in document["cases"]:
        for variant in VARIANTS:
            item = case["variants"][variant]
            item["warmup"] = run(probe, ["read", item["package"]["path"]],
                                 logs / f"{case['name']}-{variant}-warmup.log", output, args.fingerprint)
    for iteration in range(args.runs):
        document["baseline"]["runs"].append(run(probe, ["baseline"], logs / f"baseline-{iteration}.log", output, args.fingerprint))
        order = list(VARIANTS[iteration % 4:] + VARIANTS[:iteration % 4])
        if iteration % 2:
            order.reverse()
        cases = document["cases"] if iteration % 2 == 0 else list(reversed(document["cases"]))
        for variant in order:
            for case in cases:
                item = case["variants"][variant]
                result = run(probe, ["read", item["package"]["path"]],
                             logs / f"{case['name']}-{variant}-{iteration}.log", output, args.fingerprint)
                for key, value in case["scene"].items():
                    if result[key] != value:
                        raise ValueError("Measured scene counters differ from correctness preflight")
                result["round"] = iteration
                item["runs"].append(result)
                document["execution_order"].append({"round": iteration, "case": case["name"], "variant": variant, "pid": result["pid"]})
        write(output / "results.json", document)
        print(f"Measured round {iteration + 1}/{args.runs} complete", flush=True)
    for case in document["cases"]:
        for variant, item in case["variants"].items():
            item["summary"] = summarize(item["runs"])
            if identity(Path(item["package"]["path"]))["sha256"] != item["package"]["sha256"]:
                raise ValueError("Measured package changed")
        if identity(Path(case["source"]["path"]))["sha256"] != case["source"]["sha256"]:
            raise ValueError("Original source changed")
    document["baseline"]["summary"] = summarize(document["baseline"]["runs"])
    for entry in source_ids + sdk_ids + [document["probe"]]:
        if identity(Path(entry["path"]))["sha256"] != entry["sha256"]:
            raise ValueError(f"Source, SDK, or executable changed: {entry['path']}")
    document["finished_utc"] = datetime.now(timezone.utc).isoformat()
    document["passed"] = True
    document["identities_unchanged"] = True
    write(output / "results.json", document)


if __name__ == "__main__":
    main()

