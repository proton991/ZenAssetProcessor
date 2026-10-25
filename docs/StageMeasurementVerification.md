# Actual-pipeline stage measurement checkpoint

The ordinary/instrumented correctness preflight and seven alternating timing
rounds passed on 25 October 2026. The campaign separates actual parser, processing,
encoding, file/publication and package-read/decompression paths.

This processor-only checkpoint uses fingerprint
`8976c628910f91106f0055657a85e9ace6afa3efaf6748639b6cd7804ca04a4f`.
It measures the actual importer, cooker, encoder, package writer and reader
paths. Runtime adaptation/upload and GPU behavior remain outside this audit.
This is a bounded validation corpus, not a release or universal performance claim.

## Recorder and default-build behavior

[`StageMeasurements.h`](../src/Diagnostics/StageMeasurements.h) is a private,
header-only recorder enabled solely by the isolated evaluation project's
`ZENASSET_STAGE_MEASUREMENTS` definition. Default builds erase every scope and
counter macro, including argument evaluation; they have no clock/TLS operations,
profiler linkage, public profiling header or new public API. The ordinary probe
also binds a recorder and asserts that all production counters/intervals stay zero.

A caller-owned fixed-size statistics array is bound to the current thread for the
measured API call. RAII scopes record inclusive coordinator wall intervals and
subtract nested **same-thread** intervals for exclusive time. Worker threads do
not inherit the binding. Image/mesh coordinator scopes include scheduling and
waiting for drained work; they do not add worker CPU times. Import has one worker
plus its coordinator, not an inline or wholly single-thread executor. Parent
inclusive times must not be added to their children's inclusive times.

The focused recorder tests cover nested/exclusive arithmetic, idempotent stopping,
partial/failed paths, nested bindings, isolated worker bindings and stage labels.
The disabled test passes undefined tokens and side effects as macro arguments to
prove they disappear. Both isolated Release builds pass 2/2 tests; an earlier
Debug recorder build also passes 2/2.

## Actual boundaries

| Reported interval | Actual source boundary and included work |
| --- | --- |
| `source_acquire_parse` | glTF source acquisition/parser/normalization, `LoadFbxScene`, or OBJ tinyobj acquisition/parser; parser-owned I/O is included. FBX reparsing produces a second call when required. |
| `source_convert` | Imported glTF/FBX/OBJ conversion into the canonical scene. Includes its nested image and mesh intervals. |
| `source_images_io_decode_wait` | Drained glTF/FBX image tasks, synchronous glTF image-light face decoding, or uncached OBJ image acquisition/decoding. Includes scheduling/I/O; not isolated decoder kernel CPU. |
| `source_mesh_convert_wait` | Drained glTF/FBX mesh conversion or synchronous OBJ geometry conversion. |
| `imported_normalize` | `ProcessImportedAsset`, including canonical normalization and validation work. |
| `cook` and `geometry_process`, `texture_process`, `environment_process`, `page_process` | The corresponding actual `CookScene` processing calls; parents include their nested stages. |
| `texture_encode_qualify`, `hdr_encode_qualify` | Actual candidate block encoding and required qualification/fallback. Normal joint UV/LOD qualification is included. Mip generation remains texture processing. |
| `write_validate`, `serialize` | Writer validation and resource archive traversal/sizing/finish; directory/header hashing stays here. |
| `mesh_encode`, `transport_encode` | Actual semantic-mesh and Zstandard resource encoder paths, including their setup/selection/allocation. Attempts and accepted resources are counted separately. |
| `file_open`, `file_inspect`, `file_seek`, `file_read`, `file_write`, `file_flush`, `file_close` | Processor-owned file operations; completed transferred bytes are counted. These are OS-cache-inclusive library calls, not physical-device latency. |
| `publish`, `before_publish` | Atomic replacement and any before-publication callback. This campaign uses default non-durable publication and no callback. |
| `read`, `package_open`, `package_read` | Whole `ReadScene`, package opening/directory validation, and individual package resource reading. Header/directory hashing remains `package_open`. |
| `transport_decode_path` | Actual bounded streaming Zstandard resource path. Exclusive time includes staging/allocation/frame/control work after nested file reads and resource checksums are subtracted. |
| `resource_checksum` | Resource payload checksums; not every package hash. |
| `mesh_decode` | Actual semantic mesh decoding path, including its buffer/setup work. |
| `resource_deserialize_path`, `scene_validate` | Public scene reconstruction/resource lookup/limits/deallocation and final scene validation. Exclusive reconstruction time subtracts nested resource/codec scopes. |

