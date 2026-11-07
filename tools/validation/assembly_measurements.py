"""Measure a native assembly build/reuse/read campaign using one frozen compiler."""

import argparse
import json
from pathlib import Path
import shutil

from asset_processor_baseline import identity
from multi_model_baseline import measure, write


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--compiler", type=Path, required=True)
    parser.add_argument("--manifest", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--timeout", type=float, default=1800)
    parser.add_argument("--fresh-runs", type=int, default=1)
    parser.add_argument("--reuse-runs", type=int, default=5)
    args = parser.parse_args()
    output = args.output.resolve()
    workspace = Path(__file__).resolve().parents[2]
    if (
        not output.is_relative_to(workspace / "build")
        or args.fresh_runs < 1
        or args.reuse_runs < 1
    ):
        parser.error(
            "Use a new directory below processor/build and positive run counts"
        )
    manifest = args.manifest.resolve()
    config = json.loads(manifest.read_text(encoding="utf-8-sig"))
    cooked = (manifest.parent / config["outputDirectory"]).resolve()
    if not cooked.is_relative_to(workspace / "build"):
        parser.error("Measurement cooked output must remain below processor/build")
    output.mkdir(parents=True, exist_ok=False)
    compiler = output / args.compiler.name
    shutil.copy2(args.compiler.resolve(), compiler)
    record = {
        "compiler": identity(compiler),
        "manifest": identity(manifest),
        "samples": [],
        "method": "Serial fresh processes; warm/uncontrolled filesystem cache; no GPU/runtime; 16GiB observed private-commit abort; conservative 48GiB declared stage reservation",
    }
    base = [
        compiler,
        "build",
        manifest,
        "--import-workers",
        "2",
        "--job-memory",
        str(48 << 30),
        "--memory-budget",
        str(48 << 30),
    ]
    steps = [(f"fresh-{i}", base + ["--force", "on"]) for i in range(args.fresh_runs)]
    steps += [(f"reuse-{i}", base) for i in range(args.reuse_runs)]
    steps += [
        (
            f"validate-{i}",
            [
                compiler,
                "validate",
                cooked / config["assembly"],
                "--allocation-budget",
                str(16 << 30),
            ],
        )
        for i in range(args.reuse_runs)
    ]
    steps += [("assembly-only", base + ["--assembly-only", "on"])]
    for label, command in steps:
        sample = measure(
            command,
            output / f"{label}.log",
            workspace,
            args.timeout,
            16 << 30,
            disk_root=cooked,
        )
        sample["step"] = label
        sample["cooked_disk_bytes"] = sum(
            path.stat().st_size for path in cooked.rglob("*") if path.is_file()
        )
        record["samples"].append(sample)
        write(output / "results.json", record)
        print(json.dumps(sample), flush=True)
        if not sample["successful"]:
            break
    record["outputs"] = [
        identity(path) for path in sorted(cooked.rglob("*")) if path.is_file()
    ]
    write(output / "results.json", record)
    return int(not all(sample["successful"] for sample in record["samples"]))


if __name__ == "__main__":
    raise SystemExit(main())
