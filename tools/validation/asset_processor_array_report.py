"""Verify saved EditorAssetRenderingTest captures and report native allocations.

This reports the original 16/128-pixel fixtures, not general corpus acceptance.
Native VMA resource sizes exclude unused pool blocks and descriptor/view memory.
"""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import zlib


def read_ppm(path):
    magic, extent, maximum, data = path.read_bytes().split(b"\n", 3)
    width, height = map(int, extent.split())
    assert magic == b"P6" and maximum == b"255"
    assert len(data) == width * height * 3
    return width, height, data


def png_chunk(name, data):
    return struct.pack(">I", len(data)) + name + data + struct.pack(">I", zlib.crc32(name + data))


def write_png(path, width, height, data):
    stride = width * 3
    rows = b"".join(b"\0" + data[y * stride:(y + 1) * stride] for y in range(height))
    path.write_bytes(b"\x89PNG\r\n\x1a\n"
                     + png_chunk(b"IHDR", struct.pack(">2I5B", width, height, 8, 2, 0, 0, 0))
                     + png_chunk(b"IDAT", zlib.compress(rows, 9)) + png_chunk(b"IEND", b""))


def report(directory):
    pairs, resources = [], []
    for path in sorted(directory.glob("*-array-resources.json")):
        name = path.name.removesuffix("-resources.json")
        reference_name = name.removesuffix("-array") + "-standalone"
        packed = json.loads(path.read_text())
        standalone = json.loads((directory / (reference_name + "-resources.json")).read_text())
        for value, count in ((packed, 4), (standalone, 7)):
            assert len(value["resources"]) == value["authored_images"] == count
            assert sum(r["allocation_bytes"] for r in value["resources"]) == value["texture_allocation_bytes"]
            assert value["texture_bindings"] == 12 and value["sampler_bindings"] == 2
        captures = sorted(directory.glob(name + "-v*.ppm"))
        assert len(captures) == (12 if name.startswith("mips-") else 4), name
        for capture in captures:
            reference = directory / capture.name.replace("-array-", "-standalone-")
            width, height, data = read_ppm(capture)
            ref_width, ref_height, expected = read_ppm(reference)
            assert (width, height) == (ref_width, ref_height) == (256, 192)
            differences = [abs(a - b) for a, b in zip(data, expected)]
            maximum = max(differences)
            assert maximum == 0, (capture, maximum)
            for path, pixels in ((capture, data), (reference, expected)):
                write_png(path.with_suffix(".png"), width, height, pixels)
            colored = sum(max(data[i:i+3]) - min(data[i:i+3]) > 30 for i in range(0, len(data), 3))
            assert colored > 0, capture
            pairs.append(dict(array=capture.name, reference=reference.name, width=width, height=height,
                              maximum_rgb_error=maximum, colored_pixels=colored,
                              array_sha256=hashlib.sha256(capture.read_bytes()).hexdigest(),
                              reference_sha256=hashlib.sha256(reference.read_bytes()).hexdigest()))
        resources.append(dict(name=name, standalone=standalone, packed=packed,
                              allocation_delta_bytes=packed["texture_allocation_bytes"] - standalone["texture_allocation_bytes"],
                              standalone_package_bytes=(directory / (reference_name + ".zenasset")).stat().st_size,
                              array_package_bytes=(directory / (name + ".zenasset")).stat().st_size))
    assert resources, "No completed fixture captures and resource measurements found"
    result = dict(scope="Saved mixed-material fixture comparisons; no general quality or throughput claim",
                  pair_count=len(pairs), rgb_channels_compared=sum(p["width"] * p["height"] * 3 for p in pairs),
                  pairs=pairs, resources=resources)
    (directory / "comparison.json").write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    print(f"Verified {len(pairs)} exact RGB frame pairs and {len(resources)} allocation comparisons.")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("directory", type=Path)
    report(parser.parse_args().directory.resolve())