Scope sites are in `src/Importers`, `src/Processing`, `src/Processor/Cook.cpp`,
`src/Writer`, `src/Reader` and `src/Format/FileIO.cpp`. Parser/tree cleanup and
other API setup outside named scopes remain in the separately measured API
residual. Named phase sums are not forced to equal the full API duration.

## Inputs and correctness

The fixed natural inputs are the self-contained `glTF-Binary` variants of
DragonAttenuation, DamagedHelmet and Fox from the local glTF-Sample-Assets tree.
They cover large geometry, texture-dominated storage, and skinned/animated data.
Copied input SHA-256 values, source READMEs and original source identities are
retained. Authored content is not modified.

Each natural scene has ordinary/instrumented source imports, followed by matching
canonical Cook/Write/Read paths using raw, Zstandard level 3, meshoptimizer, and
combined resource policies. Geometry cooking enables vertex-fetch optimization;
quantization and texture cooking are disabled for these natural cases. All four
profiles are correctness controls; raw and combined profiles are timed.

[`Fixture.h`](../tools/evaluation/stage_measurements/Fixture.h) defines an original,
bounded workload: one triangle/material; three 64x64 color/normal/occlusion images;
and a 16x16 HDR cube with three authored mips and six faces. Its recipe is stored
in the scene. Texture cooking enables role-aware mips and BC7/BC5/BC4/BC6H. The
preflight requires one accepted image in each BC4/BC5/BC7 format and one BC6H cube,
plus nonzero actual texture/HDR encoding counters. It is not a representative HDR
production-cost corpus.

Small preflight-only controls additionally exercise embedded-texture glTF,
morph/skin FBX, and original OBJ/MTL/TGA files. Physical-file totals finish at
1/1 for each self-contained source and 3/3 for OBJ. The OBJ's expected legacy-MTL
approximation diagnostic is identical across builds.

All 32 preflight cases pass. Full scene comparisons use the independent
[`SceneValueComparison.h`](../tests/SceneValueComparison.h), with complete
BuildMetadata/diagnostic comparison added by the probe. These run in separate
processes so the reference scene does not inflate measured memory. Every compared
output package also has identical SHA-256. Each build also reads each of the 13 profile packages with its recorder binding
active and writes a raw canonical snapshot afterward, outside that binding. These
cross-build decoded snapshots pass complete scene/metadata and byte comparisons;
the instrumented compressed reader is therefore checked beyond counts alone.
Read-side counts/diagnostics match the validated package; actual semantic/transport decode counters match its directory.
Read-byte counts equal header + directory + stored resource payloads. Writer-byte
counts additionally include rewriting the reserved header/directory prefix.

## BC6H defect found by the parity gate

The initial `d3f5f8ff99dc077a26940d00ef7f69fed0a0eadfcd02a4aee9d38d3f32b6869b`
preflight failed for the original HDR fixture. Each build was repeatable by itself,
but ordinary/instrumented builds differed in all 126 BC6H blocks. The maximum
decoded RGB difference was 0.0234375. Complete metadata and all scene fields
outside the environment matched. This was an actual payload discrepancy, not
provenance or directory bytes.

The pinned upstream scalar `MapColorsQuantized` loaded uninitialized
`INTColor::pad`; its scalar dot product also included that fourth lane, unlike the
SIMD RGB-only mask. `RoughMSE` additionally passed an uninitialized endpoint alpha
to the four-lane half conversion. The tracked
[`PatchBc6h.cmake`](../cmake/PatchBc6h.cmake) replaces the palette error calculation
with the existing RGB-only `Norm`, value-initializes the two HDR endpoints, and
initializes all four integer-color members. The last correction defines padding
before implicit endpoint copy assignment in Refine/OptimizeOne; it changes no
layout, size or alignment.
It validates the exact upstream and corrected normalized-content hashes, generates
a build-local copy and installs that corrected copy with the upstream license.
The fetched dependency cache remains unchanged.

