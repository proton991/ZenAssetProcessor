"""Independent raw-package audit for optional position quantization.

Cooks reference and desktop packages with raw storage, validates both with the
producer, then reads explicit little-endian fields without calling CookScene.
Every other resource/vertex field is compared exactly; world error is measured
for every primitive instance, including ancestor transforms. This is evidence
for these inputs and does not define a new rendered-image tolerance.
"""
import argparse
import hashlib
import json
import math
from pathlib import Path
from asset_processor_paths import PROCESSOR_ROOT, VALIDATION_TOOLS
import struct

import asset_processor_baseline as baseline
from asset_processor_visual_parity import CASES

ROOT = PROCESSOR_ROOT
INVALID = 0xffffffff


class Fields:
    def __init__(self, data):
        self.data, self.offset = data, 0

    def unpack(self, fmt):
        result = struct.unpack_from("<" + fmt, self.data, self.offset)
        self.offset += struct.calcsize("<" + fmt)
        return result[0] if len(result) == 1 else result

    def skip(self, size):
        self.offset += size
        if self.offset > len(self.data):
            raise ValueError("Field exceeds resource")

    def string(self):
        size = self.unpack("Q")
        result = self.data[self.offset:self.offset + size].decode("utf-8")
        self.skip(size)
        return result

    def array(self, stride):
        count = self.unpack("Q")
        self.skip(count * stride)
        return count

    def finish(self):
        if self.offset != len(self.data):
            raise ValueError(f"Unconsumed resource bytes: {len(self.data) - self.offset}")


def package(path):
    data = path.read_bytes()
    if data[:8] != b"ZENASSET" or struct.unpack_from("<II", data, 8) != (3, 0x01020304):
        raise ValueError("Expected a version 3 little-endian package")
    count = struct.unpack_from("<Q", data, 32)[0]
    resources = {}
    for index in range(count):
        entry = struct.unpack_from("<QIIIIQQQQII", data, 64 + index * 64)
        identity, kind, version, flags, codec, offset, stored, decoded, checksum, alignment, encoding = entry
        if codec or encoding or stored != decoded or offset + stored > len(data):
            raise ValueError("This audit requires validated raw resources")
        if kind != 9 and version != 1 and not (kind in (4, 8) and version == 2):
            raise ValueError("Unknown audited resource version")
        resources[kind, (identity & INVALID) - 1] = data[offset:offset + stored]
    return resources


def nodes(resources):
    result = {}
    for (kind, index), data in resources.items():
        if kind == 14:
            fields = Fields(data)
            node = {"name": fields.string(), "parent": fields.unpack("I")}
            fields.array(4)
            node.update(translation=fields.unpack("3f"), rotation=fields.unpack("4f"), scale=fields.unpack("3f"))
            matrix = fields.unpack("16f")
            node["base"] = [[matrix[column * 4 + row] for column in range(4)] for row in range(4)]
            fields.skip(1)
            node["mesh"] = fields.unpack("I")
            fields.array(4)
            node["skin"] = fields.unpack("I")
            fields.skip(8)
            fields.array(4)
            fields.skip(8)
            fields.finish()
            result[index] = node
    return result


def meshes(resources):
    result, normalized = [], {}
    for (kind, mesh), data in resources.items():
        if kind == 2:
            fields = Fields(data)
            mesh_name = fields.string()
            masked = bytearray(data)
            for primitive in range(fields.unpack("Q")):
                name = fields.string()
                first, count, first_index, index_count, material, slot, topology = fields.unpack("7I")
                masked[fields.offset:fields.offset + 24] = bytes(24)
                bounds = fields.unpack("6f")
                fields.skip(1)
                fields.array(4)
                influences = fields.unpack("Q")
                for _ in range(influences):
                    fields.array(8)
                result.append({"mesh": mesh, "mesh_name": mesh_name, "primitive": primitive, "name": name,
                               "first": first, "count": count, "bounds": bounds, "influences": influences})
            fields.array(4)
            fields.finish()
            normalized[kind, mesh] = bytes(masked)
    return result, normalized


def animated_nodes(resources):
    result = set()
    for (kind, index), data in resources.items():
        if kind == 5:
            fields = Fields(data)
            fields.string()
            for _ in range(fields.unpack("Q")):
                fields.skip(8)
                fields.array(4)
                fields.array(4)
            for _ in range(fields.unpack("Q")):
                sampler, node, path, obj, target, prop, dimensions, element, texture = fields.unpack("9I")
                fields.string()
                if path != 4:
                    result.add(node)
                elif obj == 0 and prop in (1, 2, 3, 4):
                    result.add(target)
            fields.skip(37)
            fields.finish()
    return result


def identity():
    return [[float(row == column) for column in range(4)] for row in range(4)]


