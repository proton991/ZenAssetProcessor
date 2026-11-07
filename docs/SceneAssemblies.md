# Native scene assemblies

An assembly references separately cooked model packages. It does not flatten or
copy model geometry, images or local resource IDs. Runtime source import and an
engine dependency are not required by the reader.

`ZenAsset/Format/Assembly.h` defines models, instances, camera/environment
selection, and summary metadata. `SummarizeAssemblyModel` obtains counts and
undeformed bounds in authored coordinates from a model's hierarchy.
Its traversal is iterative, computes each node transform once, and admits scratch
storage before allocation (`maxScratchBytes`, 64 MiB by default). Cancellation
and admission failure preserve the previous output summary.
`FinalizeAssembly` checks references and affine transforms and calculates combined
bounds/counts. Counts are conservative append capacities: repeated instances count
their model again. `lightNodes` counts instantiated source light nodes separately
from light definitions. Per-model UV set lists retain their compact slot ordering;
the assembly exposes a sorted union for runtime buffer planning.

Model and instance keys use ASCII letters, digits, underscores and hyphens and
must be unique in their respective tables. Models have portable relative
`.zenasset` paths below the assembly directory. Absolute paths, parent traversal,
backslashes, empty components and Windows alternate-stream syntax are rejected.
`VerifyAssemblyDependencies` also rejects symbolic-link escapes from the resolved
assembly directory.

Each model records `PackageIdentity`: file length and the stored header hash. The
validated header covers the directory hash, and the validated directory covers
individual stored resource checksums. `Package::Identity()` reads this identity
without reading model payloads. It is not a cryptographic authentication mechanism.
`validationFingerprint` is a builder receipt associated with that identity; the
native reader does not trust it as a substitute for validating resources on read.

Instances reference a model key, carry a column-major affine transform, and have
an explicit `submitLights` flag. Disabling submission preserves all serialized
model lights. Camera/environment selectors identify an instance and a model-local
camera/image-based-light index. Empty instance plus `UINT32_MAX` means no selection.
No implicit environment or camera is selected by model order.

## Reader and writer

`WriteAssembly` uses the same atomic package writer, limits, cancellation,
compression and publication callback contract as `WriteScene`.

`ReadAssembly` reads the assembly only. It checks bounded allocation, references,
finite affine placement, selectors, and agreement between stored and computed
aggregate metadata. Failed reads return no partial assembly.
`VerifyAssemblyDependencies` opens each referenced package sequentially, checks its
identity and required scene presence, and then releases it. It does not decode
geometry, validate all payloads or retain all models. Consumers must still use
`ReadScene` for full model validation, or `ValidatePayloads` for stored integrity.
Summary counts are allocation hints subject to actual decoded scene validation,
not permission to trust invalid model counts or skip checked arithmetic.

## Container resource

`.zenscene` uses the existing `ZENASSET` v3 header/directory and contains exactly
one required `Assembly` resource (type 22, schema 1, semantic encoding 0). Its ID
uses the existing `(type << 32) | 1` rule. Raw and Zstandard transport work as for
other resources. Older readers reject the unknown required resource. A scene
reader rejects an assembly instead of interpreting it as scene geometry.

The field schema is shared by encoder and decoder in `src/Format/AssemblySchema.h`.
Integers remain little-endian, strings/arrays have u64 counts, booleans occupy one
byte, and floating-point/matrix values use the existing scene archive encoding.
The assembly stores, in order: name, models, instances, camera selector,
environment selector, aggregate counts, UV sets, bounds and bounds-presence flag.
Model records include key, path, package identity, counts, UV sets, bounds,
bounds-presence flag and validation fingerprint. Instance records include key,
model key, transform and light policy. No C++ object layout is serialized.

Bounds describe undeformed geometry under authored default node transforms. They
are not animation motion envelopes. Aggregate counts are not measured CPU/GPU
memory, and the assembly does not provide automatic streaming or independent
runtime animation playback.