The initial correction checkpoint `8fbe6200...` produced byte-identical complete
packages for the same fixture: SHA-256 `0cab8319b260c8ac3dc8b6eb7e73286516c93ff2d0e0b06e01c5cc5585278427`.
All 126 decoded blocks match exactly. The focused signed/unsigned stack regression
adds 14 reference encodes and 224 altered/repeated encodes across six constant
face colors plus one varying block, four stack patterns and two caller layouts.
Against the original encoder, 104 of 224 encodes differ; against the correction,
zero differ. Both retain finite, unit-alpha output within the 2% relative RMS
fixture limit. The same regression passes in the final Release and Debug full
suites (`build/processor-completion-verification/{release,debug}-tests.log`).
Regression logs are under `build/bc6h-regression/{prepatch,patched}-test.log`.
The final default-constructor hardening at `8976c628...` preserves all 345
nonmetadata resource payloads across all 13 cooked packages from `8fbe6200...`.
Only expected metadata identities change; all 126 original HDR blocks and their
decoded RGB values remain identical. The audit is retained at
`build/stage-measurements/bc6h-initialization/nonmetadata-equality.json`.
The preliminary `8fbe6200...` preflight is preserved under `build/stage-measurements/final`.
Original failing packages, binaries, source snapshots and logs remain under `build/stage-measurements/checkpoint`;
`bc6h-patch` contains the isolated correction probe and before/after decoded audit.

## Method and reproducibility

The host is Windows 11 build 26300, AMD Ryzen 9 9950X3D (16 cores/32 logical
processors). Both isolated builds use Ninja, CMake 4.4.2, MSVC 19.51.36256.0,
Release `/O2 /Ob2 /DNDEBUG`, C++20 and the dynamic CRT. Python is 3.14.7. No GPU is
used. Exact CMake caches, compile commands, generated headers, source lists and
hashes are copied to `final-initialized/provenance`; input/probe/source identities are checked
before and after the campaign.

Preparation and timing are separate. Timing uses fresh processes, seven rounds
with alternating ordinary/instrumented order and rotated workload order, plus a
warmup for every mode/case and no-op process baselines. The OS cache is warm;
there is no engine asset cache or OS-cache flush. CPU clocks and unrelated host
background activity are not controlled. Broad verification jobs are held outside
the timing window.

`source` times only `ImportScene`, then publishes the comparison package outside
the recorder. `cook-write` reads the canonical input before the recorder and
measures CookScene and WriteScene separately. `read` measures ReadScene directly.
Memory values are absolute process-lifetime peak working set/private commit from
the existing Windows helper. They include retained input/cooked scenes and any
untimed preparation/publication in that process. No baseline memory is subtracted.

```powershell
cmake -S tools/evaluation/stage_measurements -B build/stage-measurements/plain -G Ninja `
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_EXPORT_COMPILE_COMMANDS=ON `
  -DZENASSET_STAGE_BUILD_PIPELINE=ON -DZENASSET_STAGE_INSTRUMENT_PIPELINE=OFF `
  -DZENASSET_STAGE_DEPENDENCY_CACHE=E:/Dev/ZenAssetProcessor/build/packaging-release/_deps
# Repeat in build/stage-measurements/instrumented with INSTRUMENT_PIPELINE=ON.
cmake --build build/stage-measurements/plain --target stage_measurements stage_recorder_tests stage_recorder_disabled_tests --parallel 2
cmake --build build/stage-measurements/instrumented --target stage_measurements stage_recorder_tests stage_recorder_disabled_tests --parallel 2
python -B tools/validation/asset_processor_stage_measurements.py prepare `
  --plain build/stage-measurements/plain/stage_measurements.exe `
  --instrumented build/stage-measurements/instrumented/stage_measurements.exe `
  --models E:/Dev/glTF-Sample-Assets/Models `
  --source-list build/stage-measurements/plain/processor/FingerprintInputs.txt `
  --fingerprint 8976c628910f91106f0055657a85e9ace6afa3efaf6748639b6cd7804ca04a4f `
  --output build/stage-measurements/new-checkpoint
