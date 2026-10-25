"""Strict batch CLI, rooted outputs, content reuse and source-free result."""
import json
from pathlib import Path
import subprocess
import sys
import tempfile


def invoke(executable, *args, expected=0):
    result = subprocess.run([str(executable), *map(str, args)], capture_output=True, text=True, encoding="utf-8")
    assert result.returncode == expected, (args, result.returncode, result.stdout, result.stderr)
    return result


def main():
    executable = Path(sys.argv[1]).resolve()
    with tempfile.TemporaryDirectory(prefix="zenasset-batch-") as temporary:
        directory = Path(temporary)
        source = directory / "triangle.obj"
        source.write_text("v 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 3\n", encoding="utf-8")
        manifest = directory / "build.json"
        document = {"version": 1, "outputDirectory": "default", "jobs": [
            {"source": source.name, "output": "nested/a.zenasset"},
            {"source": source.name, "output": "b.zenasset", "compression": {"transport": "zstd", "meshoptimizer": True}},
        ]}
        manifest.write_text(json.dumps(document), encoding="utf-8")
        arguments = ("build", manifest, "--output-dir", directory / "override", "--jobs", 2,
                     "--memory-budget", 256 * 1024**2, "--job-memory", 128 * 1024**2)
        first = json.loads(invoke(executable, *arguments).stdout)
        assert [job["disposition"] for job in first["jobs"]] == ["built", "built"]
        assert all(job["published"] for job in first["jobs"])
        assert not (directory / "default").exists()
        second = json.loads(invoke(executable, *arguments).stdout)
        assert [job["disposition"] for job in second["jobs"]] == ["reused", "reused"]
        forced = json.loads(invoke(executable, *arguments, "--force", "on").stdout)
        assert all(job["published"] for job in forced["jobs"])
        material = directory / "material.mtl"
        material.write_text("newmtl matte\nKd 0.8 0.3 0.2\n", encoding="utf-8")
        source.write_text("mtllib material.mtl\nv 0 0 0\nv 1 0 0\nv 0 1 0\nusemtl matte\nf 1 2 3\n", encoding="utf-8")
        merged = subprocess.run([str(executable), *map(str, arguments)], stdout=subprocess.PIPE,
                                stderr=subprocess.STDOUT, text=True, encoding="utf-8")
        assert merged.returncode == 0, merged.stdout
        records = [json.loads(line) for line in merged.stdout.splitlines()]
        assert records[0]["status"] == 0 and len(records[0]["jobs"]) == 2
        assert any("severity" in record for record in records[1:])
        for path in (directory / "override").rglob("*.zenasset"):
            invoke(executable, "validate", path)
        invoke(executable, *arguments, "--unknown", "x", expected=2)
        invoke(executable, *arguments, "--force", "maybe", expected=2)
        invoke(executable, *arguments, "--jobs", 1, expected=2)
        invoke(executable, "build", manifest, "--memory-budget", 1, expected=3)
        for malformed in ([], None, {**document, "jobs": 1}, {**document, "extra": True},
                          {**document, "jobs": [{"source": source.name, "output": "../escape.zenasset"}]}):
            manifest.write_text(json.dumps(malformed), encoding="utf-8")
            invoke(executable, "build", manifest, expected=2)
        source.unlink()
        invoke(executable, "validate", directory / "override/nested/a.zenasset")
    print("Batch CLI cases passed")


if __name__ == "__main__":
    main()
