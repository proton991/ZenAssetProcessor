"""Run the independent per-corner fetch-order equivalence gate."""
import argparse
import hashlib
import json
import pathlib
import subprocess
import shutil
import time


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--executable", required=True, type=pathlib.Path)
    parser.add_argument("--manifest", required=True, type=pathlib.Path)
    parser.add_argument("--output", required=True, type=pathlib.Path)
    args = parser.parse_args()
    manifest = json.loads(args.manifest.read_text(encoding="utf-8-sig"))
    sources = [row["source"] for row in manifest["results"] if row["accepted"]]
    if len(sources) != len(set(sources)):
        raise ValueError("duplicate source path in corpus manifest")
    executable = args.output.with_suffix(".exe")
    shutil.copyfile(args.executable, executable)
    root = pathlib.Path(__file__).resolve().parent.parent
    code_files = ["tests/GeometryCorpusTests.cpp", "src/Processing/GeometryProcessing.cpp",
                  "src/Processor/Cook.cpp", "include/ZenAsset/Format/Processing.h"]
    report = {
        "scope": "Lossless vertex-fetch cooking only; quantization disabled. Compare every original/cooked primitive corner's full vertex bits, all UV sets, full influences, authored source resolution, custom attributes and unchanged scene records.",
        "manifest": str(args.manifest.resolve()),
        "manifest_sha256": hashlib.sha256(args.manifest.read_bytes()).hexdigest(),
        "executable": str(executable.resolve()),
        "executable_sha256": hashlib.sha256(executable.read_bytes()).hexdigest(),
        "code_sha256": {name: hashlib.sha256((root / name).read_bytes()).hexdigest() for name in code_files},
        "expected": len(sources),
        "results": [],
    }
    started = time.monotonic()
    for index, source in enumerate(sources, 1):
        row = {"source": source}
        try:
            execution = subprocess.run([str(executable)], input=(source + "\n").encode("utf-8"),
                                       stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=300)
            row.update(json.loads(execution.stdout.decode("utf-8")))
            row["exit_code"] = execution.returncode
            row["ok"] = row["ok"] and execution.returncode == 0
            if execution.stderr:
                row["stderr"] = execution.stderr.decode("utf-8", errors="replace")
        except (subprocess.TimeoutExpired, json.JSONDecodeError, UnicodeError) as error:
            row.update(ok=False, failure=str(error))
        report["results"].append(row)
        report["elapsed_seconds"] = time.monotonic() - started
        report["passed"] = sum(bool(item["ok"]) for item in report["results"])
        report["failed"] = [item["source"] for item in report["results"] if not item["ok"]]
        args.output.write_text(json.dumps(report, indent=2), encoding="utf-8")
        print(f"{index}/{len(sources)} passed={report['passed']} failed={len(report['failed'])}", flush=True)
    fields = ("primitives", "corners", "remapped_corners", "source_domain_corners",
              "influence_corners", "custom_components", "warnings", "import_ms", "cook_ms", "verify_ms")
    report["totals"] = {name: sum(row.get(name, 0) for row in report["results"]) for name in fields}
    report["changed_sources"] = sum(row.get("remapped_corners", 0) > 0 for row in report["results"])
    args.output.write_text(json.dumps(report, indent=2), encoding="utf-8")
    return 0 if not report["failed"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