python -B tools/validation/asset_processor_stage_measurements.py measure --runs 7 `
  --output build/stage-measurements/new-checkpoint
```

Initialize the matching Visual Studio developer environment before configuring or
building. A new output directory is required; recorded checkpoints are never
replaced. Build configuration and preflight commands are also retained in the
JSON/log evidence. The measurement command above must run only after a complete
preflight and a quiet shared-host window.

## Results

The final campaign passed **238 measured API processes**, 14 measured no-op baselines, 34 case warmups and two baseline warmups. All 160 postflight full-package comparisons pass; all recorded identities remain unchanged. The 32 preflight comparisons include the 13 cross-build decoded-reader snapshots.

The table gives ordinary and instrumented API durations in **milliseconds, minimum / median / maximum**. Delta is the ratio of the two medians minus one; it is an observed paired difference, not a calibrated intrinsic profiler-overhead estimate. Negative deltas do not establish a speedup. Per-stage distributions, process CPU time and both memory distributions are in [`summary.md`](../build/stage-measurements/final-initialized/summary.md); every sample is retained in JSON.

| Case / API | Ordinary min / median / max | Instrumented min / median / max | Median delta |
| --- | ---: | ---: | ---: |
| Dragon-source / source | 74.436 / 83.021 / 89.949 | 77.171 / 83.337 / 87.404 | +0.38% |
| Dragon-raw-cook-write / cook | 13.665 / 16.802 / 20.384 | 14.013 / 18.485 / 21.438 | +10.02% |
| Dragon-raw-cook-write / write | 27.683 / 32.879 / 33.921 | 28.247 / 37.249 / 41.216 | +13.29% |
| Dragon-raw-read / read | 42.381 / 48.727 / 55.172 | 41.472 / 44.776 / 56.136 | -8.11% |
| Dragon-mesh_zstd-cook-write / cook | 13.725 / 18.299 / 20.144 | 15.060 / 18.178 / 19.154 | -0.66% |
| Dragon-mesh_zstd-cook-write / write | 133.643 / 138.968 / 145.725 | 125.486 / 137.361 / 140.657 | -1.16% |
| Dragon-mesh_zstd-read / read | 55.772 / 62.871 / 73.179 | 58.824 / 63.145 / 73.332 | +0.43% |
| Helmet-source / source | 117.995 / 121.191 / 141.327 | 117.962 / 126.415 / 136.332 | +4.31% |
| Helmet-raw-cook-write / cook | 1.024 / 1.129 / 1.224 | 0.956 / 1.179 / 1.499 | +4.43% |
| Helmet-raw-cook-write / write | 36.161 / 39.336 / 43.468 | 35.338 / 40.767 / 43.112 | +3.64% |
| Helmet-raw-read / read | 44.349 / 46.826 / 51.439 | 41.532 / 47.375 / 50.420 | +1.17% |
| Helmet-mesh_zstd-cook-write / cook | 1.060 / 1.103 / 1.611 | 1.108 / 1.210 / 1.364 | +9.76% |
| Helmet-mesh_zstd-cook-write / write | 238.455 / 267.229 / 288.023 | 242.644 / 268.892 / 294.335 | +0.62% |
| Helmet-mesh_zstd-read / read | 85.727 / 98.569 / 106.856 | 85.922 / 100.073 / 121.831 | +1.53% |
| Fox-source / source | 8.229 / 8.683 / 10.326 | 8.374 / 9.243 / 9.950 | +6.45% |
| Fox-raw-cook-write / cook | 0.316 / 0.322 / 0.518 | 0.305 / 0.361 / 0.487 | +12.28% |
| Fox-raw-cook-write / write | 2.468 / 2.742 / 3.384 | 2.611 / 3.051 / 3.658 | +11.28% |
| Fox-raw-read / read | 3.204 / 3.454 / 3.954 | 3.099 / 3.670 / 4.421 | +6.24% |
| Fox-mesh_zstd-cook-write / cook | 0.302 / 0.335 / 0.424 | 0.310 / 0.320 / 0.459 | -4.44% |
| Fox-mesh_zstd-cook-write / write | 5.645 / 6.479 / 8.371 | 5.994 / 7.391 / 8.386 | +14.09% |
| Fox-mesh_zstd-read / read | 4.101 / 5.101 / 5.555 | 4.801 / 4.968 / 5.304 | -2.62% |
| Original-mesh_zstd-cook-write / cook | 300.757 / 316.433 / 354.890 | 301.665 / 313.085 / 337.203 | -1.06% |
| Original-mesh_zstd-cook-write / write | 0.859 / 0.972 / 1.183 | 0.895 / 1.042 / 1.228 | +7.26% |
| Original-mesh_zstd-read / read | 0.847 / 0.941 / 1.108 | 0.829 / 0.918 / 1.214 | -2.43% |

