# ZenAssetProcessor

Standalone scene import, asset processing, and `.zenasset` reading for ZenEngine
and other consumers. glTF/GLB, FBX and OBJ imports feed the v3 package format,
with optional lossless Zstandard chunk compression, meshoptimizer stream encoding,
texture cooking and atlas packing. The v3 schema remains unreleased; current
engine integration and rendered acceptance have separate gates.

The public format records use independent math and resource types. The reader
has no engine, graphics API, source parser, or encoder dependency. Source
processing is a separate optional target.

See [implementation status](docs/ImplementationStatus.md) for verified checkpoints
and the deferred integration and release gates.

Verified host coverage is Windows x64 Debug/Release. The
[installed SDK checkpoint](docs/ProcessorCompletionVerification.md) identifies the
exact tested builds and relocation checks; other hosts are not yet verified.
Development use remains unpinned, and release publication is a separate step.

Asset tests, corpus fixtures, quality checks and benchmarks are owned here. See
[validation tools](tools/validation/README.md) and the optional
[ZenEngine consumer tests](tests/integration/zenengine/README.md). Standalone tests
do not require an engine checkout.

## Build the format and reader

Use a C++20 compiler and CMake 3.23 or later. On Windows, run these commands from
an MSVC developer shell. Ninja is required by the supplied presets.

```sh
cmake --preset debug
cmake --build --preset debug
ctest --preset debug
cmake --install build/debug --prefix build/install-debug
cmake -S tests/consumer -B build/consumer-debug -DCMAKE_PREFIX_PATH=<absolute-install-prefix>
cmake --build build/consumer-debug
```

`ZenAsset::Format` and `ZenAsset::Reader` are exported by the installed CMake
package. `ZENASSET_BUILD_PROCESSOR=ON` enables the separately built source
processor when its dependencies are available. Reader-only builds fetch only
the bcdec block decoder. The reviewed meshoptimizer/Zstandard decoder subset
ships with the source tree, so no source importer or encoder archive is
downloaded or configured. See [Reader dependencies](docs/ReaderDependencies.md)
for provenance, integrity checks and updates.

## Build the processor and command-line tool

```sh
cmake -S . -B build/processor -DZENASSET_BUILD_PROCESSOR=ON -DZENASSET_BUILD_CLI=ON
cmake --build build/processor --config Release
ctest --test-dir build/processor -C Release --output-on-failure
cmake --install build/processor --config Release --prefix build/install
```

The processor owns pinned glTF/GLB, FBX and OBJ/MTL dependencies. Its exported
`ZenAsset::Processor` target includes the required static-library dependency
closure. See [third-party notices](THIRD_PARTY_NOTICES.md) for versions, licenses,
and redistribution files. Engine-owned types and graphics APIs are absent from
the public headers.

Source support matrices describe [glTF/GLB](docs/GltfImport.md), [FBX](docs/FbxImport.md), and the initial [OBJ/MTL subset](docs/ObjImport.md), including canonical semantics, diagnostics, image/deformation handling and resource limits.
Additional PLY/STL candidates remain disabled after an isolated [Assimp evaluation](docs/AssimpEvaluation.md).
Use [manifest batch builds](docs/BatchBuilds.md) for content-aware incremental jobs
and [CMake integration](docs/BuildIntegration.md) for build-system targets.

```sh
zenassetc cook model.glb --output model.zenasset --profile reference
zenassetc cook model.glb --output compressed.zenasset --compression zstd --zstd-level 3
zenassetc cook model.glb --output mesh-compressed.zenasset --compression zstd --mesh-compression meshopt
zenassetc cook model.glb --output optimized.zenasset --profile desktop --vertex-fetch on --compression zstd --mesh-compression meshopt
zenassetc cook model.glb --output textures.zenasset --profile desktop --texture-mipmaps on --texture-compression bc7
zenassetc inspect model.zenasset
zenassetc validate model.zenasset
```

