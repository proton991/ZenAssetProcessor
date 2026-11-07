from pathlib import Path, PurePosixPath
import json, zipfile, collections, struct, urllib.parse, math, os, hashlib
import argparse

parser = argparse.ArgumentParser(
    description="Inventory Sponza glTF dependencies, geometry and ZIP members without decoding geometry."
)
parser.add_argument("--root", type=Path, required=True)
parser.add_argument("--output", type=Path, required=True)
args = parser.parse_args()
root = args.root.resolve()
out = args.output.resolve()
out.parent.mkdir(parents=True, exist_ok=True)


def image_size(p):
    with open(p, "rb") as f:
        sig = f.read(24)
        if sig[:8] == b"\x89PNG\r\n\x1a\n":
            return [*struct.unpack(">II", sig[16:24]), "PNG"]
        if sig[:2] == b"\xff\xd8":
            f.seek(2)
            while True:
                b = f.read(1)
                if not b:
                    break
                if b != b"\xff":
                    continue
                while b == b"\xff":
                    b = f.read(1)
                m = b[0]
                if m in (0xD8, 0xD9):
                    continue
                n = struct.unpack(">H", f.read(2))[0]
                if m in (
                    0xC0,
                    0xC1,
                    0xC2,
                    0xC3,
                    0xC5,
                    0xC6,
                    0xC7,
                    0xC9,
                    0xCA,
                    0xCB,
                    0xCD,
                    0xCE,
                    0xCF,
                ):
                    x = f.read(5)
                    h, w = struct.unpack(">HH", x[1:])
                    return [w, h, "JPEG"]
                f.seek(n - 2, 1)
    return None


def mm(a, b):
    return [
        [sum(a[r][k] * b[k][c] for k in range(4)) for c in range(4)] for r in range(4)
    ]


def mat(n):
    if "matrix" in n:
        a = n["matrix"]
        return [[a[c * 4 + r] for c in range(4)] for r in range(4)]
    x, y, z, w = n.get("rotation", [0, 0, 0, 1])
    s = n.get("scale", [1, 1, 1])
    t = n.get("translation", [0, 0, 0])
    a = [
        [1 - 2 * (y * y + z * z), 2 * (x * y - z * w), 2 * (x * z + y * w), t[0]],
        [2 * (x * y + z * w), 1 - 2 * (x * x + z * z), 2 * (y * z - x * w), t[1]],
        [2 * (x * z - y * w), 2 * (y * z + x * w), 1 - 2 * (x * x + y * y), t[2]],
        [0, 0, 0, 1],
    ]
    for r in range(3):
        for c in range(3):
            a[r][c] *= s[c]
    return a