Observed API median deltas range from -8.11% to +14.09%; small operations and host variation limit precision. Default production remains uninstrumented. No-op process wall time is 5.273 / 6.558 / 9.123 ms ordinary and 5.982 / 7.509 / 8.848 ms instrumented. Baseline peak working-set medians are 4.590 and 4.605 MiB; private-commit medians are 1.184 and 1.199 MiB. These baselines are reported, not subtracted.

### Source-stage attribution

These are instrumented medians in ms. Acquisition/parser, image and mesh values are inclusive coordinator wall intervals; conversion uses exclusive time to exclude its nested image/mesh intervals. Medians from different intervals need not add to the median API duration.

| Source | Acquisition / parser | Image load / decode / drain | Mesh conversion / drain | Other conversion, exclusive | Imported normalization |
| --- | ---: | ---: | ---: | ---: | ---: |
| Dragon-source | 3.673 | 46.398 | 29.692 | 0.097 | 1.403 |
| Helmet-source | 1.991 | 119.637 | 2.984 | 0.064 | 0.145 |
| Fox-source | 0.552 | 7.181 | 0.757 | 0.327 | 0.062 |

Helmet import is dominated by image acquisition/decoding/drain in this corpus (119.637 ms within a 126.415 ms API median). This includes the coordinator waiting for its single image worker; it is not isolated decoder CPU time.

### Combined encoding, file writing and publication

Instrumented medians in ms; the encoder helper intervals have no nested measured children here. File write, flush and atomic publication remain separate from encoding. Whole WriteScene also includes validation, serialization, checksums and other control work.

| Combined case | Mesh encode | Zstandard encode | File write | Flush | Atomic publish |
| --- | ---: | ---: | ---: | ---: | ---: |
| Dragon-mesh_zstd-cook-write | 38.062 | 72.723 | 1.922 | 0.002 | 0.169 |
| Helmet-mesh_zstd-cook-write | 3.926 | 232.218 | 4.757 | 0.002 | 0.164 |
| Fox-mesh_zstd-cook-write | 0.548 | 4.431 | 0.234 | 0.002 | 0.194 |
| Original-mesh_zstd-cook-write | 0.010 | 0.406 | 0.095 | 0.002 | 0.187 |

The original texture/HDR CookScene median is 313.085 ms. Actual texture candidate encoding plus qualification takes 30.297 ms; HDR face encoding plus qualification takes 282.721 ms. Each run records 21 texture candidates and 18 HDR face candidates, with accepted BC4/BC5/BC7 images and a BC6H cube. The bounded original workload establishes actual encoder-path coverage; it does not forecast arbitrary artwork cost.

### Package reading and decompression

Instrumented medians in ms. Transport is the **exclusive bounded decode path**, including allocation/staging/frame/control work; mesh decoding is its actual helper path. These are not relabeled whole ReadScene durations or codec-only microbenchmarks.

| Combined case | File read | Transport decode path, exclusive | Mesh decode | Reconstruction, exclusive | Whole ReadScene |
| --- | ---: | ---: | ---: | ---: | ---: |
| Dragon-mesh_zstd-read | 1.713 | 20.141 | 6.974 | 27.433 | 63.145 |
| Helmet-mesh_zstd-read | 4.650 | 60.442 | 0.663 | 22.938 | 100.073 |
| Fox-mesh_zstd-read | 0.069 | 2.029 | 0.075 | 1.490 | 4.968 |
| Original-mesh_zstd-read | 0.023 | 0.187 | 0.007 | 0.054 | 0.918 |

