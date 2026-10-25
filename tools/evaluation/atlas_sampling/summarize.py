"""Summarize completed probe logs without treating an incomplete run as evidence."""
import argparse
import hashlib
import json
from pathlib import Path


def group_metrics(rows):
    return {
        "comparisons": len(rows),
        "paired_samples": sum(row["samples"] for row in rows),
        "failed": sum(not row["pass"] for row in rows),
        "channel_gate_failed": sum(row["max_byte_error"] > 1 for row in rows),
        "alpha_changed_queries": sum(row["alpha_changes"] != 0 for row in rows),
        "max_error": max(row["max_error"] for row in rows),
        "max_byte_error": max(row["max_byte_error"] for row in rows),
        "channels_above_step": sum(row["channels_above_step"] for row in rows),
        "alpha_changes": sum(row["alpha_changes"] for row in rows),
        "native_blend_error": max(row.get("native_blend_error", 0) for row in rows),
        "region_error": max(row.get("region_error", 0) for row in rows),
        "query_difference": max(row.get("query_difference", 0) for row in rows),
        "wrong_bound_levels": sum(row.get("wrong_bound_levels", 0) for row in rows),
    }


def verify_single_level_matrix(samples, settings):
    revision = settings.get("probe_revision", 0)
    if settings.get("layout", 0) >= 4 and revision >= 2:
        dimensions = [(32, 32), (13, 7), (1, 17), (257, 129), (127, 255), (1, 1)]
        if revision >= 7:
            dimensions += [(2, 2), (4, 8), (8, 4), (16, 64), (128, 256), (256, 128), (512, 1024), (1024, 512)]
        expected = {
            (width, height, pattern, srgb, False, query)
            for width, height in dimensions
            for pattern in range(3)
            for srgb in (False, True)
            for query in range(6 if revision == 2 else 12)
        }
        actual = {
            tuple(row[field] for field in ("width", "height", "pattern", "srgb", "partial", "query"))
            for row in samples
        }
        assert actual == expected and len(samples) == len(expected), "Incomplete or duplicate single-level sample matrix"
        assert all(row["samples"] == 129 * 129 and row["sampling_grid"] == row["query"] // 2 for row in samples)


def summarize(path):
    records = [json.loads(line) for line in path.read_text(encoding="utf-8-sig").splitlines()]
    assert records and records[-1]["kind"] == "summary", f"Incomplete probe: {path}"
    end = records[-1]
    samples = [row for row in records if row["kind"] == "sample"]
    assert samples and end["failed_comparisons"] == sum(not row["pass"] for row in samples)
    settings = next((row for row in records if row["kind"] == "settings"), {})
    device = next(row for row in records if row["kind"] == "device")
    verify_single_level_matrix(samples, settings)
    groups = {}
    for mode in sorted({row["mode"] for row in samples}):
        rows = [row for row in samples if row["mode"] == mode]
        groups[mode] = group_metrics(rows)
    dimension_classes = {}
    for power_two in (False, True):
        rows = [row for row in samples if all(row[key] > 0 and row[key] & (row[key] - 1) == 0
                                               for key in ("width", "height")) == power_two]
        if rows:
            dimension_classes["power_of_two" if power_two else "non_power_of_two"] = group_metrics(rows)
    allocations = {}
    for row in records:
        if row["kind"] == "allocation":
            key = tuple(row[field] for field in ("width", "height", "mips", "srgb"))
            if key in allocations:
                assert allocations[key] == row, f"Inconsistent allocation requirements: {key}"
            allocations[key] = row
    native = settings.get("native_chain", False) or settings.get("layout", 0) in (1, 2, 3)
    unique = {
        (row["width"], row["height"], row["pattern"], row["srgb"], row["partial"],
         row["query"] - 6 if native and row["query"] >= 18 else row["query"])
        for row in samples
    }
    return {
        "path": str(path),
        "sha256": hashlib.sha256(path.read_bytes()).hexdigest(),
        "settings": settings,
        "device": device,
        "summary": end,
        "comparisons": len(samples),
        "unique_queries": len(unique),
        "groups": groups,
        "dimension_classes": dimension_classes,
        "allocations": list(allocations.values()),
        "note": "Layouts 1-3 repeat gradient slots 12-17 in slots 18-23. Single-level layouts use three coordinate grids in revision 2 and six from revision 3 onward, with one explicit and one oblique gradient operation per grid (revision 1 repeated 18 operations). Revision 7 expands the dimension matrix from six to fourteen sizes; revision 8 uses production packer power-of-two pages. Single-level, non-anisotropic operations can produce identical samples; unique_queries counts settings, not independent filtered values. Dimension classes report subsets without changing the complete-matrix gate.",
    }


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("logs", type=Path, nargs="+")
    parser.add_argument("--output", type=Path, required=True)
    options = parser.parse_args()
    report = [summarize(path) for path in options.logs]
    options.output.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    for run in report:
        print(f"{run['path']}: {run['comparisons']} comparisons, "
              f"{run['summary']['failed_comparisons']} failed, "
              f"{run['summary']['validation_errors']} validation errors")


if __name__ == "__main__":
    main()
