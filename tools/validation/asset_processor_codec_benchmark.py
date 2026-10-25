"""Compare frozen raw/compressed packages with process CPU and memory evidence.

The CLI validates and decodes each complete scene. Its elapsed measurement
includes process startup/teardown; it does not include engine adaptation or GPU
work. Optional ScenePackageBenchmark output supplies separate in-process read
and adaptation timings. The OS file cache is not flushed.
"""

import argparse
import ctypes
from ctypes import wintypes
from datetime import datetime, timezone
import json
from pathlib import Path
import statistics
import subprocess
import time

from asset_processor_baseline import identity, memory


def process_cpu_seconds(process):
    values = [wintypes.FILETIME() for _ in range(4)]
    api = ctypes.WinDLL("kernel32").GetProcessTimes
    api.argtypes = [wintypes.HANDLE] + [ctypes.POINTER(wintypes.FILETIME)] * 4
    api.restype = wintypes.BOOL
    if not api(wintypes.HANDLE(int(process._handle)), *(ctypes.byref(value) for value in values)):
        raise ctypes.WinError()
    return sum((value.dwHighDateTime << 32) | value.dwLowDateTime for value in values[2:]) / 10_000_000


def measured(command, output, cwd):
    wait = ctypes.WinDLL("kernel32").WaitForSingleObject
    wait.argtypes = [wintypes.HANDLE, wintypes.DWORD]
    wait.restype = wintypes.DWORD
    record = {"command": [str(value) for value in command], "log": str(output)}
    started = time.perf_counter()
    with output.open("w", encoding="utf-8") as stream:
        process = subprocess.Popen(record["command"], cwd=cwd, stdout=stream, stderr=subprocess.STDOUT)
        record["pid"] = process.pid
        while True:
            for key, value in memory(process).items():
                record[key] = max(record.get(key, 0), value)
            status = wait(wintypes.HANDLE(int(process._handle)), 1)
            if status == 0:
                break
            if status != 258:
                raise ctypes.WinError()
            if time.perf_counter() - started > 300:
                process.kill()
                process.wait()
                raise TimeoutError(record["command"])
        record["wall_seconds"] = time.perf_counter() - started
        record["cpu_seconds"] = process_cpu_seconds(process)
        for key, value in memory(process).items():
            record[key] = max(record.get(key, 0), value)
        record["exit_code"] = process.wait()
    if record["exit_code"] != 0:
        raise RuntimeError(f"Benchmark failed; see {output}")
    return record


def summary(runs):
    return {key: {"median": statistics.median(run[key] for run in runs),
                  "min": min(run[key] for run in runs), "max": max(run[key] for run in runs)}
            for key in ("wall_seconds", "cpu_seconds", "peak_working_set_bytes", "peak_private_commit_bytes")}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--pairs", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--runs", type=int, default=7)
    parser.add_argument("--microbenchmark", type=Path)
    args = parser.parse_args()
    if args.runs < 5:
        parser.error("At least five measured repetitions are required")
    manifest = json.loads(args.pairs.read_text(encoding="utf-8"))
    cli = Path(manifest["processor"]["path"])
    if identity(cli)["sha256"] != manifest["processor"]["sha256"]:
        raise ValueError("Frozen CLI no longer matches package producer identity")
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=False)
    (out / "logs").mkdir()
    document = {"started_utc": datetime.now(timezone.utc).isoformat(), "pairs": identity(args.pairs),
                "cli": identity(cli), "method": {
                    "cli_wall": "Full process: startup, package read/decode/validation and teardown; 1 ms kernel event wait",
                    "cpu": "GetProcessTimes kernel + user CPU; short runs may quantize to zero",
                    "memory": "GetProcessMemoryInfo at 1 ms intervals and after exit, OS peak working set/private commit",
                    "os_cache": "One warm-up per package, OS cache not flushed; measured order alternates raw/zstd",
                    "excluded": "No GPU allocation/upload or rendering; CLI has no engine adapter",
                    "measured_repetitions": args.runs}, "rows": []}
    if args.microbenchmark:
        document["microbenchmark"] = identity(args.microbenchmark)
    for pair in manifest["rows"]:
        row = {"case": pair["case"], "raw": {}, "zstd": {}}
        for kind in ("raw", "zstd"):
            package = Path(pair[kind]["path"])
            if identity(package)["sha256"] != pair[kind]["sha256"]:
                raise ValueError(f"Package changed: {package}")
            item = row[kind]
            item["package"] = identity(package)
            item["warmup"] = measured([cli, "validate", package], out / "logs" / f"{pair['case']}-{kind}-warmup.log", out)
            item["runs"] = []
        for index in range(args.runs):
            for kind in (("raw", "zstd") if index % 2 == 0 else ("zstd", "raw")):
                package = Path(pair[kind]["path"])
                row[kind]["runs"].append(measured(
                    [cli, "validate", package], out / "logs" / f"{pair['case']}-{kind}-{index}.log", out))
        for kind in ("raw", "zstd"):
            row[kind]["summary"] = summary(row[kind]["runs"])
            if args.microbenchmark:
                stage_log = out / "logs" / f"{pair['case']}-{kind}-stages.log"
                row[kind]["stage_process"] = measured(
                    [args.microbenchmark, pair[kind]["path"], str(args.runs)], stage_log, out)
                row[kind]["stages"] = [json.loads(line.removeprefix("ZEN_PACKAGE_BENCH "))
                                       for line in stage_log.read_text(encoding="utf-8").splitlines()
                                       if line.startswith("ZEN_PACKAGE_BENCH ")]
                measured_stages = [entry for entry in row[kind]["stages"] if not entry["warmup"]]
                if len(measured_stages) != args.runs:
                    raise ValueError(f"Incomplete stage measurements: {stage_log}")
                row[kind]["stage_summary"] = {
                    name: {"median": statistics.median(entry[name] for entry in measured_stages),
                           "min": min(entry[name] for entry in measured_stages),
                           "max": max(entry[name] for entry in measured_stages)}
                    for name in ("read_seconds", "adapt_seconds")}
        document["rows"].append(row)
        (out / "results.json").write_text(json.dumps(document, indent=2) + "\n", encoding="utf-8")
        print(f"{pair['case']}: raw/zstd validation measurements recorded", flush=True)
    for pair in manifest["rows"]:
        for kind in ("raw", "zstd"):
            if identity(Path(pair[kind]["path"]))["sha256"] != pair[kind]["sha256"]:
                raise ValueError("Measured package changed during run")
    if identity(cli)["sha256"] != document["cli"]["sha256"]:
        raise ValueError("Frozen CLI changed during run")
    if args.microbenchmark and identity(args.microbenchmark)["sha256"] != document["microbenchmark"]["sha256"]:
        raise ValueError("Stage benchmark changed during run")
    document["finished_utc"] = datetime.now(timezone.utc).isoformat()
    document["binary_and_input_hashes_unchanged"] = True
    (out / "results.json").write_text(json.dumps(document, indent=2) + "\n", encoding="utf-8")


if __name__ == "__main__":
    main()