| Scene | Raw package bytes | Combined package bytes |
| --- | ---: | ---: |
| DragonAttenuation | 54,698,976 | 8,732,004 |
| DamagedHelmet | 86,116,204 | 21,944,028 |
| Fox | 4,674,660 | 160,240 |

The original combined package is 4,302 bytes. Combined resource compression reduces natural-package bytes here, while its warm-cache decoding work makes the natural ReadScene medians longer than raw. No cold-storage or runtime-load benefit is inferred.

### Absolute process memory

Medians of process-lifetime peaks in MiB. Each cell is **working set / private commit**, not live canonical scene size or GPU allocation. The complete min/median/max distributions are in the detailed summary.

| Case | Ordinary WS / commit | Instrumented WS / commit |
| --- | ---: | ---: |
| Dragon-source | 91.676 / 85.418 | 91.672 / 85.402 |
| Dragon-raw-cook-write | 102.461 / 99.730 | 102.430 / 99.738 |
| Dragon-raw-read | 70.137 / 66.816 | 70.160 / 66.828 |
| Dragon-mesh_zstd-cook-write | 121.379 / 118.348 | 121.406 / 118.363 |
| Dragon-mesh_zstd-read | 69.715 / 66.340 | 69.801 / 66.465 |
| Helmet-source | 112.109 / 104.781 | 112.113 / 104.785 |
| Helmet-raw-cook-write | 106.035 / 102.652 | 106.262 / 102.996 |
| Helmet-raw-read | 102.648 / 99.395 | 102.633 / 99.344 |
| Helmet-mesh_zstd-cook-write | 125.660 / 122.199 | 125.691 / 122.215 |
| Helmet-mesh_zstd-read | 102.527 / 99.180 | 102.621 / 99.344 |
| Fox-source | 14.074 / 10.129 | 14.324 / 10.203 |
| Fox-raw-cook-write | 14.172 / 10.590 | 14.199 / 10.574 |
| Fox-raw-read | 13.109 / 9.660 | 13.176 / 9.715 |
| Fox-mesh_zstd-cook-write | 21.625 / 17.902 | 21.660 / 17.879 |
| Fox-mesh_zstd-read | 13.219 / 9.672 | 13.246 / 9.684 |
| Original-mesh_zstd-cook-write | 5.766 / 1.750 | 5.797 / 1.758 |
| Original-mesh_zstd-read | 5.320 / 1.312 | 5.367 / 1.316 |

The largest observed median process peak is the Helmet combined Cook/Write process: 125.691 MiB working set and 122.215 MiB private commit in the instrumented build. This includes the canonical input read before timing, retained cooked output and writer scratch. It is not an admission-budget limit.

## Evidence

The [processor completion verification](ProcessorCompletionVerification.md) records
the final full-suite, independent HDR and installed/relocated SDK checks.

- `build/stage-measurements/final-initialized/results.json`: complete preflight, all individual measured samples, input/output
  identities, exact commands, counters and full comparison results.
- `build/stage-measurements/final-initialized/summary.md`: all API/process/stage
  min/median/max distributions; `final-initialized-timing.log`: seven completed rounds.
- `build/stage-measurements/final-initialized/logs/`: complete per-process outputs.
- `build/stage-measurements/final-initialized/sources/` and `final-initialized/provenance/`: frozen source,
  tool/build configuration, input attribution and corrected dependency header.
- `build/stage-measurements/bc6h-initialization/{plain,instrumented}-build.log`:
  final probe builds. `bc6h-patch/` holds initial corrected builds and
  `{plain,instrumented}-recorder-tests.log`: 2/2 focused tests each.
- `build/stage-measurements/bc6h-patch/{equality,decoded-equality}.log`: exact fixed
  fixture package/scene/block equality.
- `build/stage-measurements/environment-difference.log`: retained initial decoded
  mismatch; `preflight.log` and `checkpoint/`: failed pre-fix checkpoint.

No renderer integration, adapt/upload timing, new platform support, publication or
pinning action is included.
