"""Measure real scene publication, pooled staging and native upload allocation lifetimes.

Fresh means a new process/device; OS and driver caches are uncontrolled. Warm
means five model replacements after one initial load in the same process. CPU
publication includes the production synchronization and replacement work. No
CPU measurement in this report represents GPU execution time.
Optional GPU measurements sum the production staging graph's per-node timestamp
intervals, including prologue barriers. They are instrumented elapsed intervals,
not a whole-publication envelope, GPU occupancy, overlap, or a frame-time benefit.
"""
import argparse
import csv
import json
import math
import os
from pathlib import Path
from asset_processor_paths import ENGINE_ROOT
import re
import shutil
import statistics
import platform
import sys

import asset_processor_baseline as baseline
import asset_processor_upload_benchmark as isolated
import asset_processor_texture_audit as texture
import asset_processor_visual_parity as visual
import asset_processor_texture_visual as texture_visual

ROOT = ENGINE_ROOT


def records(log, label):
    return [json.loads(line.split(label + " ", 1)[1]) for line in log.splitlines() if label + " " in line]


def read_measurements(log, count):
    pool = records(log, "SCENE_UPLOAD_METRICS")
    native = records(log, "SCENE_UPLOAD_NATIVE")
    timing = records(log, "SCENE_LOAD_TIMING")
    assert len(pool) == len(native) == len(timing) == count
    samples = []
    for index, (p, n, t) in enumerate(zip(pool, native, timing)):
        assert p["published"] and t["loaded"] and n["available"] and p["native_allocation_available"]
        assert p["allocation_failures"] == p["pool_wait_failures"] == 0
        assert 0 <= p["occupied_bytes"] <= p["capacity_bytes"] <= p["peak_capacity_bytes"]
        assert p["peak_occupied_bytes"] <= p["peak_capacity_bytes"] <= p["peak_native_allocation_bytes"]
        assert p["native_allocation_bytes"] <= n["host_upload_allocation_bytes"]
        assert p["peak_native_allocation_bytes"] <= n["process_peak_host_upload_allocation_bytes"]
        assert n["transfer_serial_after"] > n["transfer_serial_before"], "This campaign requires accepted transfer-queue work"
        samples.append(dict(iteration=index, **p, **n, **t))
    ended = re.findall(r"GPU host upload allocations: peak_bytes=(\d+) remaining_bytes=(\d+)", log)
    assert len(ended) == 1 and int(ended[0][1]) == 0
    assert "VMA Total device memory leaked: 0 bytes" in log
    assert "[OK] No memory leaks detected" in log
    return dict(samples=samples, process_peak_host_upload_allocation_bytes=int(ended[0][0]),
                route_log=[line for line in log.splitlines() if any(marker in line for marker in
                    ("Upload/transfer in use:", "Upload/transfer support:", "Vulkan queues:", "Render threads:"))])


def summarize(samples):
    fields = ("staged_bytes", "peak_capacity_bytes", "peak_occupied_bytes", "peak_native_allocation_bytes",
              "allocation_flush_requests", "pool_wait_calls", "pool_wait_cpu_ns", "load_cpu_ns", "publish_cpu_ns")
    if all("upload_gpu_us" in row for row in samples):
        fields += ("upload_gpu_us", "upload_gpu_nodes", "upload_gpu_graphs")
    return {field: dict(min=min(row[field] for row in samples), median=statistics.median(row[field] for row in samples),
                        max=max(row[field] for row in samples)) for field in fields}