def multiply(a, b):
    return [[math.fsum(a[row][k] * b[k][column] for k in range(4)) for column in range(4)] for row in range(4)]


def local(node):
    w, x, y, z = node["rotation"]
    rotation = [[1 - 2 * (y*y + z*z), 2 * (x*y - z*w), 2 * (x*z + y*w), 0],
                [2 * (x*y + z*w), 1 - 2 * (x*x + z*z), 2 * (y*z - x*w), 0],
                [2 * (x*z - y*w), 2 * (y*z + x*w), 1 - 2 * (x*x + y*y), 0],
                [0, 0, 0, 1]]
    scale, translation = identity(), identity()
    for component in range(3):
        scale[component][component] = node["scale"][component]
        translation[component][3] = node["translation"][component]
    return multiply(multiply(multiply(translation, rotation), scale), node["base"])


def world(all_nodes, index):
    matrix, ancestors = identity(), []
    while index != INVALID:
        if index in ancestors:
            raise ValueError("Cyclic node ancestry")
        ancestors.append(index)
        matrix = multiply(local(all_nodes[index]), matrix)
        index = all_nodes[index]["parent"]
    return matrix, ancestors


def audit(reference, cooked, absolute, relative):
    a, b = package(reference), package(cooked)
    failures = []
    if a.keys() != b.keys():
        raise ValueError("Resource identities changed")
    changed_resources = [list(key) for key in a if a[key] != b[key]]
    for key in a:
        if key[0] not in (2, 9, 10) and a[key] != b[key]:
            failures.append(f"Non-position resource changed: {key}")
    primitives, mesh_a = meshes(a)
    cooked_primitives, mesh_b = meshes(b)
    if mesh_a != mesh_b:
        failures.append("Mesh fields other than bounds changed")
    all_nodes, animated = nodes(a), animated_nodes(a)
    morph_meshes = {struct.unpack_from("<I", data)[0] for (kind, index), data in a.items() if kind == 7}
    va, vb = a[10, 0], b[10, 0]
    vertex_count = struct.unpack_from("<Q", va)[0]
    if len(va) != 8 + vertex_count * 112 or len(vb) != len(va) or vb[:8] != va[:8]:
        raise ValueError("Unexpected canonical vertex layout")
    positions_a, positions_b, changed = [], [], set()
    for index in range(vertex_count):
        offset = 8 + index * 112
        if va[offset+12:offset+112] != vb[offset+12:offset+112]:
            failures.append(f"Non-position vertex fields changed: {index}")
        if va[offset:offset+12] != vb[offset:offset+12]:
            changed.add(index)
        positions_a.append(struct.unpack_from("<4f", va, offset))
        positions_b.append(struct.unpack_from("<4f", vb, offset))
    groups = {}
    for primitive in primitives:
        group = groups.setdefault((primitive["first"], primitive["count"]), {"exclusions": set(), "owners": []})
        group["owners"].append(primitive)
        if primitive["influences"]:
            group["exclusions"].add("full_skin_influences")
        if primitive["mesh"] in morph_meshes:
            group["exclusions"].add("morph_payload")
        for node_index, node in all_nodes.items():
            if node["mesh"] == primitive["mesh"]:
                if node["skin"] != INVALID:
                    group["exclusions"].add("skin_instance")
                if animated.intersection(world(all_nodes, node_index)[1]):
                    group["exclusions"].add("animated_instance_or_ancestor")
    ranges = list(groups)
    for first, count in ranges:
        for other, length in ranges:
            if (first, count) != (other, length) and first < other + length and other < first + count:
                groups[first, count]["exclusions"].add("partial_overlap")
    measurements = []
    excluded_vertices, eligible_vertices = set(), set()
    for (first, count), group in groups.items():
        group_changed = sum(index in changed for index in range(first, first + count))
        if group["exclusions"]:
            excluded_vertices.update(range(first, first + count))
            if group_changed:
                failures.append(f"Excluded range changed: {first}+{count}")
        else:
            eligible_vertices.update(range(first, first + count))
        for primitive in group["owners"]:
            owners = [index for index, node in all_nodes.items() if node["mesh"] == primitive["mesh"]] or [None]
            for owner in owners:
                matrix = world(all_nodes, owner)[0] if owner is not None else identity()
                lower, upper, maximum = [math.inf]*3, [-math.inf]*3, 0.0
                for index in range(first, first + count):
                    original, quantized = positions_a[index], positions_b[index]
                    location = [math.fsum(matrix[row][c] * original[c] for c in range(4)) for row in range(3)]
                    delta = [math.fsum(matrix[row][c] * (quantized[c] - original[c]) for c in range(3)) for row in range(3)]
                    maximum = max(maximum, math.sqrt(math.fsum(value*value for value in delta)))
                    for component in range(3):
                        lower[component] = min(lower[component], location[component])
                        upper[component] = max(upper[component], location[component])
                diagonal = math.sqrt(math.fsum((high-low)**2 for low, high in zip(lower, upper)))
                bound = max(absolute, relative * diagonal)
                passed = math.isfinite(maximum) and maximum <= bound
                if not passed:
                    failures.append(f"World error exceeds bound: mesh {primitive['mesh']} node {owner}")
                measurements.append({"mesh": primitive["mesh"], "primitive": primitive["primitive"], "node": owner,
                                     "mesh_name": primitive["mesh_name"], "vertex_count": count,
                                     "changed_vertices": group_changed, "exclusions": sorted(group["exclusions"]),
                                     "world_aabb_diagonal": diagonal, "maximum_world_error": maximum,
                                     "allowed_world_error": bound, "fraction_of_allowed_error": maximum / bound if bound else 0,
                                     "passed": passed})
    return {"passed": not failures, "failures": failures, "vertices": vertex_count, "changed_vertices": len(changed),
            "eligible_vertices": len(eligible_vertices), "excluded_vertices": len(excluded_vertices),
            "changed_excluded_vertices": len(changed & excluded_vertices), "changed_resources": changed_resources,
            "non_position_records_exact": not failures, "instances": measurements,
            "maximum_world_error": max((row["maximum_world_error"] for row in measurements), default=0),
            "maximum_fraction_of_allowed_error": max((row["fraction_of_allowed_error"] for row in measurements), default=0)}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--processor", type=Path, required=True)
    parser.add_argument("--reference-dir", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--position-bits", type=int, default=16)
    args = parser.parse_args()
    args.output = args.output.resolve()
    args.output.mkdir(parents=True, exist_ok=False)
    policy_path = VALIDATION_TOOLS / "asset_processor_quality.json"
    policy = json.loads(policy_path.read_text())["desktop_profile"]["quantization"]
    absolute = policy["position_absolute_error_floor_meters"]
    relative = policy["position_max_error_fraction_of_mesh_aabb_diagonal"]
    float_absolute = struct.unpack("<f", struct.pack("<f", absolute))[0]
    float_relative = struct.unpack("<f", struct.pack("<f", relative))[0]
    report = {"processor": baseline.identity(args.processor.resolve()), "policy": baseline.identity(policy_path),
              "audit_source": baseline.identity(Path(__file__).resolve()), "position_bits": args.position_bits,
              "vertex_fetch": "off", "transport": "raw", "mesh_encoding": "raw",
              "absolute_error": float_absolute, "relative_error": float_relative, "results": []}
    sources = []
    for case in CASES:
        settings = (args.reference_dir / f"{case}-source.cfg").read_text(encoding="utf-8-sig")
        source = Path([line.split("=", 1)[1].strip() for line in settings.splitlines()
                       if line.split("=", 1)[0].strip() == "default_model_path"][-1])
        sources.append(source)
        row = {"case": case, "source": baseline.identity(source), "commands": []}
        paths = {}
        for profile in ("reference", "desktop"):
            paths[profile] = args.output / f"{case}-{profile}.zenasset"
            command = [args.processor, "cook", source, "--output", paths[profile], "--workers", "2", "--profile", profile]
            if profile == "desktop":
                command += ["--position-bits", str(args.position_bits), "--position-max-error", str(absolute),
                            "--position-relative-error", str(relative), "--vertex-fetch", "off"]
            row["commands"].append(baseline.run(command, args.output / f"{case}-{profile}-cook.log", ROOT))
            row["commands"].append(baseline.run([args.processor, "validate", paths[profile]],
                                               args.output / f"{case}-{profile}-validate.log", ROOT))
        row["packages"] = {key: baseline.identity(value) for key, value in paths.items()}
        row["audit"] = audit(paths["reference"], paths["desktop"], float_absolute, float_relative)
        row["passed"] = all(command["exit_code"] == 0 for command in row["commands"]) and row["audit"]["passed"]
        report["results"].append(row)
        baseline.write(args.output / "results.json", report)
        print(f"{case}: changed={row['audit']['changed_vertices']} excluded={row['audit']['excluded_vertices']} "
              f"max_error={row['audit']['maximum_world_error']:.9g} passed={row['passed']}", flush=True)
    baseline.capture_dependency_manifest([path for path in sources if path.suffix.lower() in (".gltf", ".glb")],
                                         args.output / "gltf-dependencies.json")
    report["processor_unchanged"] = baseline.identity(args.processor.resolve()) == report["processor"]
    report["actually_changed_vertices"] = sum(row["audit"]["changed_vertices"] for row in report["results"])
    report["passed"] = all(row["passed"] for row in report["results"]) and report["processor_unchanged"]
    baseline.write(args.output / "results.json", report)
    return 0 if report["passed"] and report["actually_changed_vertices"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
