# Multiple models and large-scene processing plan

Investigated and reviewed against processor and engine source on 2026-11-06.
Status: the processor implementation is complete; the measured checkpoint and
remaining acceptance limits are recorded in [verification](MultiModelVerification.md).
The current user scope is ZenAssetProcessor only. M1/M2 are implemented, M0/M5
have processor evidence, and engine changes and runtime/GPU milestones remain
deferred. This does not close the complete cross-repository plan below. See
[assembly builds](AssemblyBuilds.md) and the [native assembly contract](SceneAssemblies.md)
for implemented APIs.
Processor implementation base: `77ab5b4a6e3eccc96877fbab2f8786bd3442934e`.
Engine checkout: `0dc3f746` on `hybrid-gi`. Uncommitted G-buffer culling work appeared
in that checkout during the review. It adds per-submesh GPU frustum culling and
leaves the blended-triangle path below unchanged.

## Decision

Add a native scene assembly that references independently cooked models. Reuse
`BuildSceneBatch`, the existing importers, and `.zenasset` resource encodings.
Do not merge all source models into one canonical `Scene` before cooking.

Start with one `.zenasset` per source model and a small assembly manifest. Make
build memory and engine ownership improvements part of this work: merely loading
five current packages concurrently would retain too many decoded copies. Add
independently addressable geometry blocks and texture mips if the measured
per-model path cannot meet the acceptance budgets. Physical pack files can remain
an optional distribution choice; logical resource identity must not depend on
which file contains the bytes.

This workload does not establish that a single file is too large for the format.
The existing container already has 64-bit offsets and per-resource compression.
The immediate limitations are composition, whole-scene ownership, eager decoding,
and incremental-build cost. A small manifest plus five native packages is the
lowest-risk first implementation, with a defined route to finer resource loading.

Three further decisions come from the source review:

- The assembly is a package in the existing container with one new required
  resource type, not a second file format or parser.
- The engine appends each model into one canonical `SceneAsset` and then runs the
  existing single-scene load path once.
