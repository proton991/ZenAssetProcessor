"""Freeze M0 asset behavior using existing engine binaries; no engine build edits.

Outputs include immutable binary identities, source manifests, test XML, process
logs, Windows peak process memory, retained v2 packages, and source-free reloads.
Each run requires a new output directory so a baseline cannot be overwritten.
"""

import argparse
import ctypes
from ctypes import wintypes
from datetime import datetime, timezone
import hashlib
import json
from pathlib import Path
from asset_processor_paths import PROCESSOR_ROOT, ENGINE_ROOT, INTEGRATION_ROOT, CORPUS, VALIDATION_TOOLS
import platform
import shutil
import struct
import subprocess
import sys
import time
from urllib.parse import unquote, urlsplit


def identity(path):
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for block in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(block)
    return {"path": str(path), "bytes": path.stat().st_size,
            "mtime_utc": datetime.fromtimestamp(path.stat().st_mtime, timezone.utc).isoformat(),
            "sha256": digest.hexdigest()}


class ProcessMemory(ctypes.Structure):
    _fields_ = [("cb", wintypes.DWORD), ("PageFaultCount", wintypes.DWORD)] + [
        (name, ctypes.c_size_t) for name in (
            "PeakWorkingSetSize", "WorkingSetSize", "QuotaPeakPagedPoolUsage",
            "QuotaPagedPoolUsage", "QuotaPeakNonPagedPoolUsage", "QuotaNonPagedPoolUsage",
            "PagefileUsage", "PeakPagefileUsage", "PrivateUsage")]


def memory(process):
    data = ProcessMemory()
    data.cb = ctypes.sizeof(data)
    api = ctypes.WinDLL("psapi").GetProcessMemoryInfo
    api.argtypes = [wintypes.HANDLE, ctypes.c_void_p, wintypes.DWORD]
    api.restype = wintypes.BOOL
    if not api(wintypes.HANDLE(int(process._handle)), ctypes.byref(data), data.cb):
        return {}
    return {"peak_working_set_bytes": data.PeakWorkingSetSize,
            "peak_private_commit_bytes": data.PeakPagefileUsage,
            "sampled_private_bytes": data.PrivateUsage}


def run(command, output, cwd, timeout=600):
    command = [str(item) for item in command]
    started = time.monotonic()
    record = {"command": command, "cwd": str(cwd), "started_utc": datetime.now(timezone.utc).isoformat(),
              "log": str(output), "timed_out": False}
    with output.open("w", encoding="utf-8") as log:
        process = subprocess.Popen(command, cwd=cwd, stdout=log, stderr=subprocess.STDOUT)
        record["pid"] = process.pid
        while process.poll() is None:
            for key, value in memory(process).items():
                record[key] = max(record.get(key, 0), value)
            if time.monotonic() - started > timeout:
                process.kill()
                record["timed_out"] = True
            time.sleep(0.02)
        for key, value in memory(process).items():
            record[key] = max(record.get(key, 0), value)
        record["exit_code"] = process.wait()
    record["wall_seconds"] = round(time.monotonic() - started, 6)
    content = output.read_text(encoding="utf-8", errors="replace")
    record["load_results"] = [line for line in content.splitlines() if line.startswith("ZEN_SCENE_LOAD ")]
    for line in content.splitlines():
        if line.startswith("ZEN_GLTF_IMPORT_RESULT "):
            record["structure"] = json.loads(line.split(" ", 1)[1])
    print(f"{output.stem}: exit={record['exit_code']} seconds={record['wall_seconds']}", flush=True)
    return record


def write(path, value):
    path.write_text(json.dumps(value, indent=2) + "\n", encoding="utf-8")


def capture_dependency_manifest(models, output):
    files = set()
    missing = []
    for source in models:
        files.add(source.resolve())
        data = source.read_bytes()
        if source.suffix.lower() == ".glb":
            length = struct.unpack_from("<I", data, 12)[0]
            data = data[20:20 + length]
        document = json.loads(data)
        for resource in document.get("buffers", []) + document.get("images", []):
            uri = resource.get("uri", "")
            if uri and not urlsplit(uri).scheme:
                dependency = (source.parent / unquote(uri)).resolve()
                if dependency.is_file():
                    files.add(dependency)
                else:
                    missing.append({"model": str(source), "dependency": str(dependency)})
    write(output, {"files": [identity(p) for p in sorted(files)], "missing": missing})


