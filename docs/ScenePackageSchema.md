# Raw scene resources in package v3

The raw reference profile serializes the canonical `Scene` contract into the
[v3 container](PackageContainer.md). `src/Format/SceneSchema.h` defines each
record's field order once for both reader and writer. `SceneResources.h` defines
the division into resources and their deterministic ordering. These are the
authoritative schema definitions; changing an existing field order or meaning
requires a resource schema version change.

Resources use schema version 1 except Metadata (version 7 or 8, accepting 1–8),
Texture (version 2, 3 or 4, accepting 1–4), Image (version 1 or 2, accepting 1–2),
and Environment (version 2, accepting 1–2). The writer retains Image v1 for
single-layer images and Texture v2 for layer-zero views; arrays and nonzero
layer views use their respective newer schemas. A spatial region requires
Texture v4; ordinary records keep their existing versions. Spatial records are
supported by the standalone format/Reader and optional [spatial cooking](SpatialCooking.md),
with engine rendering still pending; see [SpatialTextureRegions.md](SpatialTextureRegions.md).
Metadata v7 remains the default; any nondefault spatial setting selects v8.
Their versioned field additions are specified in the container document.
Raw defaults use semantic encoding zero and
transport codec zero; optional lossless mesh coding and Zstandard transport
follow the container specification. Resources use the required flag and
16-byte alignment. An ID is
`(uint64_t(resourceType) << 32) | (ordinal + 1)`. Resource ordinals are zero-based
indices in the corresponding canonical array. Singleton ordinals are zero.

| Type | Resource | Granularity |
| --- | --- | --- |
| 1 | Scene | One manifest: name, roots, UV-set numbers, environment selection, counts |
| 2 | Mesh | One mesh with primitives, source-vertex maps and full influences |
| 3 | Material | One complete core/extended material |
| 4 | Texture | One image/sampler/view binding |
| 5 | Animation | One clip with samplers and resolved semantic targets |
| 6 | Skin | One skeleton, joints and inverse-bind matrices |
| 7 | Morph | One primitive's morph payload |
| 8 | Environment | One environment's coefficients and typed-float or BC6H HDR mip faces |
| 9 | Metadata | One build identity, input revision list, source path and import dependency list |
| 10 | VertexStream | One array of canonical vertices |
| 11 | IndexStream | One array of absolute u32 indices |
| 12 | CustomAttributes | One custom vertex attribute payload |
| 13 | Image | One owned image/block payload and its mip levels |
| 14 | Node | One node definition and its references |
| 15 | Sampler | One filter/addressing definition |
| 16 | Camera | One camera |
| 17 | Light | One punctual light |
| 18 | MaterialVariants | One array of variant names |
| 19 | MaterialVariant | One primitive's variant bindings |
| 20 | TextureCoordinates | One array of per-vertex UV arrays |
| 21 | SourceDocument | One optional-source-information UTF-8 string, possibly empty |

Scene, Metadata, VertexStream, IndexStream, MaterialVariants,
TextureCoordinates and SourceDocument resources must each occur exactly once.
The manifest counts only required resources, and every required typed ordinal
must be contiguous and agree with those counts. Unreferenced optional directory
entries may carry unknown versions or encodings and are skipped. A required
reference cannot be satisfied by an optional entry.

Scalars use explicit little-endian bytes, including IEEE-754 float bit patterns.
Enums and asset IDs use u32; absent IDs retain `UINT32_MAX`. Booleans use one byte
and accept only zero or one. Strings use a u64 byte count and UTF-8 bytes. Arrays
use a u64 element count and their element schemas. Matrices are column-major;
quaternion records are `w,x,y,z`. Named math and vertex array transfers may copy
bytes on little-endian machines only when compile-time size checks establish
that their layout exactly matches the field schema. No container, pointer,
padding, or general C++ object representation is persisted.

Build metadata includes source format, importer/processor versions, library build
fingerprint, profile, source settings, cache key, build-time input revisions and
diagnostics and requested compression/processing settings. Build-time input revisions include the root source and may include
post-import content hashes. They are independent of the canonical scene's
import-time dependency stamps. Empty build fingerprints are populated from
`LibraryBuildFingerprint()` when writing. The source document has its own resource
so metadata lookup never reads a potentially large embedded glTF JSON document.

`ReadSceneMetadata` reads only the directory and Metadata payload. It validates
that payload's identity, bounds, checksum, UTF-8 and schema; it does not read
other payloads or touch source files. `ReadScene` decodes all required resources,
then applies the shared semantic scene validator before publishing an owning
immutable result. Neither API requires the original source model or textures.

Read limits are checked before requested array/string/resource allocations.
The aggregate scene budget counts directory records, scene/metadata objects,
decoded array/string storage, the current resource buffer and the shared
validator's bulk scratch arrays. It measures requested storage, not process RSS:
allocator bookkeeping, fixed stack objects and file-library buffers are outside
the counter. The runtime geometry limit separately bounds the expansion needed
by the engine adapter. Caller-owned inputs and simultaneous independent calls
remain the caller's responsibility.

The writer validates input semantics, checks the directory budget, sizes each
resource without allocating its payload, allocates exactly that resource's
requested buffer, and serializes it. The directory plus current payload share
the write allocation limit. Manifest and metadata views borrow source fields;
no whole-scene byte buffer or source-document copy is made. Publication uses an
exclusive temporary sibling, flushes and closes the complete file, then replaces
the destination atomically. Failure or cooperative cancellation before publication
preserves existing output. A retained reader continues to read its original file.

Current tests cover complete synthetic resources, imported glTF/FBX fixtures,
deterministic raw rewrites, metadata-only access, malformed UTF-8, oversized
counts, inconsistent manifests, semantically invalid geometry with valid checksums,
optional-resource versioning, byte/allocation limits, retained readers and failed
or cancelled replacement writes. Lossless compression and optional geometry/
texture cooking have separate tests. Atlas transforms and incremental resource
decoding remain later processing stages.
