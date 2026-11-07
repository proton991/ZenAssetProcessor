# Multi-model processor verification

Checkpoint: 7 November 2026, Windows x64 Release. The processor now builds the
five Sponza exports into independent native model packages and one small native
assembly. This checkpoint implements the processor portion of
[the refined plan](MultiModelScenePlan.md); it does not close engine composition,
GPU memory, rendering or complete M5 acceptance. No ZenEngine files were changed
or engine executables run for this checkpoint.

## Implemented behavior

- Version 2 build manifests describe models, repeated instances, affine placements,
  per-instance light submission and explicit camera/environment selection.
  Version 1 batch builds retain their independent-job publication behavior.
- `.zenscene` uses the existing v3 package container and references immutable
  `.zenasset` files by relative path, file size and header identity. Metadata and
  dependency reading work after relocation and without original sources.
- Changed models import once and execute sequentially. The builder consumes
  exclusive imported ownership, writes and fully validates each native model,
  releases it, then publishes the assembly last. A failure preserves the prior
  usable assembly. Unchanged jobs use content hashing, stored-payload checksums
  and a semantic-validation receipt without decoding all model payloads.
- Explicit assembly-only builds change placement without accessing sources or
  scanning model payloads and report `source_freshness_checked: false`.
- Image schema 3 stores mip zero once. Readers accept earlier image schemas,
  check their duplicated base against mip zero and retain one copy. Public
  immutable `CookScene` behavior remains unchanged. Consumers should use
  `GetImageBaseBytes` when accessing images with explicit mips.
- Explicit dry-run-first cleanup traces every retained assembly in its root,
  validates dependencies and removes only eligible unreferenced native models.

See [build usage and constraints](AssemblyBuilds.md) and
[the native contract](SceneAssemblies.md).

## Correctness evidence

The final processor/CLI Release build passed **43/43 CTests** in 17.28 seconds.
Coverage includes composition and transforms, bounded decoding/traversal,
relocation, source-free reads, corruption and unsupported required schemas,
incremental source changes, import-once behavior, cancellation and failed
publication, previous-generation preservation, cleanup safety, exclusive/shared
cook ownership, and old/new image schema compatibility. Malformed UTF-8 and
embedded-NUL public API output paths are rejected before import/publication.

A separate `ZENASSET_BUILD_PROCESSOR=OFF` build passed **4/4 CTests**. An installed
Reader-only consumer opened relocated assembly dependencies and an Image v3
scene. Its link/compile inventory contains no source importer or Processor target.
The boundary check was repeated against the final sources and matches the final
production fingerprint below; source and installed-artifact hashes are retained.

Full native validation of the Sponza assemblies reports:

| Models / instances | Vertices | Triangles | Nodes | Authored light nodes |
| ---: | ---: | ---: | ---: | ---: |
| 5 / 5 | 21,017,819 | 32,373,689 | 10,181 | 10,024 |

This includes main, curtains, nested ivy, trees and the candle export. Only the
654,755,780-byte candle glTF/BIN pair was extracted from the archive, into this
repository's ignored `build/` tree. The original corpus was not modified. All
10,000 candle lights remain serialized; the generated assembly records their
submission policy as false. No rendered lighting acceptance is claimed.

## Measurement method

Host: Ryzen 9 9950X3D (16 cores / 32 threads), 31.11 GiB usable RAM, NTFS `E:`.
The RTX 5080 was not used by these processor measurements. Runs use serial fresh
processes with warm/uncontrolled filesystem caches. No cold-storage result or
engine first-frame time is inferred from these numbers.

