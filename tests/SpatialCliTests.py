"""Spatial cooking, option ordering, manifest identity and source-free loading."""
import json
from pathlib import Path
import sys
import tempfile

from ArrayCliTests import fixture
from CliTests import invoke


def main():
    executable = Path(sys.argv[1]).resolve()
    with tempfile.TemporaryDirectory(prefix="zenasset-spatial-cli-") as temporary:
        directory = Path(temporary)
        source = directory / "three-images.gltf"
        fixture(source)
        document = json.loads(source.read_text(encoding="utf-8"))
        document["samplers"][0]["minFilter"] = 9729  # Explicitly no mip generation.
        source.write_text(json.dumps(document), encoding="utf-8")
        packed = directory / "spatial.zenasset"
        plain = directory / "standalone.zenasset"
        common = ("--profile", "desktop", "--compression", "zstd")
        invoke(executable, "cook", source, "-o", plain, *common)
        invoke(executable, "cook", source, "-o", packed, *common, "--atlas-max-dimension", 64,
               "--atlas-max-pages", 8, "--atlas-max-images", 32, "--atlas", "spatial")
        reference = json.loads(invoke(executable, "validate", plain).stdout)
        result = json.loads(invoke(executable, "validate", packed).stdout)
        assert result["images"] == reference["images"] - 2, (reference, result)
        for field in ("nodes", "meshes", "vertices", "indices", "materials", "animations", "skins"):
            assert result[field] == reference[field], field
        resources = json.loads(invoke(executable, "inspect", packed).stdout)["resources"]
        assert sum(r["type"] == 4 and r["version"] == 4 for r in resources) >= 3
        assert any(r["type"] == 9 and r["version"] == 8 for r in resources)
        before = packed.read_bytes()
        invoke(executable, "cook", source, "-o", packed, *common, "--atlas", "spatial",
               "--atlas-max-dimension", 64, "--atlas-max-pages", 8, "--atlas-max-images", 32)
        assert packed.read_bytes() == before, "Option ordering must not change settings, metadata or page bytes"
        for option, value in (("--atlas-max-dimension", 0), ("--atlas-max-dimension", 8),
                              ("--atlas-max-dimension", 63), ("--atlas-max-dimension", 16385),
                              ("--atlas-max-pages", 0), ("--atlas-max-images", 0)):
            invoke(executable, "cook", source, "-o", packed, *common, "--atlas", "spatial",
                   option, value, expected=2)
            assert packed.read_bytes() == before
        invoke(executable, "cook", source, "-o", packed, "--atlas", "spatial", expected=2)
        assert packed.read_bytes() == before

        manifest = directory / "build.json"
        settings = {"spatialPages": True, "spatialMaxDimension": 64,
                    "spatialMaxPages": 8, "spatialMaxImages": 32}
        build = {"version": 1, "outputDirectory": "batch", "jobs": [
            {"source": source.name, "output": "spatial.zenasset", "profile": "desktop",
             "processing": {"textures": settings}}
        ]}
        manifest.write_text(json.dumps(build), encoding="utf-8")
        assert json.loads(invoke(executable, "build", manifest).stdout)["jobs"][0]["disposition"] == "built"
        assert json.loads(invoke(executable, "build", manifest).stdout)["jobs"][0]["disposition"] == "reused"
        for key, value in (("spatialMaxDimension", 32), ("spatialMaxPages", 9),
                           ("spatialMaxImages", 33), ("spatialPages", False)):
            settings[key] = value
            manifest.write_text(json.dumps(build), encoding="utf-8")
            assert json.loads(invoke(executable, "build", manifest).stdout)["jobs"][0]["disposition"] == "built", key
            assert json.loads(invoke(executable, "build", manifest).stdout)["jobs"][0]["disposition"] == "reused", key
        singleton = json.loads(invoke(executable, "validate", directory / "batch/spatial.zenasset").stdout)
        assert singleton["images"] == reference["images"]
        settings["spatialPages"] = True
        settings["arrayPages"] = True
        manifest.write_text(json.dumps(build), encoding="utf-8")
        batch_before = (directory / "batch/spatial.zenasset").read_bytes()
        invoke(executable, "build", manifest, expected=2)
        assert (directory / "batch/spatial.zenasset").read_bytes() == batch_before
        source.rename(directory / "source-unavailable.gltf")
        invoke(executable, "validate", packed)
        invoke(executable, "validate", directory / "batch/spatial.zenasset")
    print("Spatial CLI, option ordering, manifest invalidation and source-free reads passed")


if __name__ == "__main__":
    main()
