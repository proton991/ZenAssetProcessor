# Import geometry admission verification

This report records the standalone processor's glTF/FBX geometry admission
contract and its focused verification. It covers processor-owned requested
storage; it is not a total-memory or RSS guarantee. Engine code and builds are
outside this verification.

## Enforced contract

[SceneImportContext::maxGeometryBytes](../include/ZenAsset/Processor/Import.h)
is a trailing public context field with a **16 GiB** default for individual
imports. glTF and FBX share one import-scoped, thread-safe allowance. A request
must reserve its requested element storage before a covered allocation or
capacity increase. Checked count/stride arithmetic rejects overflow, impossible
container sizes and insufficient allowance as `ResourceLimit`; failed imports
publish no scene. Zero allowance admits no positive geometry reservation.

The [private reservation helper](../src/Processor/GeometryBudget.h) tracks
current reservations and their historical peak. Its peak is declared admission,
not measured process memory. [ImportErrors](../src/Importers/ImportErrors.h)
retains persistent charges through conversion and post-import normalization.
Scoped temporary reservations refund after their buffers and workers finish.
Capacity-growth charges conservatively include the previous and replacement
requests; previous charges can remain until the owning scope ends. Consequently
the required allowance may exceed both final payload size and peak live buffer
storage. Nested arrays moved into the scene retain their persistent charge;
worker-result outer buffers and append destinations are charged separately while
they coexist.

| Covered path | Admitted storage |
| --- | --- |
| glTF conversion | Accessor-converted canonical geometry, index/topology expansion, flat-corner expansion, texture coordinates, all influences, custom attributes, skin matrices, morph targets/defaults and animation buffers. Meshopt decoded buffers and processor-owned Draco conversion buffers are charged before allocation. |
| FBX conversion | Expanded vertices/indices, UV/color attributes, source control-point maps, triangle/corner/UV lookup scratch, all influences, morph positions/normals/weights, skin joints/matrices, adaptive animation frames/interpolation/time buffers and final animation output. |
| Concurrent assembly | All in-flight primitive/mesh results share the allowance with canonical destination capacity. Queued mesh work drains on error/cancellation before its reader and reservations are destroyed. |
| Shared normalization | Morph-target count scratch and replacement playable-animation headers are reserved while imported scene data remains charged. |

The scope excludes parser/codec-private storage (including ufbx parsed and
evaluated trees), allocator bookkeeping, general metadata/diagnostics and
graph-validation scratch. Image data retains `maxDecodedImageBytes`; source
bytes retain their existing backend-specific policy. This change does not make
external glTF/FBX source files a cumulative `maxSourceBytes` allowance. FBX ufbx
load/evaluation temporary and result allocators retain their separate fixed
8 GiB limits. Canonical validation also retains its separate 16 GiB runtime
geometry limit, so increasing import admission does not remove final validation
limits. OBJ keeps `maxObjGeometryBytes` and its existing policy.

The CLI cooking option `--geometry-budget` sets the common glTF/FBX allowance.
Batch imports default to **256 MiB** for each geometry allowance.
[SceneBuildReservationBytes](../src/Processor/Build.cpp) reserves the larger of
common geometry and OBJ geometry once per job, alongside the other job budgets.
Only one source backend executes per job. See [BatchBuilds.md](BatchBuilds.md)
for the complete reservation formula and aggregate-initialization behavior.

## Focused verification

The final focused **Release** run passed **4/4 CTests in 0.32 seconds** on the
Windows x64 MSVC build. The verbose source of these results is
[`build/import-geometry-unit/focused-tests.log`](../build/import-geometry-unit/focused-tests.log);
the corresponding combined build is
[`focused-build.log`](../build/import-geometry-unit/focused-build.log).
Earlier failed test-build iterations are not reported as passing verification.

| Test | Verified behavior |
| --- | --- |
| [ZenAssetGeometryBudgetTests](../tests/GeometryBudgetTests.cpp) | Exact quota admission, zero and overflowing requests, move/reset ownership, concurrent competing reservations, shared-owner charges and complete refunds. |
| [ZenAssetGltfGeometryBudgetTests](../tests/GltfGeometryBudgetTests.cpp) | **93 checks** across eight generated real-byte fixtures: exact public admission boundary and one-byte shortage, complete scene equality across generous/exact budgets, topology/flat-normal expansion, cumulative rich primitives, real meshopt/Draco conversion, eight influences, skin/morph/custom/animation retention, cancellation on workers, batch allowance selection and failed-publication preservation. |
| [ZenAssetFbxGeometryBudgetTests](../tests/FbxGeometryBudgetTests.cpp) | Four owned fixtures at exact serial declared peak, peak minus one and zero; full public-scene equality using the shared independent comparison helper; public quota propagation and subsequent recovery; generated 64-mesh cumulative growth; cancellation on an active mesh worker and during animation baking; zero outstanding reservations and reusable worker pool after return. |
| ZenAssetBuildTests | Existing **57 build checks** continue to pass with the added common geometry reservation. |

