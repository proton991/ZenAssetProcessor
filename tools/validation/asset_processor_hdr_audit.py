"""Independent Environment-v2 audit using Microsoft's CPU DirectXTex decoder.

Raw storage isolates BC6H from transport codecs. Sources are hidden during
Reader-only validation/roundtrip. Each authored face/mip has its own HDR gate;
no average can hide a failed face or tail. Existing texture evidence is untouched.
"""
import argparse
import json
import math
from pathlib import Path
from asset_processor_paths import PROCESSOR_ROOT, VALIDATION_TOOLS
import shutil
import struct

import numpy as np

import asset_processor_baseline as baseline
from asset_processor_quantization import Fields, package

ROOT = PROCESSOR_ROOT
FORMATS = {6: "RGBA32Float", 11: "BC6HUfloat"}
EXPECTED = {"tiny": 11, "odd3": 11, "odd5partial": 11, "odd7full": 11, "black": 11,
            "darktail": 6, "negative": 6, "overflow": 6, "alpha": 6, "strict": 6, "dimension": 6}


def environments(resources):
    result = {}
    for (kind, index), data in resources.items():
        if kind == 8:
            fields = Fields(data)
            name = fields.string()
            intensity, rotation = fields.unpack("f"), fields.unpack("4f")
            sh_count = fields.array(12)
            prefix = data[:fields.offset]
            fmt, size, mips = fields.unpack("III")
            if fmt not in FORMATS:
                raise ValueError(f"Unsupported audited environment format {fmt}")
            faces = []
            for face in range(fields.unpack("Q")):
                extent = max(1, size >> (face // 6))
                count = fields.unpack("Q")
                byte_count = count * (16 if fmt == 6 else 1)
                expected = extent * extent * 16 if fmt == 6 else ((extent + 3) // 4) ** 2 * 16
                if byte_count != expected:
                    raise ValueError("Wrong authored environment face footprint")
                faces.append(data[fields.offset:fields.offset + byte_count])
                fields.skip(byte_count)
            fields.finish()
            if len(faces) != mips * 6 or size < 1 or not 1 <= mips <= size.bit_length():
                raise ValueError("Invalid cube shape or authored mip chain")
            result[index] = dict(name=name, intensity=intensity, rotation=rotation, sh_count=sh_count,
                                 prefix=prefix, format=fmt, size=size, mip_levels=mips, faces=faces)
    return result


def write_bc6h_dds(path, size, data):
    header = ([124, 0x81007, size, size, len(data), 0, 1] + [0] * 11
              + [32, 4, struct.unpack("<I", b"DX10")[0], 0, 0, 0, 0, 0] + [0x1000, 0, 0, 0, 0])
    # DXGI_FORMAT_BC6H_UF16=95, D3D10_RESOURCE_DIMENSION_TEXTURE2D=3.
    path.write_bytes(b"DDS " + struct.pack("<31I", *header) + struct.pack("<5I", 95, 3, 0, 1, 0) + data)


def read_float_dds(path, size):
    data = path.read_bytes()
    if (data[:4] != b"DDS " or struct.unpack_from("<I", data, 4)[0] != 124
            or struct.unpack_from("<II", data, 12) != (size, size)
            or data[84:88] != b"DX10" or struct.unpack_from("<4I", data, 128) != (2, 3, 0, 1)
            or len(data) != 148 + size * size * 16):
        raise ValueError("Unexpected DirectXTex floating-point DDS layout")
    values = np.frombuffer(data, dtype="<f4", offset=148).reshape(size, size, 4)
    if not np.all(np.isfinite(values)):
        raise ValueError("Independent decoder produced nonfinite values")
    return values


def checked_run(command, log, result, timeout=600):
    run = baseline.run(command, log, ROOT, timeout)
    result["runs"].append(run)
    if run["exit_code"] != 0 or run["timed_out"]:
        raise RuntimeError(f"Process failed: {log}")
    return run


def decode_cube(cube, directory, texconv, result):
    inputs, outputs = directory / "encoded-dds", directory / "decoded-dds"
    inputs.mkdir()
    outputs.mkdir()
    files = []
    for face, data in enumerate(cube["faces"]):
        path = inputs / f"mip-{face // 6}-face-{face % 6}.dds"
        write_bc6h_dds(path, max(1, cube["size"] >> (face // 6)), data)
        files.append(path)
    checked_run([texconv, "-nologo", "-f", "R32G32B32A32_FLOAT", "-m", "1", "-nogpu", "-dx10", "-o", outputs, *files],
                directory / "directxtex.log", result)
    return [read_float_dds(outputs / path.name, max(1, cube["size"] >> (face // 6)))
            for face, path in enumerate(files)]


def audit_case(name, directory, args, result, limit):
    paths = {profile: directory / f"{profile}.zenasset" for profile in ("default", "reference", "bc6h", "repeat")}
    resources = {profile: package(path) for profile, path in paths.items()}
    reference, cooked = resources["reference"], resources["bc6h"]
    defaults_equal = resources["default"] == reference
    deterministic = resources["repeat"] == cooked
    nonenvironment_equal = reference.keys() == cooked.keys() and all(
        reference[key] == cooked[key] for key in reference if key[0] not in (8, 9))
    original, encoded = environments(reference), environments(cooked)
    entry = dict(name=name, packages={key: baseline.identity(path) for key, path in paths.items()},
                 default_reference_equal=defaults_equal, repeated_cook_resource_equal=deterministic,
                 nonenvironment_resources_equal=nonenvironment_equal, cubes=[], source_free_roundtrips=[])
    valid = defaults_equal and deterministic and nonenvironment_equal and original.keys() == encoded.keys()
    for index, cube in encoded.items():
        source = original[index]
        preserved = (cube["prefix"] == source["prefix"] and cube["size"] == source["size"]
                     and cube["mip_levels"] == source["mip_levels"])
        raw_bytes, cooked_bytes = sum(map(len, source["faces"])), sum(map(len, cube["faces"]))
        measurement = dict(index=index, name=cube["name"], size=cube["size"], authored_mip_levels=cube["mip_levels"],
                           face_count=len(cube["faces"]), format=FORMATS[cube["format"]],
                           lighting_and_authored_shape_preserved=preserved, raw_pixel_bytes=raw_bytes,
                           cooked_pixel_bytes=cooked_bytes, upload_payload_bytes=cooked_bytes, quality=[])
        valid &= preserved and cube["format"] == EXPECTED.get(name, cube["format"])
        if cube["format"] == 6:
            measurement["retained_rgba32_exact"] = cube["faces"] == source["faces"]
            valid &= measurement["retained_rgba32_exact"]
        else:
            decoded = decode_cube(cube, directory, args.texconv, result)
            for face, values in enumerate(decoded):
                extent = values.shape[0]
                authored = np.frombuffer(source["faces"][face], dtype="<f4").reshape(extent, extent, 4)
                a, b = authored[:, :, :3].astype(np.float64), values[:, :, :3].astype(np.float64)
                energy = float(np.sum(a * a))
                error = float(np.sum((a - b) ** 2))
                relative = math.sqrt(error / energy) if energy else (0.0 if error == 0 else math.inf)
                alpha_exact = bool(np.all(authored[:, :, 3] == 1) and np.all(values[:, :, 3] == 1))
                passed = math.isfinite(relative) and relative <= limit and alpha_exact
                measurement["quality"].append(dict(mip=face // 6, face=face % 6, size=extent,
                    relative_rmse=relative, max_absolute_error=float(np.max(np.abs(a-b))),
                    source_min=float(np.min(a)), source_max=float(np.max(a)),
                    decoded_min=float(np.min(b)), decoded_max=float(np.max(b)),
                    alpha_exact=alpha_exact, limit=limit, passed=passed))
                valid &= passed
        entry["cubes"].append(measurement)
    for profile in ("reference", "bc6h"):
        path = paths[profile]
        checked_run([args.cli, "validate", path], directory / f"validate-{profile}.log", result)
        output = directory / f"roundtrip-{profile}.zenasset"
        checked_run([args.roundtrip, path, output], directory / f"roundtrip-{profile}.log", result)
        exact = resources[profile] == package(output)
        entry["source_free_roundtrips"].append(dict(profile=profile, resource_equal=exact,
                                                    output=baseline.identity(output)))
        valid &= exact
    entry["passed"] = bool(valid)
    return entry


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--cli", type=Path, required=True)
    parser.add_argument("--roundtrip", type=Path, required=True)
    parser.add_argument("--fixtures", type=Path, required=True)
    parser.add_argument("--texconv", type=Path, required=True)
    parser.add_argument("--source", type=Path, default=Path("E:/Dev/glTF-Sample-Assets/Models/EnvironmentTest/glTF-IBL/EnvironmentTest.gltf"))
    args = parser.parse_args()
    args.output = args.output.resolve()
    args.output.mkdir(parents=True, exist_ok=False)
    args.cli, args.roundtrip = args.cli.resolve(), args.roundtrip.resolve()
    args.fixtures, args.texconv = args.fixtures.resolve(), args.texconv.resolve()
    settings = json.loads((VALIDATION_TOOLS / "asset_processor_quality.json").read_text())
    limit = settings["desktop_profile"]["texture_encoding"]["hdr_relative_rmse_max"]
    result = dict(method="DirectXTex CPU BC6H_UF16 to R32G32B32A32_FLOAT; separate authored face/mip gates",
                  decoder_source="https://github.com/microsoft/DirectXTex/releases/tag/may2026",
                  hdr_relative_rmse_max=limit, binaries={name: baseline.identity(getattr(args, name))
                      for name in ("cli", "roundtrip", "fixtures", "texconv")},
                  audit=baseline.identity(Path(__file__)), runs=[], cases=[], passed=False,
                  memory_note="Single warm diagnostic process samples; no performance threshold or GPU allocation inferred.")
    output = args.output / "results.json"
    original_copy, hidden_copy = args.output / "source", args.output / "source-hidden"
    try:
        checked_run([args.fixtures, args.output], args.output / "fixtures.log", result)
        corpus_dir = args.output / "EnvironmentTest"
        corpus_dir.mkdir()
        shutil.copytree(args.source.parent, original_copy)
        source = original_copy / args.source.name
        result["source_dependencies"] = [baseline.identity(path) for path in sorted(original_copy.rglob("*")) if path.is_file()]
        for profile in ("default", "reference", "bc6h", "repeat"):
            command = [args.cli, "cook", source, "--output", corpus_dir / f"{profile}.zenasset", "--compression", "none"]
            if profile != "default":
                command += ["--profile", "reference" if profile == "reference" else "desktop"]
            if profile in ("bc6h", "repeat"):
                command += ["--environment-bc6h", "on"]
            checked_run(command, corpus_dir / f"cook-{profile}.log", result)
        if not original_copy.resolve().is_relative_to(args.output):
            raise ValueError("Source copy escaped the audit directory")
        original_copy.rename(hidden_copy)
        result["source_directory_absent_during_read"] = not original_copy.exists()
        for name in [*EXPECTED, "EnvironmentTest"]:
            entry = audit_case(name, args.output / name, args, result, 0.0 if name == "strict" else limit)
            result["cases"].append(entry)
            baseline.write(output, result)
        result["passed"] = all(case["passed"] for case in result["cases"])
    except Exception as error:
        result["error"] = repr(error)
        raise
    finally:
        if hidden_copy.exists() and not original_copy.exists():
            hidden_copy.rename(original_copy)
        result["source_copy_restored"] = original_copy.exists()
        baseline.write(output, result)
    print(json.dumps({"passed": result["passed"], "cases": len(result["cases"]), "output": str(output)}))
    return 0 if result["passed"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
