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

## Multi-model Sponza workflow

These tools exercise the standalone processor against `E:/Dev/main_sponza`,
including its `pkg_*` add-ons. They do not launch ZenEngine or measure rendering.
See [multi-model verification](../../docs/MultiModelVerification.md) for results,
[assembly builds](../../docs/AssemblyBuilds.md) for the build contract, and
[native assemblies](../../docs/SceneAssemblies.md) for the reader API.

| Script | Purpose and inputs |
| --- | --- |
| `sponza_inventory.py` | `--root`, `--output`: inspect extracted glTF/accessor metadata, image headers/content hashes and archive members without decoding geometry or images. Bounds come from transformed accessor bounds. |
| `multi_model_baseline.py` | `--root`, `--compiler`, `--output`: serial single-model cooks; `--models` selects `main curtains ivy trees candles`. Selecting candles extracts only its glTF/BIN pair and records their identities. `--runs`, `--timeout` and `--memory-gib` bound the campaign; optional `--quality 0..4` enables explicit desktop BC/mip processing. |
| `multi_model_texture_preflight.py` | `--root`, `--compiler`, `--output`, optional `--timeout`: generate fixtures referencing authored 4K images; measure color quality levels 0..4, one normal case and one scalar case. The scalar fixture treats the packed image's red channel as occlusion; this is an isolated encoder check. |
| `sponza_assembly_manifest.py` | `--root`, `--candles`, `--manifest`, `--output-dir`: write an explicit five-model version 2 manifest. Default desktop processing uses mips, BC7/BC5/BC4 and `--quality 4`; `--reference` selects the lossless image baseline. Both use Zstandard level 3 and meshoptimizer. Candle light submission is disabled while its authored lights remain serialized. |
| `assembly_measurements.py` | `--compiler`, `--manifest`, `--output`: run build, reuse, sequential native validation and assembly-only samples. `--fresh-runs`, `--reuse-runs` and `--timeout` control repetitions and per-process time limits. |

Run the following PowerShell commands from the processor repository. Set
`$Compiler` to the Release executable being evaluated. Each measurement output
directory must be new and below `build/`. The assembly's cooked output must also
be below `build/`. Every fresh sample uses `--force on`, including `fresh-0`.
Start with an absent cooked directory for an initial-publication measurement;
an existing directory measures a rebuild with its recorded initial disk occupancy.

```powershell
$Root = 'E:/Dev/main_sponza'
$Compiler = 'E:/Dev/ZenAssetProcessor/build/processor/Release/zenassetc.exe'
$Campaign = 'build/sponza-reproduction'
python tools/validation/sponza_inventory.py --root $Root --output "$Campaign/inventory.json"
python tools/validation/multi_model_baseline.py --root $Root --compiler $Compiler --output "$Campaign/baseline" --models main curtains ivy trees candles --runs 3 --timeout 1800 --memory-gib 16 --quality 0
python tools/validation/multi_model_texture_preflight.py --root $Root --compiler $Compiler --output "$Campaign/preflight" --timeout 180
$Candles = (Get-Content "$Campaign/baseline/extracted-inputs.json" -Raw | ConvertFrom-Json | Where-Object { $_.path.EndsWith('.gltf') }).path
python tools/validation/sponza_assembly_manifest.py --root $Root --candles $Candles --manifest "$Campaign/desktop.json" --output-dir "$Campaign/desktop-cooked" --quality 0
python tools/validation/assembly_measurements.py --compiler $Compiler --manifest "$Campaign/desktop.json" --output "$Campaign/desktop-measurements" --fresh-runs 3 --reuse-runs 5 --timeout 1800
```

The example uses quality 0, matching the report's explicit policy after preflight;
the manifest generator still defaults to quality 4. The example baseline matches
the desktop assembly's texture quality policy. For
the reference comparison, generate a separate manifest and cooked directory
with `--reference`, then run the same measurement command against it. Omit
`--quality` when collecting a corresponding single-model reference baseline.

```powershell
python tools/validation/sponza_assembly_manifest.py --root $Root --candles $Candles --manifest "$Campaign/reference.json" --output-dir "$Campaign/reference-cooked" --reference
python tools/validation/assembly_measurements.py --compiler $Compiler --manifest "$Campaign/reference.json" --output "$Campaign/reference-measurements" --fresh-runs 3 --reuse-runs 5 --timeout 1800
```

Keep the JSON reports, manifests, extracted-input identities, frozen compiler
copies and per-process logs together. The measurement scripts record exact
commands, executable identity, wall time, sampled Windows process memory and
failure status. Baselines also hash source dependencies; preflight records its
source images; assembly campaigns retain manifest/output identities and cooked
directory bytes. Assembly samples include `initial_disk_bytes`,
`peak_sampled_disk_bytes` and `final_disk_bytes` for the cooked tree, with a disk
scan approximately once per second. These sum file lengths, not allocated disk
blocks; the non-atomic scans can miss brief temporary peaks. Baseline `disk_bytes`
is a final snapshot of its whole campaign directory, including the compiler and
extracted inputs, so it is not package size alone or a temporary-disk peak.

Runs are serial fresh processes with warm/uncontrolled filesystem caches. The
assembly harness declares 48 GiB stage reservations and separately aborts at
16 GiB observed private commit; reservations are not measured memory usage.
Preflight uses a 4 GiB private-commit abort. Timeouts and memory aborts are failed
samples, not successful timings. Inspect every sample's `successful` and `aborted`
fields; preflight completes its report even when an individual sample fails.
Keep the host awake and avoid concurrent workloads when comparing repeated runs.
Engine integration and runtime performance remain separate verification work.