The glTF acceptance boundaries observed in this build were:

| Fixture | Workers | Requested admission bytes |
| --- | ---: | ---: |
| Base triangles | 1 | 1,540 |
| Missing normals / expanded corners | 1 | 2,572 |
| Triangle strip | 1 | 1,556 |
| Line loop | 1 | 1,564 |
| Rich skin/morph/custom/animation | 1 | 5,164 |
| 32 rich primitives sharing source accessors | 4 | 163,376 |
| Actual meshopt positions | 1 | 1,588 |
| Actual Draco geometry | 1 | 1,660 |

The compressed fixtures use the actual encoders, then import their encoded
bytes. The exact extra admission versus base is **48 bytes** for meshopt decoded
positions and **120 bytes** for Draco-converted positions, normals and indices;
these are asserted in the tests, not inferred merely from compressed file size.
The batch test preserves the previous published package byte-for-byte when a
later import fails its geometry allowance, and separately rejects a one-byte
aggregate reservation shortage before dispatch.

The serial FBX declared peaks observed in this build were:

| Owned fixture | Declared peak bytes |
| --- | ---: |
| `maya_cube_7500_ascii.fbx` | 7,885 |
| `maya_uv_and_color_sets_7500_ascii.fbx` | 9,552 |
| `combined_morph_skin_ascii.fbx` | 23,829 |
| `maya_anim_interpolation_7700_ascii.fbx` | 43,649 |

Each exact peak succeeds and peak minus one fails with a `maxGeometryBytes`
diagnostic and no asset. The combined fixture additionally confirms all six
influences survive alongside skin, morph and baked animation data. The parallel
cancellation callback requires a worker-thread poll after charged work, so it
cannot pass solely by cancelling destination preflight. Reservation counts must
be zero before the import returns; these tests do not equate that count with
physical memory release by the allocator.

These byte values depend on C++ type sizes, build configuration and the current
conservative accounting policy. They are verification observations, not stable
serialized-format constants or recommended per-asset budgets. Full-scene
comparisons test semantic preservation between allowance settings; they are not
an independent proof of every source format feature. General format scope is
specified in [GltfImport.md](GltfImport.md) and [FbxImport.md](FbxImport.md).

## Reproduction

Build the four targets in an already configured standalone processor tree:

```powershell
cmake --build build/packaging-release --target ZenAssetGeometryBudgetTests ZenAssetGltfGeometryBudgetTests ZenAssetFbxGeometryBudgetTests ZenAssetBuildTests
ctest --test-dir build/packaging-release -R '^ZenAsset(GeometryBudgetTests|GltfGeometryBudgetTests|FbxGeometryBudgetTests|BuildTests)$' --output-on-failure -V
```

The FBX test uses the processor-owned corpus configured through
`ZENASSET_TEST_CORPUS_DIR`. Generated test fixtures are temporary and cleaned up.
Run configuration suites sequentially; the FBX test's temporary directory is
shared by its Debug and Release invocations.

## Combined import checkpoint

The subsequent production checkpoint is
`d3f5f8ff99dc077a26940d00ef7f69fed0a0eadfcd02a4aee9d38d3f32b6869b`.
It includes the import limits and evaluation-only timing scopes. Its 140 hashed
production inputs and saved sources are recorded in
[source-identity.json](../build/import-budget-verification/source-identity.json).

The ordinary standalone suite passed **40/40 Release tests in 20.60 seconds**
and **40/40 Debug tests in 442.91 seconds**, with no skipped tests. The build/test
commands and logs are in
[ordinary-commands.json](../build/import-budget-verification/ordinary-commands.json),
with parsed results in
[full-suite-results.json](../build/import-budget-verification/full-suite-results.json).
Both configurations used the processor-owned fixture directory.

The raw package corpus accepted all **360 distinct scenes** and checked full
public-scene equality after reading each package, followed by deterministic
byte-identical rewriting. The original list passed 341 entries and reported
19 missing FBX files because those fixtures had moved out of ZenEngine. All
19 then passed from their processor-owned paths; the initial failures remain
in the evidence. See
[corpus results and input hashes](../build/import-budget-verification/scene-corpus-results.json)
and [path migration](../build/import-budget-verification/corpus-path-migration.json).
This is source-import and lossless-package evidence, not a lossy HDR encoding
or rendered-output comparison. Installed SDK consumption is reported separately.

