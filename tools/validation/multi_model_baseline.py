"""Bounded, serial processor-only Sponza baseline; never launches an engine.

Fresh processes use the existing Windows lifetime memory sampler. These runs do
not flush filesystem caches. A timeout or memory abort is evidence of failure,
not a successful timing sample. Generated inputs and outputs stay below build/.
"""

import argparse
from datetime import datetime, timezone
import json
from pathlib import Path, PurePosixPath
import shutil
import subprocess
import time
import zipfile
from urllib.parse import unquote

from asset_processor_baseline import identity, memory

MODELS = {
    "main": "NewSponza_Main_glTF_003.gltf",
    "curtains": "pkg_a_curtains/NewSponza_Curtains_glTF.gltf",
    "ivy": "pkg_b_ivy1/pkg_b_ivy/NewSponza_IvyGrowth_glTF.gltf",
    "trees": "pkg_c_trees/NewSponza_CypressTree_glTF.gltf",
}


def write(path, value):
    path.write_text(json.dumps(value, indent=2) + "\n", encoding="utf-8")


def extract_candles(root, output):
    source = root / "pkg_d_10k_candles.zip"
    extracted = []
    with zipfile.ZipFile(source) as archive:
        members = [
            item
            for item in archive.infolist()
            if PurePosixPath(item.filename).suffix.lower() in (".gltf", ".bin")
        ]
        if len(members) != 2 or sum(item.file_size for item in members) > 1 << 30:
            raise ValueError("Expected one candle glTF/BIN pair below 1 GiB")
        for member in members:
            relative = PurePosixPath(member.filename)
            destination = (output / relative).resolve()
            if (
                relative.is_absolute()
                or ".." in relative.parts
                or not destination.is_relative_to(output)
            ):
                raise ValueError("Archive member escapes generated input directory")
            destination.parent.mkdir(parents=True, exist_ok=True)
            temporary = destination.with_suffix(destination.suffix + ".extracting")
            with archive.open(member) as reader, temporary.open("xb") as writer:
                shutil.copyfileobj(reader, writer, 1 << 20)
            temporary.replace(destination)
            extracted.append(
                {
                    **identity(destination),
                    "member": member.filename,
                    "crc32": member.CRC,
                }
            )
    write(output / "extracted-inputs.json", extracted)
    return next(
        Path(item["path"]) for item in extracted if item["path"].endswith(".gltf")
    )


def disk_bytes(root):
    total = 0
    for path in root.rglob("*"):
        try:
            if path.is_file():
                total += path.stat().st_size
        except FileNotFoundError:
            # Staging files can be renamed between enumeration and stat.
            pass
    return total