def inspect(name, d, json_bytes, resolve):
    access = d.get("accessors", [])
    meshes = d.get("meshes", [])
    nodes = d.get("nodes", [])

    def count(p):
        v = access[p["attributes"]["POSITION"]]["count"]
        i = access[p["indices"]]["count"] if "indices" in p else v
        mode = p.get("mode", 4)
        return dict(
            vertices=v,
            indices=i,
            triangles=i // 3 if mode == 4 else max(i - 2, 0) if mode in (5, 6) else 0,
            mode=mode,
        )

    counts = [[count(p) for p in m["primitives"]] for m in meshes]
    roots = d.get("scenes", [{"nodes": []}])[d.get("scene", 0)].get("nodes", [])
    reachable = []
    bounds = [[float("inf")] * 3, [float("-inf")] * 3]

    def visit(i, parent):
        reachable.append(i)
        n = nodes[i]
        world = mm(parent, mat(n))
        if "mesh" in n:
            for p in meshes[n["mesh"]]["primitives"]:
                a = access[p["attributes"]["POSITION"]]
                if "min" in a and "max" in a:
                    for mask in range(8):
                        v = [
                            a["max" if mask & (1 << c) else "min"][c] for c in range(3)
                        ] + [1]
                        pt = [
                            sum(world[r][c] * v[c] for c in range(4)) for r in range(3)
                        ]
                        for r in range(3):
                            bounds[0][r] = min(bounds[0][r], pt[r])
                            bounds[1][r] = max(bounds[1][r], pt[r])
        for c in n.get("children", []):
            visit(c, world)

    visit_identity = [[int(r == c) for c in range(4)] for r in range(4)]
    for i in roots:
        visit(i, visit_identity)
    meshrefs = collections.Counter(
        nodes[i]["mesh"] for i in reachable if "mesh" in nodes[i]
    )
    buffers = []
    images = []
    for b in d.get("buffers", []):
        uri = b.get("uri")
        ref = resolve(uri) if uri else {"embedded": True}
        buffers.append(dict(uri=uri, declared_bytes=b["byteLength"], **ref))
    for im in d.get("images", []):
        uri = im.get("uri")
        ref = resolve(uri) if uri else {"embedded": True}
        images.append(dict(uri=uri, **ref))
    # Vertex totals are sums of primitive POSITION accessor counts (shared accessors could overcount); unique below.
    position_accessors = {
        p["attributes"]["POSITION"] for m in meshes for p in m["primitives"]
    }
    primitive_counts = [c for m in counts for c in m]
    return dict(
        name=name,
        json_bytes=json_bytes,
        nodes=len(nodes),
        reachable_nodes=len(reachable),
        roots=len(roots),
        meshes=len(meshes),
        mesh_nodes=sum(meshrefs.values()),
        shared_meshes={str(k): v for k, v in meshrefs.items() if v > 1},
        primitives=len(primitive_counts),
        materials=len(d.get("materials", [])),
        images_count=len(images),
        textures=len(d.get("textures", [])),
        lights_definitions=len(
            d.get("extensions", {}).get("KHR_lights_punctual", {}).get("lights", [])
        ),
        light_nodes=sum(
            "KHR_lights_punctual" in nodes[i].get("extensions", {}) for i in reachable
        ),
        skins=len(d.get("skins", [])),
        animations=len(d.get("animations", [])),
        extensions=d.get("extensionsUsed", []),
        primitive_modes=dict(collections.Counter(c["mode"] for c in primitive_counts)),
        vertices=sum(c["vertices"] for c in primitive_counts),
        unique_position_vertices=sum(access[i]["count"] for i in position_accessors),
        indices=sum(c["indices"] for c in primitive_counts),
        triangles=sum(c["triangles"] for c in primitive_counts),
        instance_triangles=sum(
            meshrefs[i] * sum(c["triangles"] for c in counts[i]) for i in meshrefs
        ),
        world_bounds_from_accessor_bounds=bounds,
        buffers=buffers,
        images=images,
        primitive_details=[
            dict(
                mesh_index=mi,
                mesh_name=mesh.get("name"),
                primitive_index=pi,
                material=primitive.get("material"),
                attributes=sorted(primitive["attributes"]),
                **counts[mi][pi],
                canonical_geometry_112_byte_vertices_4_byte_indices=112
                * counts[mi][pi]["vertices"]
                + 4 * counts[mi][pi]["indices"]
            )
            for mi, mesh in enumerate(meshes)
            for pi, primitive in enumerate(mesh["primitives"])
        ],
        mesh_names=[m.get("name") for m in meshes],
        buffer_view_bytes=sum(b["byteLength"] for b in d.get("bufferViews", [])),
        root_transforms=[
            dict(
                index=i,
                name=nodes[i].get("name"),
                **{
                    k: nodes[i][k]
                    for k in ("translation", "rotation", "scale", "matrix")
                    if k in nodes[i]
                }
            )
            for i in roots[:5]
        ],
        normalized_json_sha256=hashlib.sha256(
            json.dumps(d, sort_keys=True).encode()
        ).hexdigest(),
    )


reports = []
for p in sorted(root.rglob("*.gltf")):

    def resolve(uri):
        target = (p.parent / urllib.parse.unquote(uri)).resolve()
        return dict(
            path=str(target),
            exists=target.is_file(),
            bytes=target.stat().st_size if target.is_file() else None,
            dimensions=(
                image_size(target)
                if target.is_file()
                and target.suffix.lower() in (".png", ".jpg", ".jpeg")
                else None
            ),
        )

    reports.append(
        inspect(
            str(p.relative_to(root)),
            json.loads(p.read_text(encoding="utf-8-sig")),
            p.stat().st_size,
            resolve,
        )
    )
zipreports = []
for p in sorted(root.glob("*.zip")):
    with zipfile.ZipFile(p) as z:
        infos = z.infolist()
        ext = collections.defaultdict(lambda: [0, 0])
        for i in infos:
            ext[Path(i.filename).suffix.lower()][0] += 1
            ext[Path(i.filename).suffix.lower()][1] += i.file_size
        zipreports.append(
            dict(
                name=p.name,
                bytes=p.stat().st_size,
                uncompressed_bytes=sum(i.file_size for i in infos),
                extensions=dict(ext),
                entries=[
                    dict(
                        path=i.filename,
                        bytes=i.file_size,
                        compressed_bytes=i.compress_size,
                        crc32=i.CRC,
                    )
                    for i in infos
                ],
            )
        )
        if "10k_candles" in p.name and not any(
            Path(model["name"]).name == "NewSponza_4_Combined_glTF.gltf"
            for model in reports
        ):
            n = next(n for n in z.namelist() if n.endswith(".gltf"))
            raw = z.read(n)

            def resolve(uri):
                target = str(PurePosixPath(n).parent / urllib.parse.unquote(uri))
                i = z.getinfo(target)
                return dict(
                    path=str(p) + "!" + target,
                    exists=True,
                    bytes=i.file_size,
                    compressed_bytes=i.compress_size,
                    crc32=i.CRC,
                )

            reports.append(
                inspect(str(p.name) + "!" + n, json.loads(raw), len(raw), resolve)
            )