def read_gpu_measurements(prefix, log, samples):
    profile_path = Path(str(prefix) + ".profile.json")
    passes_path = Path(str(prefix) + ".passes.csv")
    profile = json.loads(profile_path.read_text(encoding="utf-8"))
    assert profile["run_succeeded"] and profile["input_files_unchanged"]
    assert not any(profile["dropped"].values()), profile["dropped"]
    graph_map = {}
    for graph in profile["graphs"]:
        key = (bool(graph["transfer_only"]), graph["execution"])
        assert key not in graph_map, key
        graph_map[key] = graph
    with passes_path.open(newline="", encoding="utf-8") as source:
        passes = list(csv.DictReader(source))
    scopes, current = [], None
    for line in log.splitlines():
        if "SCENE_UPLOAD_PROFILE " in line:
            event = json.loads(line.split("SCENE_UPLOAD_PROFILE ", 1)[1])["event"]
            if event == "begin":
                assert current is None
                current = []
            else:
                assert event == "end" and current is not None
                scopes.append(current)
                current = None
        elif "SCENE_UPLOAD_GRAPH " in line:
            assert current is not None
            current.append(json.loads(line.split("SCENE_UPLOAD_GRAPH ", 1)[1]))
    assert current is None and len(scopes) == len(samples)
    seen = set()
    for sample, scope in zip(samples, scopes):
        upload_graphs, upload_passes = [], []
        for marker in scope:
            key = (bool(marker["transfer_only"]), marker["execution"])
            assert key not in seen, key
            seen.add(key)
            graph = graph_map[key]
            selected = [row for row in passes if int(row["graph_record"]) == graph["record"]]
            assert marker["omitted_nodes"] == graph["omitted_nodes"] == graph["omitted_submissions"] == 0
            assert len(selected) == marker["node_count"] == graph["node_count"]
            assert graph["phase"] == "startup"
            if graph["graph"] == "staging_upload":
                upload_graphs.append(graph)
                upload_passes.extend(selected)
        assert upload_graphs and upload_passes
        for row in upload_passes:
            assert row["gpu_status"] == "available", row
            assert row["pass"] in ("upload_copy", "upload_mipmaps"), row
            assert math.isfinite(float(row["gpu_us"])) and float(row["gpu_us"]) >= 0, row
        assert any(row["queue"] == "transfer" for row in upload_passes)
        sample.update(upload_gpu_us=sum(float(row["gpu_us"]) for row in upload_passes),
                      upload_gpu_nodes=len(upload_passes), upload_gpu_graphs=len(upload_graphs),
                      upload_gpu_queues=sorted(set(row["queue"] for row in upload_passes)),
                      upload_gpu_graph_records=[graph["record"] for graph in upload_graphs])
    return [baseline.identity(Path(str(prefix) + suffix))
            for suffix in (".profile.json", ".passes.csv", ".frames.csv", ".config.cfg")]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--renderer", type=Path, required=True)
    parser.add_argument("--processor", type=Path, required=True)
    parser.add_argument("--reference-config", type=Path, required=True)
    parser.add_argument("--role-split-source", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--gpu-timings", action="store_true", help="Capture and require complete production staging GPU intervals")
    parser.add_argument("--cases", nargs="+", choices=("bc7", "bc5", "pressure-bc7", "role-split"),
                        default=["bc7", "bc5", "pressure-bc7", "role-split"])
    args = parser.parse_args()
    for name in ("renderer", "processor", "reference_config", "role_split_source", "output"):
        setattr(args, name, getattr(args, name).resolve())
    args.output.mkdir(parents=True, exist_ok=False)
    binary_dir = args.output / "binaries"
    binary_dir.mkdir()
    # This machine's existing RTSS no-hook profile; copied bytes are verified.
    renderer = binary_dir / "7zFM.exe"
    processor = binary_dir / "zenassetc.exe"
    shutil.copy2(args.renderer, renderer)
    shutil.copy2(args.processor, processor)
    for dll in args.renderer.parent.glob("*.dll"):
        shutil.copy2(dll, binary_dir / dll.name)
    assert baseline.identity(renderer)["sha256"] == baseline.identity(args.renderer)["sha256"]
    script = args.output / Path(__file__).name
    shutil.copy2(Path(__file__), script)
    template = args.reference_config.read_bytes()
    report = dict(method=__doc__, renderer=baseline.identity(renderer), processor=baseline.identity(processor),
                  script=baseline.identity(script), reference_config=baseline.identity(args.reference_config),
                  policy=baseline.identity(texture.POLICY), cases=[], passed=False,
                  gpu_timings=args.gpu_timings,
                  helpers=[baseline.identity(Path(module.__file__)) for module in (baseline, isolated, texture, visual, texture_visual)],
                  shaders=texture_visual.shader_snapshot(), os=platform.platform(), python=sys.version,
                  pool_native_definition="Sum of native allocation extents of pool-owned buffers; excludes references retained after pool ownership ends.",
                  backend_native_definition="VMA allocation extents for CPU-write transfer-source buffers until actual backend destruction, including retained references; excludes allocator block commitment and driver-private memory.",
                  backend_peak_scope="Cumulative per process; warm replacement samples share this peak and are not independent native peak measurements.",
                  cpu_wait_scope="WaitForSubmittedAllocations only, excluding the preceding flush; CPU time, not GPU execution time.")
    flags = {"bc7": ["--texture-compression", "bc7"], "pressure-bc7": ["--texture-compression", "bc7"],
             "bc5": ["--normal-bc5", "on"], "role-split": ["--occlusion-bc4", "on"]}
    expected = {"bc7": (13, 14), "pressure-bc7": (13, 14), "bc5": (9,), "role-split": (7,)}
    for name in args.cases:
        directory = args.output / name
        directory.mkdir()
        source = (args.role_split_source if name == "role-split" else
                  isolated.fixture(directory / "source", "normal" if name == "bc5" else "color",
                                   4096 if name == "pressure-bc7" else 1024))
        row = dict(case=name, source=baseline.identity(source), packages={}, cooks={}, measurements={})
        for variant in ("raw", "cooked"):
            package = directory / (variant + ".zenasset")
            command = [processor, "cook", source, "--output", package, "--profile", "desktop",
                       "--compression", "none", "--texture-mipmaps", "on", "--workers", "2"]
            if variant == "cooked":
                command += flags[name]
            row["cooks"][variant] = baseline.run(command, directory / (variant + "-cook.log"), ROOT)
            assert row["cooks"][variant]["exit_code"] == 0
            row["packages"][variant] = baseline.identity(package)
            validation = baseline.run([processor, "validate", package], directory / (variant + "-validate.log"), ROOT)
            assert validation["exit_code"] == 0
        images, _, _ = texture.scene_images(texture.package(Path(row["packages"]["cooked"]["path"])))
        row["converted_images"] = sum(image["format"] in expected[name] for image in images.values())
        assert row["converted_images"] > 0, "Compression fell back: " + name
        report["cases"].append(row)
    baseline.capture_dependency_manifest([Path(row["source"]["path"]) for row in report["cases"]], args.output / "source-dependencies.json")
    config = ROOT / "Data/engine.cfg"
    lock = config.with_name(config.name + ".gltf-smoke.lock")
    descriptor = os.open(lock, os.O_CREAT | os.O_EXCL | os.O_WRONLY)
    os.close(descriptor)
    original = config.read_bytes()
    last = original
    (args.output / "original-engine.cfg").write_bytes(original)
    os.environ.update(VK_LAYER_VALIDATE_SYNC="1", VK_LOADER_LAYERS_DISABLE="~implicit~", DISABLE_RTSS_LAYER="1")
    try:
        for row in report["cases"]:
            directory = args.output / row["case"]
            for threaded in (0, 1):
                mode = str(threaded)
                runs = {variant: dict(validation=None, fresh=[], warm=None) for variant in ("raw", "cooked")}
                schedule = [(variant, "validation", 0) for variant in ("raw", "cooked")]
                schedule += [(variant, "fresh", index) for index in range(3)
                             for variant in (("raw", "cooked") if index % 2 == 0 else ("cooked", "raw"))]
                schedule += [(variant, "warm", 0) for variant in ("raw", "cooked")]
                for variant, kind, index in schedule:
                    package = Path(row["packages"][variant]["path"])
                    assert baseline.identity(package) == row["packages"][variant]
                    assert config.read_bytes() == last, "External config edit; refusing to overwrite it"
                    last = template + f"\ncamera_position=0,0,1.2\ndefault_model_path={package.as_posix()}\n".encode()
                    config.write_bytes(last)
                    label = f"{variant}-thread-{mode}-{kind}-{index}"
                    (directory / (label + ".cfg")).write_bytes(last)
                    capture = directory / (label + ".ppm")
                    command = [renderer, "--no-ui", "--no-asset-cache", "--disable-rt", "--fixed-step", "--vsync=0",
                               "--frames=1", "--width=640", "--height=480", "--mode=2", "--gpu-memory-stats",
                               f"--rhi-thread={threaded}", "--enable-validation" if kind == "validation" else "--disable-validation"]
                    if kind == "validation":
                        command += [f"--capture={capture}"]
                    if kind == "warm":
                        command += ["--scene-load-repeats=5"]
                    profile_prefix = directory / (label + "-gpu")
                    if args.gpu_timings:
                        command += [f"--profile={profile_prefix}"]
                    run = baseline.run(command, directory / (label + ".log"), ROOT, timeout=300)
                    assert run["exit_code"] == 0, run
                    log = Path(run["log"]).read_text(encoding="utf-8", errors="replace")
                    assert not any(term in log for term in ("[error]", "VUID-", "SYNC-HAZARD")), label
                    observed = read_measurements(log, 6 if kind == "warm" else 1)
                    if args.gpu_timings:
                        observed["gpu_profile"] = read_gpu_measurements(profile_prefix, log, observed["samples"])
                    observed["process"] = run
                    if kind == "validation":
                        observed["capture"] = baseline.identity(capture)
                    if kind == "fresh":
                        runs[variant][kind].append(observed)
                    else:
                        runs[variant][kind] = observed
                for variant in ("raw", "cooked"):
                    values = runs[variant]
                    values["summary"] = dict(fresh=summarize([run["samples"][0] for run in values["fresh"]]),
                                             warm=summarize(values["warm"]["samples"][1:]))
                comparison = visual.compare(Path(runs["raw"]["validation"]["capture"]["path"]),
                                            Path(runs["cooked"]["validation"]["capture"]["path"]))
                assert comparison["same_dimensions"] and comparison["visible"]
                row["measurements"][mode] = dict(variants=runs, capture_comparison=comparison)
                baseline.write(args.output / "results.json", report)
                print(row["case"], "thread", mode, "passed", flush=True)
        report["measurements_complete"] = all(len(row["measurements"]) == 2 for row in report["cases"])
    finally:
        if config.read_bytes() == last:
            config.write_bytes(original)
        report["configuration_restored"] = config.read_bytes() == original
        lock.unlink(missing_ok=True)
        report["renderer_unchanged"] = baseline.identity(renderer) == report["renderer"]
        report["processor_unchanged"] = baseline.identity(processor) == report["processor"]
        report["helpers_unchanged"] = all(baseline.identity(Path(item["path"])) == item for item in report["helpers"])
        report["shaders_unchanged"] = texture_visual.shader_snapshot() == report["shaders"]
        report["passed"] = all(report.get(key, False) for key in ("measurements_complete", "configuration_restored", "renderer_unchanged", "processor_unchanged", "helpers_unchanged", "shaders_unchanged"))
        baseline.write(args.output / "results.json", report)
    return 0 if report["passed"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
