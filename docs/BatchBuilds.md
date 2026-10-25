# Standalone batch builds

`zenassetc build assets.json` uses the existing `ImportScene`, `CookScene` and
`WriteScene` implementations. Public APIs are `ReadSceneBuildManifest` and
synchronous `BuildSceneBatch` in `ZenAsset/Processor/Build.h`. Reader-only packages
acquire no batch/parser dependency. See [CMake integration](BuildIntegration.md).

```json
{
  "version": 1,
  "outputDirectory": "cooked",
  "jobs": [{
    "source": "models/scene.obj",
    "output": "models/scene.zenasset",
    "profile": "desktop",
    "sourceSettings": { "sceneIndex": 4294967295 },
    "processing": { "geometry": { "optimizeVertexFetch": true } },
    "compression": { "transport": "zstd", "zstandardLevel": 3, "meshoptimizer": true }
  }]
}
```

Version 1 requires `version`, `outputDirectory`, nonempty `jobs`, and each job's
`source` and `output`. Other fields use public API defaults. Relative source paths
and output directories use the manifest's parent. Outputs must be relative
`.zenasset` paths without `.`/`..`, rooted inside the resolved output directory.
Unknown/duplicate properties, wrong JSON types and unsupported settings fail.

Optional names match the public records exactly:

- `sourceSettings`: `sceneIndex`, `fallbackMissingNamedUVSets`, `animationBake`.
  The bake object accepts `sampleRate`, `translationTolerance`, `rotationTolerance`,
  `scaleTolerance`, `valueTolerance`, `maxSubdivisions`, `maxValues`.
- `processing.geometry`: `optimizeVertexFetch`, `positionQuantizationBits`,
  `positionMaxRelativeError`, `positionMaxAbsoluteError`.
- `processing.textures`: `generateMipmaps`, `compressBC7`, `bc7Quality`,
  `maxDimension`, `colorMinPsnr`, `colorMaxRmse`, `scalarMaxError`,
  `normalRmsAngleDegrees`, `normalMaxAngleDegrees`, `alphaCoverageMaxError`,
  `compressBC5Normals`, `compressBC4Occlusion`, `compressBC6HEnvironment`,
  `hdrMaxRelativeRmse`, `arrayPages`, `arrayMaxDimension`, `arrayMaxLayers`,
  `arrayMaxPages`, `arrayMaxImages`, `spatialPages`, `spatialMaxDimension`,
  `spatialMaxPages`, `spatialMaxImages`. See [array cooking](ArrayCooking.md)
  and [spatial cooking](SpatialCooking.md); the two modes are mutually exclusive.
- `compression`: `transport` (`none`/`zstd`), `zstandardLevel` (1–19),
  `meshoptimizer` (boolean).

CLI options are `--output-dir DIR` (relative to the current directory),
`--jobs 1..64`, `--memory-budget BYTES`, `--job-memory BYTES`,
`--manifest-budget BYTES`, and `--force on|off`. Unknown/repeated options fail.
JSON results stay in request order and report status, `built`/`reused`/`not_built`,
cache identity, publication state and omitted-diagnostic count. The overall error
is the first failed job in request order after workers drain.

## Freshness and publication

Identity covers every source/processing/compression setting (including disabled
values), profile, package schema, processor/importer versions and source-build
fingerprint. Inputs use existing XXH64 cache fingerprints, not security hashes.
Machine paths and import-time stamps are provenance; runtime scene semantics
never consult those files or depend on their times.

Reuse requires full `ReadScene` resource/graph validation and input content checks.
Unchanged touches can hit; same-size/time content changes invalidate. Missing
preferred paths are retained so their appearance invalidates fallback resolution.
Corrupt outputs or different identities recook; `--force on` always recooks.

Preflight discovers every dependency before any output can be replaced and rejects
output aliases against all sources, dependencies and other outputs, including
existing hardlinks/symlinks. Changed jobs import once during serial preflight,
release the scene, then import again during execution and compare complete input
identity. This deliberate extra import avoids retaining all scenes or introducing
a second staged writer. Unchanged jobs use validated package dependencies.

Import-time stamps are preserved. Hashes are added only while that imported
revision remains present. After the complete temporary package is flushed and
closed, the writer's callback rechecks content, missing paths and output resolution
immediately before atomic replacement. Prepublication failures/cancellation keep
prior bytes. A subsequent durability error can leave a complete new output;
`published: true` distinguishes this. Cancellation after commitment cannot undo it.
Publication is atomic per job, not a transaction over the batch: successful jobs
remain committed if another executing job fails. Preflight failure publishes none.