The `reference` profile preserves imported values, using raw resources by default.
`--compression zstd` compresses chunks independently and retains raw bytes for
chunks that do not benefit. This changes file storage, not GPU texture formats
or runtime geometry precision. Static codec workspaces and input/output buffers
are checked against the configured budgets; frames require a known content size
and cannot use dictionaries, concatenated frames, or trailing data.
`--mesh-compression meshopt` adds lossless vertex-byte and index-sequence encoding
before optional Zstandard compression. It preserves index order for points,
lines, and triangles, including provoking vertices. Each stream retains its raw
representation when the complete encoded stream would be larger. Encoding runs
in bounded blocks so cancellation and scratch limits apply during processing.
`inspect` reads the container directory; `validate` decodes and checks the whole
scene. Cooked packages can be read without the source files. Commands emit JSON
results and diagnostics and return a nonzero exit code on failure. `--help`
lists supported limits and options. Optional array packing uses `--atlas array`;
see [array cooking](docs/ArrayCooking.md). The optional `--atlas spatial` path has
[explicit single-level eligibility and exclusions](docs/SpatialCooking.md).

[Matched codec measurements](docs/CodecMeasurementVerification.md) record package
size, read time and process memory for raw, Zstandard, meshoptimizer and combined
storage. Smaller files can require more read CPU time; combining codecs does not
always produce the smallest package. [Atlas measurements](docs/AtlasCookingMeasurements.md)
separately record cooking and publication costs for matching packing on/off inputs.
Packing can increase package size and processing memory. These are scoped local
measurements, and texture/atlas processing remains disabled by default.

Source import allows 16 GiB of cumulative decoded/converted image buffers by
default, including temporary decoder copies. This is an allocation allowance,
not the compressed file size or final texture size. For example, 72 RGBA8 4K
images occupy 4.5 GiB, but decoding them with temporary copies consumes at least
9 GiB of this allowance. Set `SceneImportContext::maxDecodedImageBytes` or the
CLI's `--image-budget` to choose a different bound; exceeding it fails the import
without publishing a partial scene.

`SceneImportContext::maxGeometryBytes` separately admits glTF/FBX geometry,
skin, morph, custom-attribute and animation storage before allocation. It defaults
to 16 GiB; `zenassetc cook --geometry-budget BYTES` changes it. Capacity growth and
conversion copies are charged conservatively, including overlapping worker results
and destination buffers, so the allowance can exceed the final geometry size.
Parser/codec-private memory, general scene metadata and allocator bookkeeping are
outside this contract; it is not a process-memory limit. OBJ keeps its separate
`maxObjGeometryBytes` policy. [Batch builds](docs/BatchBuilds.md) use smaller defaults. See the
[import admission verification](docs/ImportGeometryBudgetVerification.md).

The development `desktop` profile exposes explicit geometry and texture options.
`--vertex-fetch on` reorders vertex storage while preserving primitive and corner
order and the associated data domains. `--position-bits 1..24` requests optional
position quantization, guarded by measured error limits; zero disables it.
`--position-max-error` sets the absolute error in meters (default `0.000001`),
and `--position-relative-error` sets the instance-bounds fraction (default
`0.00001`). The accepted error is the maximum of those two limits. Unsupported
deforming, animated, or overlapping cases retain original data with diagnostics.
The reference profile rejects these geometry transformations. See
[geometry processing](docs/GeometryProcessing.md) for eligibility and limits.

`--texture-mipmaps on` fills missing mip levels. `--texture-compression bc7`
encodes eligible RGBA8 material images after role-aware filtering and per-mip
quality checks. Failed images retain their original pixels with diagnostics.
Both options default to off, and selecting desktop alone enables neither.
See [texture cooking](docs/TextureProcessing.md) for settings, shared-image
splitting, alpha handling, allocation limits, and the remaining format scope.

Consumers use `find_package(ZenAsset 0.1 CONFIG REQUIRED COMPONENTS Reader)` or
request the `Processor` component for source import and writing. Link
`ZenAsset::Reader` for `OpenPackage`, `ReadSceneMetadata`, and `ReadScene`, or
`ZenAsset::Processor` for `ImportScene`, `CookScene`, and `WriteScene`. Import
normalizes source data; cooking applies explicit processing settings once.
Adapt the returned immutable cooked scene directly or serialize that same owner;
the writer does not repeat cooking. A returned scene owns
its records and immutable image data independently of the package file handle.

See [the container contract](docs/PackageContainer.md),
[scene schemas and byte limits](docs/ScenePackageSchema.md), and
[reader fuzzing](tests/fuzz/README.md). A successful container read establishes
directory validity; use `ReadScene` for resource graph validation.

## Provenance

Initial scene semantics and byte-throughput kernels are extracted from
ZenEngine, copyright (c) 2024 proton991, under the included MIT license. During
the migration, the engine implementation remains until adapter parity is
verified. This repository does not depend on engine headers or its build tree.