The final compiler is frozen in each campaign, SHA-256
`999fe31c4028927e88aa66f23cdb86e5f982d272f0ccd4c9ae455eef0eeffca5`.
Its production build fingerprint is
`a03b1718e141854b7ece342c35bd2a3fb9357a3d8809aeecbb88196f2bb78a31`.
Exact arguments, executable/manifest/output hashes, timestamps, memory peaks and
logs are retained beside each campaign's `results.json`. Source identities cover
97 input files. See [reproduction commands](../tools/validation/README.md#multi-model-sponza-workflow).

Builds use two intra-model workers, `--job-memory 51539607552` and
`--memory-budget 51539607552`. These **48 GiB conservative stage reservations are
not measured RAM usage**. Existing stage allowance partitioning and cumulative
image admissions require this headroom for the full-resolution main scene.
The external monitor aborts at 16 GiB observed private commit. Native validation
uses `--allocation-budget 17179869184`. Process peaks include allocations outside
the stage accounting. This is an observation/abort mechanism, not an OS-enforced
memory limit.

Both profiles use Zstandard level 3 and meshoptimizer. Reference preserves
imported image data. Desktop explicitly enables generated mips, BC7, BC5 normals
and eligible BC4 occlusion, at authored 4K resolution. The full cook selects BC7
quality 0 after a 4K color preflight measured quality 0..4 at 11.101, 12.020,
14.559, 14.524 and 16.619 seconds respectively. Existing numerical quality and
alpha requirements were not relaxed. Quality 4 remains the generator's default.

## Full-corpus results

| Measure | Lossless image reference | Desktop quality 0 |
| --- | ---: | ---: |
| Fresh full cook, one final-binary sample | 68.635 s | 619.102 s (10 min 19 s) |
| Unchanged build, median of five | 0.988 s | 0.905 s |
| Unchanged build range | 0.988–1.050 s | 0.904–0.905 s |
| Full sequential native validation, median of five | 12.222 s | 8.981 s |
| Native validation range | 10.238–13.130 s | 8.842–9.042 s |
| Fresh cook peak private commit | 6.98 GiB | 6.98 GiB |
| Fresh cook peak working set | 6.91 GiB | 6.91 GiB |
| Native validation peak private commit | 4.93 GiB | 3.89 GiB |
| Current generation, five models plus assembly | 3,149,924,952 bytes (2.934 GiB) | 2,817,450,392 bytes (2.624 GiB) |
| Assembly alone | 2,601 bytes | 2,601 bytes |

Every final-campaign sample succeeded. Desktop's observed fresh cook, unchanged
build, process memory and current-generation disk size meet the respective
provisional processor targets. Native validation decodes and checks models one at
a time; it does not measure simultaneous residency or an engine load to frame.

All final-binary durations in seconds are retained here as well as in JSON:

- Reference reuse: 0.988376, 1.008033, 0.987984, 0.988138, 1.049539.
- Desktop reuse: 0.904949, 0.903656, 0.904789, 0.904610, 0.903546.
- Reference native validation: 12.222234, 12.041498, 13.130225, 12.915088, 10.238124.
- Desktop native validation: 9.042083, 8.917072, 8.981461, 8.842228, 8.988686.

A real desktop placement edit translated the main instance by five source units
while every job source pointed at a nonexistent path. Assembly-only publication
completed in **0.025530 seconds including process startup**, with unchanged model
identities, hashes and modification times. The original placement was then
restored; the assembly's bytes matched the original exactly. The normal unchanged
assembly-only samples were 0.024993 seconds (reference) and 0.025193 seconds
(desktop). These short process timings have the monitor's approximately 20 ms
sampling granularity.

After dry-run inspection, explicit cleanup removed ten obsolete reference models
(6,299,844,702 bytes) and five obsolete desktop models (2,817,447,791 bytes).
Each cooked directory now contains exactly its five referenced packages and one
assembly, with total logical bytes equal to the table above. The combined removed
logical size is 8.49 GiB. Both full assemblies passed native validation again after
cleanup. Retained earlier generations had raised the directory snapshots to
9,449,769,654 and 5,634,898,183 bytes respectively; these snapshots are not claimed
as exact transient peaks.

Earlier successful desktop repetitions were **613.480 and 601.889 seconds**,
using compiler SHA-256
`a2baf0cf1b0129ce58760103f7479dce340fc4eeef62d35142b89b85b5715eb4`.
The intervening production change validates the public assembly output path.
These repetitions corroborate throughput but are kept separate from the final
compiler cohort; no same-build median combines them.

A third earlier desktop repetition failed its timeout after the machine slept
and hibernated. Its recorded elapsed time was 4,178.196 seconds. Windows power
events show sleep at 00:12:36, hibernation at 01:12:40 and resume at 01:12:48 KST.
The failed sample is retained and excluded from successful timing statistics.
The previously published full assembly still passed native validation afterward.
Only the abandoned build's verified staging files were removed.

The pre-change individual reference cooks totalled 48.22 seconds, with main
peaking at 6.98 GiB private commit. They did not perform the new assembly's
one-time semantic read and transaction work, so this is historical context, not
a matched throughput speedup comparison.

## Texture storage and remaining limits

The desktop output contains **31 BC7 images** (28 sRGB, three linear), each with
13 explicit mip levels, and **56 RGBA8 images** retaining their original base-only
chain. No BC5 or BC4 image passed this corpus's eligibility/quality checks. Owned
image payloads total **4,451,555,472 bytes (4.15 GiB)** before package transport
compression. The source reference has 87 4K RGBA images (5.438 GiB base pixels).
Image schema 3 eliminates duplicated mip-zero storage where explicit mips exist;
base-only images remain valid schema 1 records.

Diagnostics include undefined positive-Z normal directions, reconstructed normal
mip angular limits, BC7 scalar-channel error, and exact-alpha preservation of the
original chain. These are recorded fallbacks, not complete BC/mip coverage. The
tree alpha histogram has 39,801 intermediate-alpha pixels out of 16,777,216
(0.2372%); material alpha policy was not changed. Engine texture ownership,
GPU residency, leaf blending and candle/AS/GI scale remain unverified.

The following plan items remain explicit limits of this checkpoint:

- The conservative complete processor source fingerprint still invalidates cache
  reuse after source changes. Explicit importer/cook-version compatibility has
  not replaced it. Changed builds rehash sources before publication to detect
  mutation, rather than promising one hash pass across the entire transaction.
- The 1.2x matched individual desktop cook ratio, raw/Zstd/meshoptimizer/combined
  codec matrix on this corpus, per-stage CPU/I/O/allocation metrics and full-scale
  one-texture edit timings are unmeasured. Small fixtures cover incremental
  isolation and same-size/time content changes.
- These campaigns recorded directory snapshots, not exact transient peaks. The
  harness now supports one-second logical-file sampling for future campaigns;
  that still cannot establish an exact physical-disk maximum. There is no new
  free-space admission reservation; write failures return status and preserve the
  previous assembly. The strict temporary-disk gate is not claimed as verified.
- New assembly tests do not establish every mixed-importer, animated/skinned/morph
  combination or a valid package with payload offsets beyond 4 GiB. Existing
  single-model compatibility and integer-overflow tests cover narrower cases.
- No engine integration, simultaneous runtime residency, GPU uploads, rendering
  comparison, first-frame/AS/GI readiness or runtime performance acceptance was
  attempted. Conditional finer resource blocks and candle re-instancing remain
  decisions for measured runtime limits, rather than consequences of file size.

## Local evidence

The following generated evidence is retained under ignored `build/` paths on the
test machine; source tools and this report are tracked repository content:

- `multi-model-investigation-20261106/`: inventories, 97-file input identities,
  manifests, final build/43-test logs, Reader boundary/4-test logs, texture storage,
  leaf alpha histogram, power events, interrupted-stage cleanup record,
  `final-summary.json`, `placement-20261107/`, cleanup dry-run/applied records,
  post-cleanup validation and the final `*-storage-20261107.json` audits.
- `multi-model-reference-20261107/` and `multi-model-desktop-20261107/`: final
  compiler campaigns and per-process logs.
- `multi-model-reference-final/` and `multi-model-desktop-final/`: predecessor
  compiler repetitions, including the failed interrupted sample.
- `multi-model-baseline-20261106/` and `multi-model-texture-preflight-20261106/`:
  original per-model measurements, selective candle inputs and 4K encoder trials.
- `multi-model-assembly-reference/sponza.zenscene` and
  `multi-model-assembly-desktop/sponza.zenscene`: current native assemblies and
  their sibling `models/` directories.
