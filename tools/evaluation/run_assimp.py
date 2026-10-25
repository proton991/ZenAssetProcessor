"""Evaluate selected Assimp PLY/STL mappings; this does not enable an importer."""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import subprocess


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--probe", type=Path, required=True)
    parser.add_argument("--source", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    fixtures = args.output / "fixtures"
    fixtures.mkdir(exist_ok=True)
    positions = [(0.0, 0.0, 0.0), (2.0, 0.0, 0.0), (0.0, 3.0, 0.0)]
    colors = [(255, 0, 0), (0, 255, 0), (0, 0, 255)]
    header = (
        "ply\nformat {encoding} 1.0\nelement vertex 3\n"
        "property float x\nproperty float y\nproperty float z\n"
        "property float nx\nproperty float ny\nproperty float nz\n"
        "property uchar red\nproperty uchar green\nproperty uchar blue\n"
        "element face 1\nproperty list uchar uint vertex_indices\nend_header\n"
    )
    for endian, encoding in [("<", "binary_little_endian"), (">", "binary_big_endian")]:
        payload = header.format(encoding=encoding).encode("ascii")
        for position, color in zip(positions, colors):
            payload += struct.pack(endian + "6f3B", *position, 0.0, 0.0, 1.0, *color)
        payload += struct.pack(endian + "B3I", 3, 0, 1, 2)
        (fixtures / f"triangle_{encoding}.ply").write_bytes(payload)
    ascii_rows = [" ".join(str(v) for v in (*p, 0, 0, 1, *c)) for p, c in zip(positions, colors)]
    ascii_ply = header.format(encoding="ascii") + "\n".join(ascii_rows) + "\n3 0 1 2\n"
    (fixtures / "triangle_ascii.ply").write_text(ascii_ply, encoding="ascii")
    (fixtures / "invalid_index.ply").write_text(ascii_ply.replace("3 0 1 2", "3 0 1 99"), encoding="ascii")
    binary_stl = b"ZenAsset static geometry evaluation".ljust(80, b"\0") + struct.pack("<I", 1)
    binary_stl += struct.pack("<12fH", 0, 0, 1, *positions[0], *positions[1], *positions[2], 0)
    (fixtures / "triangle_binary.stl").write_bytes(binary_stl)
    (fixtures / "truncated_binary.stl").write_bytes(binary_stl[:-10])
    cases = []
    for name in ["triangle_ascii.ply", "triangle_binary_little_endian.ply", "triangle_binary_big_endian.ply"]:
        cases.append((fixtures / name, "raw", "colored_triangle"))
    cases.extend([
        (fixtures / "triangle_binary.stl", "raw", "binary_triangle"),
        (fixtures / "invalid_index.ply", "raw", "reject"),
        (fixtures / "truncated_binary.stl", "raw", "reject"),
        (args.source / "test/models/STL/triangle.stl", "raw", "ascii_stl"),
        (args.source / "test/models/STL/triangle_with_two_solids.stl", "raw", "two_solids"),
        (args.source / "test/models/STL/triangle_with_empty_solid.stl", "raw", "empty_solid"),
        (args.source / "test/models/PLY/points.ply", "raw", "points"),
        (args.source / "test/models/PLY/cube_uv.ply", "raw", "quad_uv"),
        (args.source / "test/models/PLY/cube_uv.ply", "triangulate", "tri_uv"),
        (args.source / "test/models/PLY/cube_binary.ply", "triangulate", "accepted"),
        (args.source / "test/models/PLY/float-color.ply", "triangulate", "accepted"),
        (args.source / "test/models/STL/Spider_ascii.stl", "raw", "accepted"),
        (args.source / "test/models/STL/Spider_binary.stl", "raw", "accepted"),
    ])
    results = []
    for path, mode, expected in cases:
        process = subprocess.run([str(args.probe), str(path), mode], capture_output=True, text=True, timeout=30)
        result = json.loads(process.stdout)
        checks = {"exit_agrees": process.returncode == (0 if result["accepted"] else 1)}
        checks["expected_acceptance"] = result["accepted"] == (expected != "reject")
        meshes = result.get("mesh_data", [])
        if result["accepted"]:
            checks["static_only"] = result["animations"] == result["cameras"] == result["lights"] == 0
            checks["no_deformation"] = all(m["bones"] == m["morphs"] == 0 for m in meshes)
        if expected in ("colored_triangle", "binary_triangle") and meshes:
            mesh = meshes[0]
            checks["triangle_counts"] = result["meshes"] == 1 and mesh["vertices"] == 3 and mesh["faces"] == 1
            checks["positions"] = [s["position"] for s in mesh["samples"]] == [list(p) for p in positions]
            checks["normals"] = all(s.get("normal") == [0, 0, 1] for s in mesh["samples"])
            if expected == "colored_triangle":
                checks["colors"] = [s.get("color") for s in mesh["samples"]] == [
                    [1, 0, 0, 1], [0, 1, 0, 1], [0, 0, 1, 1]]
        if expected == "ascii_stl" and meshes:
            checks["positions"] = [s["position"] for s in meshes[0]["samples"]] == [[1, 1, 0], [-1, 1, 0], [0, -1, 0]]
        if expected == "two_solids":
            checks["solids"] = result.get("meshes") == 2 and result.get("root_children") == 2
            checks["names"] = [m["name"] for m in meshes] == ["testTriangle_1", "testTriangle_2"]
        if expected == "empty_solid":
            checks["nonempty_mesh"] = len(meshes) == 1 and meshes[0]["vertices"] == 3
        if expected == "points" and meshes:
            checks["points"] = meshes[0]["vertices"] == 4 and meshes[0]["primitive_types"] == 1
            checks["channels"] = bool(meshes[0]["colors"] and meshes[0]["normals"])
        if expected in ("quad_uv", "tri_uv") and meshes:
            checks["faces"] = meshes[0]["faces"] == (6 if expected == "quad_uv" else 12)
            checks["channels"] = meshes[0]["uv_channels"] == 1 and bool(meshes[0]["normals"])
            checks["uv_values"] = [s["uv"] for s in meshes[0]["samples"]] == [[0, 0, 0], [1, 0, 0], [1, 1, 0], [0, 1, 0]]
        results.append({"path": str(path), "sha256": digest(path), "mode": mode, "expectation": expected,
                        "result": result, "checks": checks, "passed": all(checks.values()), "stderr": process.stderr})
    record = {
        "scope": "Assimp parser evaluation only; no ZenAsset backend or runtime support enabled",
        "assimp_commit": subprocess.check_output(["git", "-C", str(args.source), "rev-parse", "HEAD"], text=True).strip(),
        "probe_sha256": digest(args.probe), "passed": sum(r["passed"] for r in results),
        "total": len(results), "results": results,
    }
    (args.output / "results.json").write_text(json.dumps(record, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({"passed": record["passed"], "total": len(results),
                      "failed": [str(r["path"]) for r in results if not r["passed"]]}))
    return 0 if record["passed"] == len(results) else 1


if __name__ == "__main__":
    raise SystemExit(main())