- Block-compressed textures with packaged mips are required for the complete
  corpus on this 16 GiB device. The RGBA8 path leaves too little memory for
  acceleration structures and GI; see [Runtime memory budget](#runtime-memory-budget).

Composition alone does not make the scene usable. Tree-leaf blending, the
single-draw candle mesh, and acceleration-structure and GI growth are renderer
risks that need their own evidence; see [Renderer scale risks](#renderer-scale-risks).

## Local corpus and evidence

Read-only inventory of `E:/Dev/main_sponza` found these five distinct glTF inputs:

| Model | Source location relative to corpus root | Triangles | POSITION vertices | Images | Light nodes |
| --- | --- | ---: | ---: | ---: | ---: |
| Main | `NewSponza_Main_glTF_003.gltf` | 3,747,018 | 2,049,137 | 72 | 24 |
| Curtains | `pkg_a_curtains/NewSponza_Curtains_glTF.gltf` | 1,997,366 | 1,059,862 | 8 | 0 |
| Ivy | `pkg_b_ivy1/pkg_b_ivy/NewSponza_IvyGrowth_glTF.gltf` | 5,065,558 | 3,268,629 | 3 | 0 |
| Trees | `pkg_c_trees/NewSponza_CypressTree_glTF.gltf` | 2,023,747 | 1,714,610 | 4 | 0 |
| Candles | Inside `pkg_d_10k_candles.zip`: `pkg_d_10k_candles/NewSponza_4_Combined_glTF.gltf` | 19,540,000 | 12,925,581 | 0 | 10,000 |
| Total | Five models | 32,373,689 | 21,017,819 | 87 | 10,024 |

The candle filename is misleading: its mesh is named `Candles`; it is not the
complete assembled Sponza scene. Its geometry is baked into one mesh with three
primitives. The 10,000 light nodes reference one light definition. None of these
five exports shares a mesh between mesh nodes, so preserving authored instancing is
necessary for the general feature but will not by itself shrink this corpus.

The candle mesh is still flattened instanced content. Across 10,000 candles, wax has
exactly 817 vertices and 1,554 triangles per candle, flame has exactly 147 and 272,
and wick has exactly 128 triangles (its vertex count is not a whole multiple). A
read-only sample of the first 300 wax candles found candle 0's vertex order and
pairwise distances repeated to float rounding, at scale 1.0. The other candles and
the wick/flame primitives remain unchecked. If the pattern holds, the model is 1,954
unique triangles plus 10,000 transforms; see the M4 route below.

The candle wax primitive alone has an estimated 1.026 GiB of canonical vertex/
index data, so even splitting only at primitive boundaries cannot guarantee small
working buffers. Candles have no `TANGENT` attribute and no normal maps. The
processor has no tangent generator: the glTF importer stores zero tangents, so the
baseline has no tangent-generation cost for this model.

There are 139 meshes, 457 primitives, 40 materials, and 10,181 nodes overall.
All five glTFs have no skins or animations; separate fixtures must cover those
semantics. Preserve authored node transforms, including the ivy root scale of
100. Initially place every model with an identity assembly transform, then verify
alignment visually. Apply any engine centering/scaling to the assembled world once.

The conservative accessor bounds of curtains, ivy, trees and candles all fall inside
main's, (-16.25, -1.13, -9.40) to (20.00, 18.76, 14.36). The engine's normalization
should therefore not change when the other models are added, which keeps existing
normalized camera poses valid. M1's aggregate bounds confirm this.

Only two materials blend. `LeafSpring` (trees) is double-sided `BLEND` on 1,354,570
triangles with no `alphaCutoff`, and main's `dirt_decal` is `BLEND` on 8,140
triangles. No material uses `MASK`; ivy leaves, curtains and candles are `OPAQUE`.
Main and trees use up to two UV sets per primitive, while curtains, ivy and candles
use one, so composed models have different `vertexTexCoordSets`.

| Storage/working-data measure | Bytes | GiB |
| --- | ---: | ---: |
| Existing corpus directory, including duplicate export formats and archives | 24,379,911,094 | 22.706 |
| Five glTFs, their BINs and referenced PNG paths, including candle ZIP member sizes | 3,645,943,238 | 3.396 |
| Referenced PNG source files | 2,359,280,651 | 2.197 |
| RGBA8 base-level pixels for 87 image paths | 5,838,471,168 | 5.438 |
| Estimated canonical vertices and indices: `112*vertices + 4*indices` | 2,742,479,996 | 2.554 |
| Candle canonical vertices and indices alone | 1,682,145,072 | 1.567 |

All 87 referenced PNGs are 4096 x 4096. SHA-256 identifies 84 distinct PNG contents:
three metal-door maps are duplicated between main and curtains. Sharing those
three base-level RGBA images saves 192 MiB before mipmaps; cross-model texture
deduplication is useful but is not the main memory solution here. Full RGBA mip
chains for the 87 paths are approximately 7.25 GiB before duplicate owners,
role-dependent variants, decoder scratch, and staging.

The geometry estimate excludes UV side streams, node/mesh metadata, runtime
deformation storage, allocator overhead and GPU acceleration structures. Counts
come from JSON accessors, not a successful full import. Texture memory is an
RGBA8 footprint calculation, not measured process memory.

The candle ZIP is 3,190,731,713 bytes and expands to 9,926,405,859 bytes in full.
Only its glTF and BIN, totaling 654,755,780 bytes, are needed for the selected
test. Extract only these members into a generated test-input directory, preserve
their relative URI, validate resolved destinations, and record CRC/content hashes.
Do not ingest the alternate FBX/MAX/USD exports as additional models.

Local evidence, generated without modifying source assets. These links point into
the ignored `build/` directory and exist only on this machine, so M0 moves the
inventory script to `tools/` and records a summary in a tracked document:

- [Reproducible inventory script](../build/multi-model-investigation-20261106/sponza_inventory.py).
- [Inventory, hashes, bounds, dependencies and ZIP members](../build/multi-model-investigation-20261106/sponza-inventory.json).
- [Machine snapshot](../build/multi-model-investigation-20261106/machine.json): Ryzen 9 9950X3D,
  16 cores/32 threads, approximately 31.11 GiB physical RAM, RTX 5080, and
  approximately 265.5 GiB free on E: at observation time.
- [Default batch probe](../build/multi-model-investigation-20261106/default-probe/probe.json):
  installed Release compiler SHA-256
  `86e654a2f1ae5d9070613afa0e24963d330164218dda0fcdc4172420595de579`.
  One main-scene job with default limits failed with exit code 3 / status 7:
  `Decoded image buffers exceed the configured byte limit`. Nothing was published.
  Its approximately 1.98 seconds is a single admission-failure observation, not
  a cook-throughput baseline. The installed executable is identified separately
  from the audited source checkout.

The existing engine package for the main model alone,
`45805307d6863205.zenasset`, is 2,107,087,334 bytes (1.962 GiB). Its directory shows
that it is not a cooked texture package:

| Resource | Canonical bytes | Stored bytes |
| --- | ---: | ---: |
| 72 images, RGBA8 base level only, Zstandard | 4,831,840,800 | 2,030,582,979 |
| Vertex and index streams, meshoptimizer plus Zstandard | 274,467,560 | 57,123,453 |
| UV side stream, 24 bytes per vertex, Zstandard | 49,179,296 | 15,019,036 |

The engine generates RGBA mips during upload for this package. Applying these
ratios to the corpus is an unmeasured estimate: geometry and UVs store in about
0.7 GiB, so the five packages total about 2.9 GiB with lossless RGBA8 base levels,
or about 2.4 GiB with BC textures and each mip stored once.

Historical ZenApp logs show approximately 13.64 seconds between the first startup
log and the first frame graph for that package: 8.03 seconds before renderer
configuration (package read, adaptation and runtime build) and 5.61 seconds for
device creation, GPU preparation and uploads. This is a single Nsight-launched
observation, neither an isolated loader timing nor a combined-scene result; see the
existing [ZenApp Sponza capture report](../../ZenEngine/Doc/ComputeGI/ZenAppNewSponza4KNsightProfile.md).
That run's first field generation logged a 4,127,801,344-byte scratch budget. The
budget is half of free device memory, so about 7.7 GiB was free with main loaded.
The RTX 5080 reports a `maxStorageBufferRange` of 4,294,967,295, so the 2.19 GiB
combined vertex buffer fits. The limit is about 38.3 million 112-byte vertices, the
same bound as the package chunk.

No new full cook, combined native load, or rendering benchmark was run during this
planning investigation.

## What must change

| Current implementation | Implication |
| --- | --- |
| [Build API](../include/ZenAsset/Processor/Build.h) and [batch contract](BatchBuilds.md) already accept multiple independent jobs | Extend this path with assembly output; avoid a second batch scheduler. |
| [Batch preflight/execution](../src/Processor/Build.cpp) imports changed sources twice; reuse calls full `ReadScene`; an unchanged job hashes all inputs three times (`Reuse`, `RunJob` entry, reuse branch) | A large no-op must not decode gigabytes or read 3.4 GiB of sources three times, and changed builds should import once while preserving dependency/alias checks. |
| [Build fingerprint](../cmake/BuildFingerprint.cmake) hashes every processor source file, and reuse requires an exact match | Any processor commit invalidates every cooked model, and ZenEngine follows processor main. See [Open decisions](#open-decisions). |
| [CookScene](../src/Processor/Cook.cpp) copies `Scene` for enabled operations; callers retain the original through writing | Add an internal consuming path and shorten owner lifetimes while preserving the immutable public API. |
| [Image schema](../src/Format/SceneSchema.h) serializes both `bytes` and `mips[0]`, and [validation](../src/Format/Validation.cpp) requires them to match | A mipped image holds its base level twice, on disk and in memory. Zstandard's capped 8 MiB window cannot remove a 16 MiB duplicate. A new image schema should store each mip once and drop the in-memory copy; keep compatibility decoding for old assets. |
| [Resource layout](../src/Format/SceneResources.h) has singleton vertex/index/UV streams; [ReadScene](../src/Reader/Scene.cpp) is eager | Per-model files bound work only at model granularity. Large models need typed block reads if that bound is insufficient. |
| The [index stream](ScenePackageSchema.md) stores absolute vertex numbers | Composition must rewrite every index value with checked addition, not only `firstVertex`/`firstIndex` ranges. |
| [Scene](../include/ZenAsset/Format/Scene.h) stores UV sets as one `std::vector<Vec2>` per vertex | `ReadScene` makes one heap allocation per vertex, 12.9 million for candles alone. Flatten this in the same schema change as mips if M0 shows that it matters. |
| [Writer](../src/Writer/Scene.cpp) serializes one whole resource at a time; [compression](../src/Writer/Compression.cpp) allocates resource-sized output | Reuse the container writer, but page oversized resources or provide bounded output streaming. It does not currently buffer the whole file. |
| [Package limits](../include/ZenAsset/Reader/Package.h): 16 GiB file/aggregate transport-decoded bytes, 4 GiB chunk; [scene limits](../include/ZenAsset/Reader/Scene.h): 24 GiB allocation/16 GiB runtime geometry; [engine read limits](../../ZenEngine/ZenCore/Include/AssetLib/SceneAssetIO.h): 16 GiB allocation | These are configurable admission limits, not process-memory guarantees. Raising only file size is insufficient. |
| `desktop` leaves processing disabled; [texture maximum dimension](TextureProcessing.md) only controls eligibility; [engine texture creation](../../ZenEngine/ZenCore/Source/Graphics/RenderCore/V2/TextureManager.cpp) uses one mip level for a block-compressed image without packaged mips | Select codecs explicitly and enable `generateMipmaps` with them. Never describe the maximum dimension as texture resizing or silently lower source resolution. |
| [Runtime launch settings](../../ZenEngine/ZenCore/Source/Runtime/RuntimeLaunchSettings.cpp) accept one prepared `.zenasset` | Add native assembly support through setup validation, loader and ZenApp. |
| [Engine adapter](../../ZenEngine/ZenCore/Source/AssetLib/ProcessedSceneAdapter.cpp), runtime builder and RenderScene copy geometry/images; no path releases CPU pixels after upload | Current static path retains four vertex arrays and two index arrays, plus textures/UVs. Ownership changes are essential for this corpus; see [Runtime memory budget](#runtime-memory-budget). |
| [ZenApp](../../ZenEngine/ZenApp/RuntimeApplication.cpp) loads the package before creating the device | `RenderScene::GetResourceError` runs only after a full CPU load. Assembly counts allow the same check to fail earlier. |
| [SceneLighting](../../ZenEngine/ZenCore/Include/Graphics/RenderCore/V2/SceneLighting.h) supports 32 lights | Preserve all 10,024 serialized light instances; never claim that current rendering evaluates them all. |

The 112-byte canonical vertex stream reaches the default 4 GiB chunk boundary at
about 38.35 million vertices. This corpus is below that count, but runtime copies
and texture data still make a flattened all-model cook/load a poor default.
Some counts/ranges remain 32-bit; composition must use checked arithmetic and
fail before allocating or narrowing invalid ranges.

## Runtime memory budget

These are arithmetic estimates from the inventory and current engine layouts, not
measurements. M0 and M3 replace them.

| GiB | Main today | All five, RGBA8 | All five, BC |
| --- | ---: | ---: | ---: |
| Vertices, indices, UVs and voxel triangles | 0.37 | 3.66 | 3.66 |
| Textures with full mips | 6.00 | 7.25 | about 1.75 |
| Free device memory, before AS growth and GI scratch | 7.7 (logged) | about 3.1 | about 8.6 |

The free-memory row applies the two rows above it to the logged 7.7 GiB. It excludes
BLAS growth and field scratch. Field scratch was 0.89 GiB for main; scaled linearly
by triangle count it is about 7.7 GiB, while its budget is half of free memory. RGBA8
would leave about 3.1 GiB for all of that, so BC textures are a prerequisite for M3,
not only a size comparison. The BC column assumes every texture qualifies; each RGBA
fallback adds about 64 MiB.

Under current ownership the complete corpus would retain about 17 GiB of CPU memory
after load: 8.8 GiB in four vertex arrays, 0.7 GiB in two index arrays, 2.2 GiB of UV
data and 5.4 GiB of RGBA8 pixels. That misses the 16 GiB target before counting
transient peaks, when the adapter also holds second copies of images and geometry.

## Assembly and resource contract

Use an authoring build manifest with stable model keys, source paths, selected
source scene, processing settings, instances and transforms. Extend the existing
strict manifest parser with an explicitly versioned assembly form. Keep v1 batch
behavior unchanged. Discovery may suggest glTF files, but an explicit manifest
decides which exports belong in the scene.

Produce a versioned native assembly, provisionally `.zenscene`, containing model
package references, expected content identities, ordered instance records,
placement transforms, per-instance light policy, aggregate bounds/counts, and
selected camera/environment. Runtime references resolve relative to the assembly,
need no source files, and must remain valid when the cooked directory is relocated.

Store the assembly in the existing package container with one new required resource
type. This reuses the header and directory checksums, bounded `SceneArchive`
decoding, atomic publication and corruption tests, and adds no parser to
`ZenAsset::Reader`. `OpenPackage` already rejects unknown required types, so older
readers fail explicitly. Exact suffix/schema naming is an implementation detail to
freeze with format tests before integration.

Identify a referenced package by its file size and stored header hash. That hash
covers the directory hash, which covers every resource checksum, so a reference can
be verified when the package opens, without reading payloads. `Package` needs a
small public accessor for it. Payload corruption is still detected on read.

Model-local IDs remain local. A reference is `(model key, local resource ID)`;
an instance has its own stable instance key. Repeated instances share immutable
geometry/material/image data and own placement transforms. Preserve namespaced
animation data and targets. The first runtime integration retains the current
single selected clip/time policy; independently timed simultaneous instances
require a later playback-state change and are not promised by ID remapping.
Explicit camera/environment selection avoids arbitrary last-model-wins behavior.

For the first engine integration, read and adapt models sequentially, appending each
with checked remapping into one `SceneAsset` preallocated from the assembly's
aggregate counts, since the renderer uses global arrays today. Release each temporary
model after appending. `CompleteSceneLoad`, `PrepareScene` and `CommitScene` then run
once on the merged asset, so the runtime builder, its default material and the
renderer keep their single-scene assumptions. Merging `zenasset::Scene` objects first
would keep every model resident, and merging `sg::Scene` objects would duplicate
`BuildNodes`. Share static
runtime geometry between loader/render owners; allocate mutable posed geometry
only where deformation or an explicit geometry edit requires it. Define copy-on-
write for `RenderScene::UpdateVertices` and instance-local edits so changing one
instance cannot mutate another instance's shared data. This is a bounded transition design, not a
claim of zero-copy compatibility between different processor/engine types.

Remapping must cover roots, parents, children and `morphWeightsSource`; every index
value as well as `firstVertex`/`firstIndex`; UV ranges; mesh and material references,
including per-node bindings and primitives with no material; image, texture, sampler
and `linearSource` references; overrides, skins/joints, morphs, variants and the
variant name table; custom attributes; cameras/lights; image-based lights; and all
typed animation targets. Merge `vertexTexCoordSets` as a union and move each model's
per-vertex coordinates to the merged compact slots, because this corpus mixes one-set
and two-set models. `RenderScene` uses one UV stride for the whole buffer, so a third
set anywhere adds 16 bytes per vertex everywhere. Define what a merged asset keeps of
`sourceDocument`. Preserve source hierarchy and apply one world normalization after
composition. Check aggregate GPU limits with `RenderScene::GetResourceError`, from
assembly counts where possible. Commit the candidate once through the current
`PrepareScene`/`CommitScene` path; failure leaves the active scene intact.

If this model-granularity design misses the budgets below, evaluate candle
re-instancing first, because it addresses the largest model directly. An explicit,
opt-in cook step would detect repeated components, verify each against a stated
position tolerance, and emit one mesh plus 10,000 node transforms. If the sample
above holds for all candles, this removes about 1.57 GiB of canonical geometry and
most of the largest BLAS, and per-submesh culling can then reject individual candles.
It produces about 30,000 draw records per raster pass, because draws are per node and
submesh. The uncommitted G-buffer work would cull those on the GPU, but the other
raster passes need checking. It does not shrink the per-instance voxel triangle
buffer. It also changes authored node structure, so it must never run by default and
must remain visible in diagnostics.

If budgets are still missed, extend native resources
with mesh-local geometry blocks and independently stored mip payloads. Start by
measuring 16-64 MiB decoded geometry blocks, including splitting a single oversized
primitive; preserve primitive topology, material assignment and corner order.
Metadata must describe bounds, sizes, format, dependencies, checksum and codec
before payload allocation. Introduce typed selective reads rather than treating
raw `Package::Read` bytes as an already validated scene. Older readers must reject
unsupported required schemas explicitly; keep reading old `.zenasset` packages.

Shared texture identity includes source content, semantic role/transfer function,
format, mip policy, quality/settings and tool/schema version. Samplers and UV
transforms remain views. Share only compatible cooked results; identical source
PNGs used as normals and color do not necessarily yield interchangeable pixels.
Use SHA-256 or equivalent strong content identity for immutable shared objects;
existing XXH64 fingerprints remain cache/checksum mechanisms, not security claims.

Initially retain coarse model files. Do not create thousands of tiny files or
one universal shared bundle that every scene must load. If finer resources are
needed, store immutable objects first and optionally gather them into indexed,
seekable packs with independent chunks; avoid whole-pack compression. Define
dependency tracing and reader lifetimes before explicit garbage collection.

## Build and runtime performance work

Choose import-once staging for the new assembly build path. Import/cook each
changed model under admission control into a generation-specific temporary
package, recording complete dependencies. After every job is planned, run global
input/output alias checks and recheck input identities. Only then publish immutable
generation packages and atomically replace the assembly last. A failed build
leaves the previous assembly and its dependencies usable. Do not overwrite model
files still referenced by the old assembly. Bound and report temporary disk use;
do not hold all imported scenes in memory to avoid the second import.
Share implementation helpers with existing batch jobs, but retain v1 publication
semantics: successful jobs may remain committed when another executing job fails.
Assembly generations add a transaction at the assembly reference boundary; they
must not silently change the public v1 batch contract.

Name each model package by its identity, for example
`models/<key>-<cache key>.zenasset`, and never overwrite one. An unchanged model keeps
its file, a rebuild writes only changed packages plus a new assembly, and no
generation directories are needed. Explicit cleanup deletes only files that no
retained assembly references.

Preserve existing same-size/time content-change detection, missing-preferred-path
tracking, source mutation checks, cancellation and corruption detection. A warm
build may use metadata plus streamed payload checksums and a recorded successful
semantic-validation identity for unchanged compiler-produced output. It must not
replace full validation with timestamps or metadata alone. Validate newly produced
resources, and retain an explicit full semantic `validate` path. Start with
bounded integrity scanning; optimize hashing further only from measured evidence.
Bind a validation record to exact package content plus schema and validator/build
identity. If that record is missing or mismatched, perform full semantic validation
before reuse; successful stored-payload checksums alone cannot substitute for it.

Hash each input once per build instead of the current three passes. A placement-only
edit cannot meet its gate while it hashes 3.4 GiB of sources, so provide an explicit
assembly-only mode that uses recorded model identities, reads no sources, and reports
that it did not check source freshness.

Use one shared byte-admission policy across import, decode, cooking, encoding and
write scratch. Begin with one large job at a time, using the existing intra-model
workers; allow two jobs only when measured headroom permits. Report configured
reservations and observed working set/private commit separately. Source parser
allocations outside current accounting must be included in measurements and
documented; an allocation allowance is not a hard RSS ceiling.

Introduce an internal ownership-consuming cook path so the CLI/batch can release
replaced image buffers and avoid unnecessary geometry copies. Preserve immutable
public `CookScene` behavior. Start by releasing the imported owner before writing
when the cooked owner is independent, then measure whether per-image import/cook
staging is necessary for the main scene. Remove duplicate base mip persistence
with a versioned schema and compatible reader.

Compare raw, Zstd level 3, meshoptimizer and their combination on this corpus.
Choose based on package size and read/cook time together: existing small-scene
measurements show combined codecs are not invariably smallest. Cook BC7 color/BC5
normal/BC4 eligible scalar textures with generated mips for the desktop profile,
preserving packed channels, alpha coverage and existing quality limits, and compare
them with a lossless reference profile. Report RGBA fallback counts/bytes. Keep full
authored 4K resolution for primary acceptance. BC7 quality 4 is the default, and
[TextureProcessing.md](TextureProcessing.md) records that its offline cost still
requires throughput work, so nothing yet supports the 15-minute cook target at 4K.
Time one 4K texture per role at each quality before committing to a full cook.

For scale intuition only, 84 unique 4K textures at 8 bits/pixel with complete mips
would occupy approximately 1.75 GiB if each had one compatible BC7/BC5 output and
each mip were stored once. Actual output can differ due to roles, BC4 eligibility,
quality fallbacks and retained records; this is not a promised result.

Reuse the engine's existing bounded `StagingUploadQueue` (default 64 MiB pool),
including its wait/peak counters. Avoid a second upload scheduler. Stream validated
payload ranges into that queue where possible, avoid assembling another entire
mip-chain copy, and release CPU pixels after all required uploads/CPU consumers
complete. Keep resources alive until GPU work completes. Loading every shard
eagerly is not streaming; full-scene resident GPU bytes must still fit the device.

## Milestones and exit criteria

| Milestone | Work | Required exit evidence |
| --- | --- | --- |
| M0: Baseline and fixture preparation | Selectively extract candle glTF/BIN; freeze input/settings/build hashes; choose BC quality from single-texture timings; run existing serial per-model cooks with explicit limits and memory monitoring; load each model alone in ZenApp on both backends; log per-mesh AS sizes and field capacities; check all 10,000 candles for repetition; measure the `LeafSpring` alpha distribution | Successful or explicit bounded failure for all five sources; per-stage time, peak memory, final/temp bytes; per-model load time, GPU memory, AS size, field status and frame time; no inferred throughput from metadata; [renderer risks](#renderer-scale-risks) answered or explicitly deferred |
| M1: Native composition | Versioned authoring/native assembly, stable references/instances, existing batch reuse, relocation and source-free reading | Two-model fixtures and all-five metadata/count/bounds checks; old single-model API/tests still pass |
| M2: Bounded incremental builds | Import-once staging, publish-last assembly, consuming ownership, mip storage correction, content-aware reuse | No double source import, unchanged jobs do not fully decode scenes, interruption/corruption tests preserve prior usable generation |
| M3: Engine composition and ownership | Assembly setup/loader, checked `SceneAsset` append, one normalization, shared static storage, CPU pixel release, BC package loading, bounded upload, per-instance light policy, renderer-risk responses | All five geometries/materials displayed together in ZenApp; exact counts and source-free load; measured CPU/GPU memory, frame time and GI/AS readiness within gates |
| M4: Resource granularity if required | Candle re-instancing if M0 confirms repetition; then geometry blocks, selective typed reads, per-mip/shared payloads, then GPU block addressing if aggregate GPU limits require it | Demonstrated budget/latency improvement over M3 with identical reference semantics, or stated tolerances for re-instancing; no claims based on file splitting alone |
| M5: Acceptance | Fresh/no-op/incremental builds, native loading, failure recovery and rendered quality comparisons | Recorded results against the targets below; limitations and failures remain explicit |

M0-M3 and M5 are the minimum delivery. M4 becomes required if M3 misses the agreed
memory/load gates or a model exceeds reader/device limits; do not declare the
feature finished on the basis of successful serialization alone.

The first implementation slice should be M0 followed by M1 on small fixtures and
main+curtains. M2 and M3 can then proceed in parallel: main+curtains in ZenApp needs
no M2 work, and it exposes engine problems before the build transaction is written.
Follow with cumulative main+curtains+ivy, then trees once the leaf-blending decision
is made, then candles. The candle geometry and 10k-node hierarchy are the largest
stress case, so they must be included before declaring acceptance.

M0 build settings need care. `desktop` alone enables nothing, so each job must set
`generateMipmaps`, `compressBC7`, `compressBC5Normals` and `compressBC4Occlusion`.
`zenassetc build` exposes only `--job-memory`, which gives decoded images two ninths
of its total. Main's 72 images need 4.5 GiB, so `--job-memory` must be at least
20.3 GiB and `--memory-budget` must be at least the same. These are admission numbers
rather than usage, so report working set and private commit beside them.

## Proposed performance gates

These are provisional engineering targets for this 32 GiB / RTX 5080 host, not
measurements or guarantees. M0 must establish the baseline and explain any target
revision before optimization results are accepted. Use the complete five-model
set, full authored resolution, and the same explicit quality policy for comparisons.

| Measure | Initial target |
| --- | --- |
| Fresh complete cook | At most 15 minutes for the chosen desktop quality policy; no more than 1.2x the sum of matched one-model cook times; each changed source imported once |
| Unchanged build, warm filesystem | At most 10 seconds; zero source image decode/geometry conversion and zero rewritten payloads; each input hashed once |
| Placement-only assembly edit | At most 1 second of assembly-only build work excluding process startup; no source read, model cook or payload rewrite |
| One-model or one-texture edit | Rebuild only affected model/resource outputs; report read/hash/cook/write work separately; no unrelated payload rewrites |
| Processor peak memory | At most 16 GiB private commit and peak working set on the full build; admissions bounded with headroom for OS/other processes |
| Cooked disk footprint | Initial target at most 4 GiB for quality-accepted desktop output (estimated 2.4 GiB with each mip stored once, 3.7 GiB with today's duplicate base level); publish actual raw/reference comparison and fallback reasons |
| Retained disk after explicit cleanup | Current cooked generation plus declared reusable cache budget; no unbounded old generations; separately report old+new overlap during publication |
| Temporary disk during rebuild | At most old generation + new generation + 2 GiB additional scratch, excluding source data; validate before admitting work |
| Full native load to first complete geometry/material frame | At most 30 seconds warm filesystem / 60 seconds controlled cold filesystem; completion includes required uploads, report GI/AS readiness separately |
| Runtime CPU and GPU memory | Initial CPU peak target 16 GiB; GPU residency at most 80% of measured available device budget, including staging/AS/render targets for the recorded configuration |
| Frame time at fixed views | At most 33.3 ms median GPU frame on HWRT at 3840 x 2160 and base clocks, with render-thread CPU time reported separately and Compute GI reported on the same views. Main alone measures 17.3 ms HWRT and 26.9 ms Compute GI |
| GI and ray readiness | Both backends become ready without a provider fallback; report AS bytes and build time, field generations, retries and whether field capacities were memory-limited |

Keep at least three fresh-process repetitions for expensive cooks and five for
native loads/no-op builds; report every sample plus median/range. Do not label a
new process as a cold filesystem run. Measure cold storage explicitly or mark it
unmeasured. Do not run heavyweight jobs concurrently with benchmark repetitions.

Capture wall/CPU time for dependency scanning, import, image decode, cooking,
encoding, write/flush, native read, adaptation, upload and first frame. Record
sampled/lifetime working set and private commit, stage allocation peaks, bytes
read/written, final versus temporary/cache disk bytes, GPU residency and staging
waits. Preserve executable/build/input/setup hashes and exact arguments.

## Correctness and integration tests

Test mixed glTF/FBX/OBJ assemblies, repeated instances, duplicate local names/IDs,
relative paths and relocation, source-scene selection, absent dependencies,
deterministic output, corrupt/truncated chunks, oversize declarations, cancellation,
source edits during build and interrupted publish. Synthetic >4 GiB boundary
cases should test directory/count arithmetic without requiring giant CI fixtures.

Use small skinned/morph/animated fixtures to cover remapping that Sponza does not
exercise, including camera/light/material animation targets, UV maps, variants
and negative/nonuniform placement scales. Verify two differently interpreted uses
of the same image do not alias incompatible cooked data. Compare full reference
graph/geometry/material values, with explicit bounds for any enabled lossy step.
Test explicit geometry edits and animation against shared static owners, including
copy-on-write isolation between repeated model instances. Cover index rebasing across
models, models with different `vertexTexCoordSets` including a non-prefix list such
as sets 0 and 2 beside sets 0 and 1, and a primitive with no material beside a model
that has materials.

In ZenEngine, configure integration builds explicitly with
`-DZEN_ASSET_PROCESSOR_SOURCE_DIR=E:/Dev/ZenAssetProcessor`; the general Release
build currently can use a fetched processor checkout. Record both revisions.
Follow project C++ rules, use engine containers where suitable, and format changed
C++ with each project's `.clang-format`.

Runtime performance must use the actual standalone ZenApp and explicit
`--setup=<file.zenrender>` plus startup overrides. Retain actual framebuffer/render
extents, fullscreen state, backend and async-compute state. Do not edit
`Data/engine.cfg`. Use `tools/profile_hybrid_gi.py` for any Nsight GPU Trace follow-up.
Close the editor and close RTSS or exclude ZenApp from it before timing.
Use fixed views showing curtains, ivy, trees and candles, plus the courtyard camera
from the existing ZenApp capture report for a direct comparison with main alone.
Compare blended leaves, normal maps and metallic/roughness materials as well as counts.

## Renderer scale risks

Loading the scene does not establish that the renderer can use it. These risks come
from reading the current code; none has been measured at this scale. M0 measures each
one per model, before composition depends on the answer.

| Risk | Current path | Required response |
| --- | --- | --- |
| Blended tree leaves | [`BuildForwardGraph`](../../ZenEngine/ZenCore/Source/Graphics/RenderCore/V2/DeferredLightingRenderer.cpp) makes one sort entry per `BLEND` triangle every frame and stable-sorts them on the render thread, then rebuilds a sorted index copy, compares it, and uploads it when the order changes. It was tuned for 8,140 decal triangles. `LeafSpring` adds 1,354,570: about 49 MB of entries and 16 MB of indices per frame, plus forward lighting with overdraw | Measure the leaf texture's alpha distribution in M0. If coverage is close to binary, use an explicit, recorded alpha-mask policy for this material; otherwise sort coarser groups above a triangle threshold. Do not add trees to the combined test until one is chosen |
| Single candle mesh | All 19.54 million candle triangles belong to one node and three submeshes, so per-submesh culling cannot reject an individual candle, and every raster pass that draws the mesh submits all of them | Measure candles alone in ZenApp. If raster cost dominates, re-instancing or cluster culling becomes a renderer requirement rather than a memory option |
| Acceleration structures | [`SceneRayQuery`](../../ZenEngine/ZenCore/Source/Graphics/RenderCore/V2/SceneRayQuery.cpp) builds one BLAS per mesh with `PREFER_FAST_TRACE` and `ALLOW_UPDATE` and no compaction. `accelerationStructureBudgetMB` defaults to 0, which disables its size check, and shared scratch must fit 32 bits. Candles are one 19.54-million-triangle BLAS | Log `GetAccelerationStructureBuildSizes` per mesh in M0, which gives exact sizes without a build. If they do not fit, evaluate disabling `allowUpdate` and adding compaction for static meshes |
| Distance field | [`DistanceFieldScene`](../../ZenEngine/ZenCore/Source/Graphics/RenderCore/V2/DistanceFieldScene.cpp) derives capacities from triangle sums and scales them to half of free memory. Main used a 98 MB snapshot and 0.89 GiB of scratch at 3.75 million triangles. Blended triangles also take coverage slots, and the memory-limited path has not run at this scale | Record generations, batches, retries, coverage-budget failures and memory-limited status. Keep sizing derived from the scene and device, with no scene-tuned constants |

## Lighting scope that must remain explicit

Preserve all 10,024 light instances in processor/native data. The present engine
accepts only 32 scene lights and warns while dropping excess additions. That is
not a valid full-lighting acceptance result. Add preflight reporting so the
limitation is visible before expensive load/preparation.

The asset feature can validate all geometry/materials together using an explicitly
recorded supported lighting setup while retaining every authored light in native
data. Full authored candle lighting requires a separate many-light renderer
design: clustered/tiled light selection and bounded shadow/GI work, with its own
quality and performance gates. Raising the constant alone is insufficient. Do not
silently disable lights or describe the supported-lighting geometry test as full
authored-lighting parity.

Apply any explicitly selected supported light set before constructing RenderScene
and publishing GPU resources. Today the constructor adds imported lights before
render settings are applied, so merely supplying a later setup override still
produces overflow warnings and dropped light IDs. Retain all native/CPU light
nodes, and reject an over-capacity rendering request without a declared selection
policy instead of silently accepting it. The first policy is per instance: the
assembly states whether an instance submits its lights to rendering. Main submits
its 24 and candles submit none, which fits the 32-light limit without depending on
light order, and the recorded setup states that candle lighting is excluded.

## Open decisions

Each item lists a recommended default. Items 1 and 2 are needed before M3 can finish.

1. Frame-time gate. Without one, M3 can pass at an unusable frame rate. Recommended:
   adopt the provisional 33.3 ms HWRT target and revise it from M0's per-model runs.
2. Tree-leaf blending. Recommended: use an explicit alpha-mask policy if M0 shows
   near-binary coverage, because it removes per-frame sorting and most forward cost.
   This changes the image, so record it and compare against reference. If coverage
   is not close to binary, change the sort instead.
3. Candle re-instancing. Recommended: keep authored geometry through M3, because the
   model is the stress case, and re-instance only if M3 misses a gate.
4. Reuse identity. Recommended: for assembly builds, key reuse on explicit importer,
   cook and schema versions plus settings, retain the source fingerprint as provenance,
   and add a fixture output-hash test that fails when a version bump is missed. The
   alternative keeps today's guarantee and accepts a full recook after every
   processor commit.

## External designs consulted

- [NVIDIA Donut](https://github.com/NVIDIA-RTX/Donut/blob/main/README.md) supports
  scene-layout JSON that combines multiple glTF models. Its
  [scene loader](https://github.com/NVIDIA-RTX/Donut/blob/main/src/engine/Scene.cpp)
  separates model loading and graph assembly. Adopt this separation while using
  Zen's cooked runtime format rather than requiring source import at launch.
- [Unity Addressables bundle memory guidance](https://docs.unity3d.com/Packages/com.unity.addressables@2.7/manual/memory-assetbundles.html)
  describes the tradeoff between bundle count, overhead, unload granularity and
  dependency breadth. Its [dependency documentation](https://docs.unity3d.com/Packages/com.unity.addressables@2.7/manual/AssetDependencies.html)
  explains how implicit shared assets can be duplicated. Apply these lessons to
  explicit shared-resource identities and measured package granularity.
- [OpenUSD load/unload and payloads](https://openusd.org/release/glossary.html#load-unload)
  separates scene composition from optional payload loading to control working
  sets. Use that principle for future resource residency; adopting USD itself or
  its complete composition system is unnecessary for this feature.

The proposed architecture is an inference from these patterns and the inspected
Zen code. None of these external sources establishes ZenEngine performance.
