# Asset validation tools

These scripts belong to ZenAssetProcessor. They cover package structure and
round trips, compression and quantization, texture quality, normal mips, atlas
sampling, geometry poses, upload measurements and rendered comparisons. They were
moved from ZenEngine's `tools` directory during the repository ownership cleanup.

Run a command's `--help` for its inputs. Existing script names are preserved so
imports between the tools remain local. Pure package audits use this repository
as their working directory. Rendering comparisons use the sibling `ZenEngine`
checkout by default; set `ZENENGINE_SOURCE_DIR` to override that location. Generated
reports default to this repository's `build` directory.

```sh
python tools/validation/asset_processor_texture_audit.py --help
python tools/validation/asset_processor_codec_benchmark.py --help
python tools/validation/verify_gltf_corpus.py --help
```

Some quality/image commands require NumPy and Pillow. Native process-memory
measurements use Windows APIs. Rendering commands use the engine's configuration
and capture lock and restore the original configuration after each batch.

The [optional ZenEngine consumer suite](../../tests/integration/zenengine/README.md)
builds `ZenAssetEngineTests`, `ZenAssetTextureIntegrationTest`,
`EditorAssetRenderingTest`, `GLTFCorpusImport`, `SceneLoadBenchmark`,
`ScenePackageSmoke`, `ScenePackageBenchmark` and `SceneUploadBenchmark`.
`asset_processor_baseline.py --build-root <directory>` expects `zenengine-debug`
and `zenengine-release` beneath that directory; its default is this repository's
`build` directory. Existing frozen baseline records are unchanged.

Standalone processor builds with `ZENASSET_BUILD_TESTING=ON` also build
`ZenAssetRoundtripAudit` and `ZenAssetHdrFixtures` from the two C++ probe sources
here. Neither helper requires ZenEngine.

`asset_processor_provoking_vertex.py` runs the standalone Vulkan flat-shading
acceptance probe against an installed Processor/Reader SDK. It does not use or
build ZenEngine. See [commands and verified scope](../../docs/ProvokingVertexVerification.md).

`asset_processor_atlas_measurements.py` measures separate CookScene and WriteScene
stages for matched array/spatial off/on workloads against an installed SDK.
It keeps full-scene correctness checks outside timed processes and requires actual
page creation. See [method, results and limits](../../docs/AtlasCookingMeasurements.md).

`asset_processor_codec_measurements.py` compares raw, Zstandard, meshoptimizer
and combined packages with source-absent equality checks and repeated fresh-process
reads. See [measurement scope and results](../../docs/CodecMeasurementVerification.md).

`asset_processor_atlas_workloads.py` audits authored natural-asset eligibility and
measures only branches that actually produce pages. See
[natural workload evidence](../../docs/AtlasWorkloadVerification.md) for matching
mip-preparation policy, exclusions and costs.

`asset_processor_stage_measurements.py` compares ordinary and privately instrumented
actual processor pipelines, with separate parser/image processing/encoding/file
publication and package-read/decode intervals. Full scene and decoded-reader
parity checks precede repeated timings. See
[stage boundaries, measurements and limits](../../docs/StageMeasurementVerification.md).
