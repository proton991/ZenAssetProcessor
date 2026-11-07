# Native multi-model assemblies

`zenassetc build` accepts a version 2 manifest that produces one `.zenscene`
assembly and independently cooked `.zenasset` models. An assembly uses the same
package container as a scene, with one required Assembly resource. It stores no
source mesh or texture payloads and does not flatten models into one giant scene.
Version 1 independent batch manifests keep their existing behavior.

```json
{
  "version": 2,
  "name": "Courtyard",
  "outputDirectory": "cooked",
  "assembly": "courtyard.zenscene",
  "jobs": [
    {
      "key": "building",
      "source": "models/building.gltf",
      "profile": "desktop",
      "processing": {
        "textures": {
          "generateMipmaps": true,
          "compressBC7": true,
          "compressBC5Normals": true,
          "compressBC4Occlusion": true
        }
      },
      "compression": {"transport": "zstd", "zstandardLevel": 3, "meshoptimizer": true}
    },
    {"key": "props", "source": "models/props.obj"}
  ],
  "instances": [
    {"key": "building-instance", "model": "building", "submitLights": true},
    {
      "key": "props-instance",
      "model": "props",
      "transform": [1,0,0,0, 0,1,0,0, 0,0,1,0, 2,0,0,1],
      "submitLights": false
    }
  ]
}
```

Each job uses the [existing batch processing settings](BatchBuilds.md), replacing
`output` with `key`. Keys use ASCII letters, digits, underscores or dashes, with
at most 128 characters. Package filenames are generated from the model key,
settings/input identity and package header identity. The assembly output is a
`.zenscene` basename in `outputDirectory`. Relative source/output paths resolve
from the authoring manifest's parent. Unknown/repeated fields are rejected.

Every instance references a job key. Transform values are 16 finite column-major
floats; omission means identity. The native graph preserves source coordinates
and hierarchy. `submitLights` defaults to true, and records rendering policy
without deleting source lights. Optional `camera` and `environment` objects use
`{"instance":"instance-key","resource":0}`; omission means no selected resource.
Bounds and counts are computed by the processor. Model-local identifiers remain
local; consumers must namespace or remap them when constructing a combined scene.

## Builds and reuse

```sh
zenassetc build courtyard.json --import-workers 2
zenassetc inspect cooked/courtyard.zenscene
zenassetc validate cooked/courtyard.zenscene
```

Assembly builds admit one large model job at a time. `--import-workers 1..64`
sets intra-model importer concurrency; `--jobs` continues to control version 1
batch concurrency and does not make assembly jobs run concurrently. Existing
`--job-memory`, `--memory-budget`, `--manifest-budget`, `--force`, and
`--output-dir` options apply. These are conservative allocation reservations, not
process-RSS ceilings. Large scenes require explicit higher per-stage admission.

Changed models import once, cook, serialize into an exclusive staging directory,
release their cooked owner, then undergo one full native semantic validation.
Only summaries and dependencies remain while the next model is built. Newly
validated packages become immutable files under `models/`; the assembly is
atomically replaced last. Cancellation or a failed model keeps the previous
assembly intact. Valid old model files remain untouched. Incomplete builds may
leave unreferenced immutable files; explicit cleanup below removes only eligible
files unreferenced by all retained assemblies within its selected root.

Unchanged models read only native metadata, stream stored-payload checksums, and
hash source dependencies. A semantic-validation receipt binds the native package
identity to the validator build fingerprint. Missing/mismatched receipts require
full native validation. Corrupt model payloads never count as reuse; a rebuild
publishes a different immutable filename when the original path is corrupt.
Unchanged builds preserve the assembly and model bytes instead of rewriting them.

This implementation retains the existing conservative full processor build
fingerprint in the settings/cache key. Processor source changes can therefore
recook models. Looser explicit cook/importer-version reuse is not enabled yet.
Publication rechecks input content to detect edits during a changed build; this
deliberately adds a final hash pass. Same-size/time source edits between builds
invalidate reuse. Source verification and publication are not a filesystem
snapshot; independent writers require external coordination.

## Source-free placement changes

```sh
zenassetc build courtyard.json --assembly-only on
```

This explicit mode uses model keys and identities from the previous assembly in
the output directory. It changes placement, instance light policy and selected
camera/environment without accessing source files, cooking, scanning model
payloads, or validating source freshness. All requested model keys must already
exist in that assembly. Changing job processing settings in this mode does not
recook models; run a normal build to apply them. JSON output reports
`source_freshness_checked: false`. Package headers/directories and identities
are verified, while payload corruption remains detectable on subsequent reads.

## Reader APIs and validation

`ReadAssembly` returns only immutable assembly metadata. `Package::Identity`
exposes file size and the validated header hash. The header hash covers the
directory hash, whose entries include stored resource checksums.
`VerifyAssemblyDependencies` checks that relative model references resolve inside
the assembly directory and match the recorded package identities. It does not
decode geometry or textures. Both operations work with the original sources
absent and after relocating the complete cooked directory.

`zenassetc validate` on an assembly additionally reads and semantically validates
each referenced model sequentially, comparing its counts, UV sets and bounds
with the assembly records. This avoids retaining all models simultaneously.
It reports aggregate model/instance/vertex/triangle/node/light-node counts.
`inspect` remains a bounded directory inspection, not full semantic validation.

The native reader has no source parser or engine dependency. ZenEngine assembly
loading, GPU ownership, renderer scaling and light evaluation are separate
integration work; the processor preserves all authored lights regardless of
instance submission policy.

## Explicit cleanup

```sh
zenassetc clean cooked
zenassetc clean cooked --dry-run off
```

Cleanup defaults to dry-run and reports candidate paths/bytes. An explicit
`--dry-run off` deletes only valid identity-named packages directly in
`cooked/models`. Every `.zenscene` anywhere below the selected root is retained;
its complete model dependencies must open with matching identities and pass stored
payload checksums. At least one valid assembly is required. Corrupt or unreadable
assemblies/dependencies, traversal, symbolic links and Windows reparse points
abort cleanup before deletion. Unknown files and known source-provenance paths
are preserved. The scan, retained metadata and path records have explicit budgets.

Directory snapshots, retained manifest identities, and candidate package identities
are rechecked before deletion. JSON `deleted` reports actual progress if a later
filesystem error interrupts deletion. This is not a multi-file transaction.
Same-process assembly builds share the cleanup gate; other processes must coordinate
publication and deletion externally to avoid changes in the final filesystem
check/remove interval. Keep every retained assembly inside the selected root.
Do not clean while a reader holds an older assembly only in memory: retain that
assembly as a file until the reader has finished using its referenced packages.