def measure(command, log, cwd, timeout, memory_limit, disk_root=None):
    started = time.monotonic()
    result = {
        "command": [str(part) for part in command],
        "started_utc": datetime.now(timezone.utc).isoformat(),
        "log": str(log),
        "aborted": None,
    }
    if disk_root is not None:
        result["initial_disk_bytes"] = disk_bytes(disk_root)
        result["peak_sampled_disk_bytes"] = result["initial_disk_bytes"]
    next_disk_sample = started
    with log.open("w", encoding="utf-8") as stream:
        process = subprocess.Popen(
            result["command"], cwd=cwd, stdout=stream, stderr=subprocess.STDOUT
        )
        while process.poll() is None:
            for key, value in memory(process).items():
                result[key] = max(value, result.get(key, 0))
            if disk_root is not None and time.monotonic() >= next_disk_sample:
                result["peak_sampled_disk_bytes"] = max(
                    result["peak_sampled_disk_bytes"], disk_bytes(disk_root)
                )
                next_disk_sample = time.monotonic() + 1
            if result.get("peak_private_commit_bytes", 0) > memory_limit:
                result["aborted"] = "private_commit_limit"
            elif time.monotonic() - started > timeout:
                result["aborted"] = "time_limit"
            if result["aborted"]:
                process.kill()
                break
            time.sleep(0.02)
        process.wait()
        for key, value in memory(process).items():
            result[key] = max(value, result.get(key, 0))
        result["exit_code"] = process.returncode
    result["wall_seconds"] = time.monotonic() - started
    if disk_root is not None:
        result["final_disk_bytes"] = disk_bytes(disk_root)
        result["peak_sampled_disk_bytes"] = max(
            result["peak_sampled_disk_bytes"], result["final_disk_bytes"]
        )
    result["successful"] = result["exit_code"] == 0 and result["aborted"] is None
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, required=True)
    parser.add_argument("--compiler", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument(
        "--models",
        nargs="+",
        choices=[*MODELS, "candles"],
        default=[*MODELS, "candles"],
    )
    parser.add_argument("--timeout", type=float, default=600)
    parser.add_argument("--memory-gib", type=float, default=16)
    parser.add_argument("--quality", type=int, choices=range(5))
    parser.add_argument("--runs", type=int, default=1)
    args = parser.parse_args()
    root, compiler, output = (
        args.root.resolve(),
        args.compiler.resolve(),
        args.output.resolve(),
    )
    workspace = Path(__file__).resolve().parents[2]
    if (
        not output.is_relative_to(workspace / "build")
        or args.runs < 1
        or args.memory_gib <= 0
        or args.timeout <= 0
    ):
        parser.error(
            "Use a new output below processor/build, positive runs/timeout/memory"
        )
    output.mkdir(parents=True, exist_ok=False)
    # Freeze the executable so rebuilds elsewhere cannot change a running campaign.
    frozen = output / compiler.name
    shutil.copy2(compiler, frozen)
    models = {name: root / MODELS[name] for name in args.models if name in MODELS}
    if "candles" in args.models:
        models["candles"] = extract_candles(root, output)
    inputs = set(models.values())
    for source in models.values():
        gltf = json.loads(source.read_text(encoding="utf-8-sig"))
        for item in gltf.get("buffers", []) + gltf.get("images", []):
            uri = item.get("uri", "")
            if uri and not uri.startswith("data:"):
                inputs.add((source.parent / unquote(uri)).resolve())
    document = {
        "compiler": identity(frozen),
        "invocation": list(__import__("sys").argv),
        "method": "Serial fresh processes; uncontrolled/warm OS cache; absolute process peaks; no rendering",
        "sources": [identity(path) for path in models.values()],
        "inputs": [identity(path) for path in sorted(inputs)],
        "samples": [],
    }
    for repetition in range(args.runs):
        for name, source in models.items():
            package = output / f"{name}-{repetition}.zenasset"
            command = [
                frozen,
                "cook",
                source,
                "--output",
                package,
                "--workers",
                "2",
                "--image-budget",
                str(16 << 30),
                "--geometry-budget",
                str(16 << 30),
                "--allocation-budget",
                str(16 << 30),
                "--compression",
                "zstd",
                "--zstd-level",
                "3",
                "--mesh-compression",
                "meshopt",
            ]
            if args.quality is not None:
                command += [
                    "--profile",
                    "desktop",
                    "--texture-mipmaps",
                    "on",
                    "--texture-compression",
                    "bc7",
                    "--normal-bc5",
                    "on",
                    "--occlusion-bc4",
                    "on",
                    "--texture-quality",
                    str(args.quality),
                ]
            sample = measure(
                command,
                output / f"{name}-{repetition}.log",
                output,
                args.timeout,
                int(args.memory_gib * (1 << 30)),
            )
            sample.update(model=name, repetition=repetition)
            if sample["successful"]:
                sample["package"] = identity(package)
            sample["disk_bytes"] = sum(
                path.stat().st_size for path in output.rglob("*") if path.is_file()
            )
            document["samples"].append(sample)
            write(output / "results.json", document)
            print(json.dumps(sample), flush=True)
    return int(not all(sample["successful"] for sample in document["samples"]))


if __name__ == "__main__":
    raise SystemExit(main())