def capture_additional_corpus(root, out, corpus):
    """Record each supplied FBX and retain an external HDR/IBL golden package."""
    release = out / "binaries/release"
    fbx_results = []
    for number, source in enumerate(sorted((CORPUS / "FBX").glob("*.fbx"))):
        fbx_results.append({"source_identity": identity(source), "run": run(
            [release / "GLTFCorpusImport.exe", source], out / "logs" / f"fbx-{number}.log", root)})
    write(out / "fbx-corpus.json", fbx_results)
    if corpus is not None:
        original_dir = corpus / "Models/EnvironmentTest/glTF-IBL"
        if original_dir.is_dir():
            directory = out / "golden/environment-ibl"
            source_dir = directory / "temporary-source"
            shutil.copytree(original_dir, source_dir)
            source = source_dir / "EnvironmentTest.gltf"
            entry = {"inputs": [identity(p) for p in sorted(original_dir.rglob("*")) if p.is_file()]}
            entry["source_structure"] = run([release / "GLTFCorpusImport.exe", source],
                                            out / "logs/ibl-source.log", root)
            entry["benchmark"] = run([release / "SceneLoadBenchmark.exe", source,
                                      f"--cache-dir={directory / 'cache'}", "--warm-runs=3"],
                                     out / "logs/ibl-benchmark.log", root)
            # Only remove the fresh temporary copy, with the absolute target checked.
            if not source_dir.resolve().is_relative_to(out.resolve()):
                raise ValueError("Temporary source directory escaped the baseline output")
            shutil.rmtree(source_dir)
            entry["source_directory_deleted_before_reload"] = not source_dir.exists()
            entry["packages"] = []
            for package in sorted((directory / "cache").glob("*.zenasset")):
                package_entry = identity(package)
                package_entry["source_free_reload"] = run([release / "GLTFCorpusImport.exe", package],
                                                          out / "logs/ibl-source-free.log", root)
                package_entry["structure_matches_source"] = (
                    entry["source_structure"].get("structure") is not None and
                    entry["source_structure"].get("structure") == package_entry["source_free_reload"].get("structure"))
                entry["packages"].append(package_entry)
            write(out / "ibl-golden.json", entry)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--engine", type=Path, default=ENGINE_ROOT)
    parser.add_argument("--build-root", type=Path, default=PROCESSOR_ROOT / "build",
                        help="Parent of zenengine-debug and zenengine-release integration builds")
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--corpus", type=Path)
    parser.add_argument("--sponza", type=Path)
    args = parser.parse_args()
    root = args.engine.resolve()
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=False)
    (out / "logs").mkdir()
    (out / "golden").mkdir()
    snapshot = {}
    for config in ("debug", "release"):
        original = args.build_root.resolve() / f"zenengine-{config}"
        destination = out / "binaries" / config
        destination.mkdir(parents=True)
        names = ["ZenAssetEngineTests.exe", "GLTFCorpusImport.exe", "SceneLoadBenchmark.exe"]
        names.extend(file.name for file in (original / "bin").glob("*.dll"))
        snapshot[config] = []
        for name in names:
            source = original / "bin" / name
            snapshot[config].append(identity(source))
            shutil.copy2(source, destination / name)
        shutil.copy2(original / "CMakeCache.txt", destination / "CMakeCache.txt")
    (out / "baseline-binary-warning.txt").write_text(
        "These are pre-existing binaries, identified by hashes/timestamps. They were not rebuilt from the current dirty tree.\n",
        encoding="utf-8")
    machine = subprocess.check_output([
        "pwsh", "-NoProfile", "-Command",
        "@{ os=Get-CimInstance Win32_OperatingSystem | Select-Object Caption,Version,TotalVisibleMemorySize; "
        "cpu=Get-CimInstance Win32_Processor | Select-Object Name,NumberOfCores,NumberOfLogicalProcessors; "
        "gpu=Get-CimInstance Win32_VideoController | Select-Object Name,DriverVersion } | ConvertTo-Json -Depth 4"
    ], text=True, encoding="utf-8")
    metadata = {"started_utc": datetime.now(timezone.utc).isoformat(), "engine": str(root),
                "python": sys.version, "platform": platform.platform(), "machine": json.loads(machine),
                "binaries": snapshot, "os_cache": "OS cache not flushed; cold means no asset cache, not cold storage",
                "memory_method": "GetProcessMemoryInfo on benchmark process at 20 ms intervals and after exit; OS peak working set and peak private commit, no GPU allocation measurement",
                "benchmark_settings": {"SceneLoader_load_workers": 1, "SceneLoader_processing_workers": 0,
                                       "warm_runs": 3, "SceneImportSettings": "engine defaults"},
                "git_head": subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=root, text=True).strip(),
                "git_status": subprocess.check_output(["git", "status", "--short"], cwd=root, text=True)}
    write(out / "metadata.json", metadata)
    source_files = sorted(set((CORPUS).rglob("*")))
    source_files += sorted((root / "ZenCore/Source/AssetLib").rglob("*"))
    source_files += sorted((root / "ZenCore/Include/AssetLib").rglob("*"))
    source_files += sorted((INTEGRATION_ROOT / "CommonTest").glob("Scene*Tests.cpp"))
    write(out / "source-manifest.json", [identity(path) for path in source_files if path.is_file()])
    results = {"tests": [], "fixtures": [], "benchmarks": [], "corpus": None}
    release = out / "binaries/release"
    gtest_filter = "*Scene*:*Gltf*:*GLTF*:*Fbx*:*FBX*:*Animation*:*Morph*:*PhysicalUnits*"
    for config in ("debug", "release"):
        result = run([out / "binaries" / config / "ZenAssetEngineTests.exe", f"--gtest_filter={gtest_filter}",
                      f"--gtest_output=xml:{out / (config + '-tests.xml')}"],
                     out / "logs" / f"{config}-tests.log", root, 900)
        results["tests"].append(result)
        write(out / "results.json", results)
    fixtures = ["complete_scene.glb", "typed_animation_targets.gltf", "all_primitive_modes.gltf",
                "FBX/blender_293_embedded_textures_7400_binary.fbx", "FBX/combined_morph_skin_ascii.fbx",
                "FBX/maya_instanced_skin_7700_binary.fbx", "FBX/max_instanced_material_7700_ascii.fbx",
                "FBX/maya_camera_light_axes_y_up_7700_ascii.fbx"]
    for number, name in enumerate(fixtures):
        original = CORPUS / name
        directory = out / "golden" / original.stem
        directory.mkdir()
        source = directory / original.name
        shutil.copy2(original, source)
        entry = {"source_identity": identity(original), "temporary_source": str(source)}
        entry["source_structure"] = run([release / "GLTFCorpusImport.exe", source],
                                        out / "logs" / f"fixture-{number}-source.log", root)
        entry["benchmark"] = run([release / "SceneLoadBenchmark.exe", source,
                                  f"--cache-dir={directory / 'cache'}", "--warm-runs=3"],
                                 out / "logs" / f"fixture-{number}-benchmark.log", root)
        source.unlink()
        entry["source_deleted_before_reload"] = not source.exists()
        entry["packages"] = []
        for package in sorted((directory / "cache").glob("*.zenasset")):
            header = package.open("rb").read(48)
            package_entry = identity(package)
            package_entry["header"] = {"magic": header[:8].decode("ascii"),
                                       "schema": struct.unpack_from("<I", header, 8)[0],
                                       "sections_bytes": struct.unpack_from("<QQQ", header, 16)}
            package_entry["source_free_reload"] = run([release / "GLTFCorpusImport.exe", package],
                                                       out / "logs" / f"fixture-{number}-source-free.log", root)
            package_entry["structure_matches_source"] = (
                entry["source_structure"].get("structure") is not None and
                entry["source_structure"].get("structure") == package_entry["source_free_reload"].get("structure"))
            entry["packages"].append(package_entry)
        results["fixtures"].append(entry)
        write(out / "results.json", results)
    if args.sponza:
        capture_dependency_manifest([args.sponza.resolve()], out / "sponza-input-manifest.json")
        results["benchmarks"].append({"source_identity": identity(args.sponza), "run": run(
            [release / "SceneLoadBenchmark.exe", args.sponza.resolve(), f"--cache-dir={out / 'sponza-cache'}", "--warm-runs=3"],
            out / "logs/sponza-benchmark.log", root, 1800)})
        results["benchmarks"][-1]["packages"] = [identity(p) for p in (out / "sponza-cache").glob("*.zenasset")]
        write(out / "results.json", results)
    if args.corpus:
        corpus = args.corpus.resolve()
        models = [p for p in sorted(corpus.rglob("*")) if p.suffix.lower() in {".gltf", ".glb"}]
        write(out / "corpus-model-manifest.json", [identity(p) for p in models])
        capture_dependency_manifest(models, out / "corpus-input-manifest.json")
        results["corpus"] = run([sys.executable, VALIDATION_TOOLS / "verify_gltf_corpus.py", "--assets", corpus,
                                 "--importer", release / "GLTFCorpusImport.exe", "--report", out / "gltf-corpus.json",
                                 "--jobs", "2", "--timeout", "120"], out / "logs/gltf-corpus.log", root, 3600)
    capture_additional_corpus(root, out, args.corpus.resolve() if args.corpus else None)
    results["finished_utc"] = datetime.now(timezone.utc).isoformat()
    write(out / "results.json", results)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