unique_images = {im["path"]: im for r in reports for im in r["images"]}
for r in reports:
    imgs = {i["path"]: i for i in r["images"]}.values()
    r["image_source_bytes"] = sum(i["bytes"] or 0 for i in imgs)
    r["image_dimension_histogram"] = dict(
        collections.Counter(str(i.get("dimensions")) for i in imgs)
    )
    r["image_rgba8_base_bytes"] = sum(
        i["dimensions"][0] * i["dimensions"][1] * 4 for i in imgs if i.get("dimensions")
    )
    r["dependency_bytes"] = (
        r["json_bytes"]
        + sum(b["bytes"] or 0 for b in r["buffers"])
        + r["image_source_bytes"]
    )
    r["declared_buffer_bytes"] = sum(b["declared_bytes"] for b in r["buffers"])
allfiles = [p for p in root.rglob("*") if p.is_file()]
file_ext = collections.defaultdict(lambda: [0, 0])
for p in allfiles:
    file_ext[p.suffix.lower()][0] += 1
    file_ext[p.suffix.lower()][1] += p.stat().st_size
report = dict(
    root=str(root),
    files_count=len(allfiles),
    directory_bytes=sum(p.stat().st_size for p in allfiles),
    file_extensions=dict(file_ext),
    models=reports,
    archives=zipreports,
    unique_image_paths=len(unique_images),
    unique_image_source_bytes=sum(i["bytes"] or 0 for i in unique_images.values()),
    unique_image_rgba8_base_bytes=sum(
        i["dimensions"][0] * i["dimensions"][1] * 4
        for i in unique_images.values()
        if i.get("dimensions")
    ),
)
groups = collections.defaultdict(list)
png_types = collections.Counter()
for path, im in unique_images.items():
    with open(path, "rb") as image_file:
        im["sha256"] = hashlib.file_digest(image_file, "sha256").hexdigest()
    groups[im["sha256"]].append(path)
    with open(path, "rb") as image_file:
        header = image_file.read(29)
    im["png_bit_depth"] = header[24]
    im["png_color_type"] = header[25]
    png_types[str((header[24], header[25]))] += 1
for model in reports:
    model["canonical_geometry_112_byte_vertices_4_byte_indices"] = (
        112 * model["vertices"] + 4 * model["indices"]
    )
    for im in model["images"]:
        im.update(unique_images[im["path"]])
report["model_totals"] = {
    key: sum(model[key] for model in reports)
    for key in (
        "nodes",
        "meshes",
        "mesh_nodes",
        "primitives",
        "vertices",
        "indices",
        "triangles",
        "materials",
        "lights_definitions",
        "light_nodes",
        "dependency_bytes",
        "declared_buffer_bytes",
        "canonical_geometry_112_byte_vertices_4_byte_indices",
    )
}
report["image_duplicate_content_groups"] = [
    paths for paths in groups.values() if len(paths) > 1
]
report["unique_image_content_count"] = len(groups)
report["unique_image_content_source_bytes"] = sum(
    unique_images[paths[0]]["bytes"] for paths in groups.values()
)
report["png_types"] = dict(png_types)
report["measurement_scope"] = (
    "Read-only glTF JSON/accessor metadata, PNG headers, ZIP central directories, SHA256 over referenced extracted PNG files. No binary geometry decode, import, texture decode, cook, GPU upload, or rendering. Candle BIN not extracted. Bounds are conservative transformed accessor bounds."
)
out.write_text(json.dumps(report, indent=2), encoding="utf-8")
print("REPORT", out)
for r in reports:
    print(
        json.dumps(
            {
                k: v
                for k, v in r.items()
                if k
                not in (
                    "images",
                    "buffers",
                    "mesh_names",
                    "root_transforms",
                    "json_sha256",
                    "primitive_details",
                )
            },
            indent=None,
        )
    )
print(
    json.dumps(
        {k: v for k, v in report.items() if k not in ("models", "archives")}, indent=2
    )
)