Same-process batch calls serialize through a cancellable gate. Callbacks can run
on workers; `Waiting` uses the global index `UINT32_MAX`. Callbacks must be
thread-safe and must not recursively invoke a batch. Importer workers are divided
across admitted jobs; synchronous return drains all workers. Independent processes
and non-batch writers require external coordination. Atomic replacement is not a
lock, and sequential content checks are not a filesystem snapshot.

## Bounds

Defaults: 1 MiB manifest, 1,024 jobs, 64 MiB planning/results, 4 MiB metadata and
128 returned diagnostics per job, 4 GiB aggregate job reservations. Planning may
admit fewer than the job-count limit when metadata is large. Omitted diagnostics
are counted. A failed job never returns a partial scene.

`SceneBuildReservationBytes` sums source and decoded-image limits, the larger
of `maxGeometryBytes` (glTF/FBX) and `maxObjGeometryBytes`, two scene allocation
budgets, 4 MiB hash scratch and three metadata budgets. One source backend runs
per job, so the two geometry allowances share one reservation slot. Workers are
admitted by this reservation, requested concurrency and CPU count. Planning and
results have a separate bound.

The glTF/FBX geometry allowance covers conservative requested capacities for
geometry, deformation, animation and conversion scratch before allocation,
including concurrent worker-result and destination storage. Previous capacity
growth charges may remain reserved until the owning import/scratch scope ends.
These reservations are **not an RSS guarantee**: parser/codec-private memory,
allocator bookkeeping, general scene metadata and graph-validation scratch have
separate accounting or remain outside the geometry contract.

Batch import defaults are 128 MiB source, 256 MiB decoded images, and 256 MiB
for each geometry allowance. Both scene allocation budgets default to 256 MiB.
These per-stage defaults are intentionally smaller than individual import limits.
When replacing `importContext` with an aggregate, set its fifth member
`maxGeometryBytes` explicitly to retain the desired batch reservation; an omitted
member takes the standalone 16 GiB default.

Path resolution reserves bounded planning scratch before copying/resolving paths.
Preflight dependency discovery temporarily uses the admitted job's metadata limit;
retaining its dependency list is then charged to the separate planning budget.

`--job-memory` accepts 64 MiB–1 TiB. After 16 MiB hash/metadata overhead, it assigns
one ninth to source bytes and two ninths each to decoded images, the geometry
slot (both backend allowances receive that limit), and each of the two scene
allocation budgets. Encoder scratch is also capped inside
the writer budget. Large assets require explicit higher limits.

## Verification snapshot

The initial Release snapshot passed `ZenAssetBuildTests` (57 checks),
`ZenAssetBuildCommandLine`, `ZenAssetCompressionWriterTests`, and the existing
`ZenAssetCommandLine` test. Coverage includes wrong-shaped/unknown/duplicate JSON,
full package validation before reuse, force, source and dependency edits with
unchanged size/time, missing preferred inputs, hardlink aliases, settings and
build identity, budget overflow, cancellation and prepublication preservation.
The build and test logs are `build/processor-port/m7-build-compile-final.log` and
`m7-build-tests-final.log`; installed consumer verification is recorded separately.

The subsequent full Release suite passed 29/29 (`m7-full-tests.log`). After making
the CLI finish its result line before emitting diagnostics, the batch API and both
CLI suites passed 3/3 (`m7-report-tests.log`); merged stdout/stderr is covered.
Installed Compiler and Reader-only CMake consumers, source-free loading and actual
batch/editor canonical equality are recorded in ZenEngine's
`Doc/ZenAssetProcessorM7BatchVerification.md` and `ZenAssetProcessorM7EditorVerification.md`.

A small local timing sample used `complete_scene.glb` (5,352 bytes) and
`blender_279_sausage_7400_binary.fbx` (101,980 bytes). Three fresh-output repetitions
gave medians of 26.64 ms for two serial individual cooks and 30.53 ms for a two-job
batch with serial preflight plus execution imports. Five unchanged batch runs had
a median of 11.45 ms including full package validation. These include process
startup, use warm filesystem state, and do not establish large-asset throughput
or RSS behavior. The frozen compiler SHA256 is
`50003ab02ae43ef181bc710855f1e60a940ec602d3407e267a5658471508c485`;
`build/m7-batch-verification/timing.json` retains inputs, repetitions, build identity
and methodology. Later integration builds have their own recorded fingerprints.
