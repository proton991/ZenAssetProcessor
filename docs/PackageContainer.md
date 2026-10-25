# v3 package container (working specification)

This contract is implemented by the low-level reader and scene writer.
[Scene resource schemas](ScenePackageSchema.md) define the higher-level graph.
Integers are explicitly little-endian.
No C++ struct layout is written directly.

## Header

The 64-byte header contains:

| Offset | Type | Field |
| --- | --- | --- |
| 0 | 8 bytes | ASCII `ZENASSET` |
| 8 | u32 | Container version, currently 3 |
| 12 | u32 | Byte-order marker `0x01020304` |
| 16 | u32 | Header byte length, 64 |
| 20 | u32 | Directory entry byte length, 64 |
| 24 | u64 | Directory offset, 64 |
| 32 | u64 | Resource count |
| 40 | u64 | Complete file byte length |
| 48 | u64 | XXH64 of all encoded directory entries |
| 56 | u64 | XXH64 of header bytes 0 through 55 |

## Directory

Each 64-byte record contains:

| Offset | Type | Field |
| --- | --- | --- |
| 0 | u64 | Nonzero resource ID |
| 8 | u32 | Resource type |
| 12 | u32 | Resource schema version |
| 16 | u32 | Flags; bit zero marks a required resource |
| 20 | u32 | Transport codec; zero means raw, one means Zstandard |
| 24 | u64 | Payload file offset |
| 32 | u64 | Stored payload size |
| 40 | u64 | Transport-decoded payload size |
| 48 | u64 | XXH64 of stored payload bytes |
| 56 | u32 | Power-of-two alignment, at most 4096 |
| 60 | u32 | Resource-specific semantic encoding |

Entries are strictly ordered by increasing resource ID and non-overlapping
payload offset. Payloads follow the directory; alignment gaps are permitted.
The final payload ends at the declared file size. This canonical ordering
allows bounded single-pass structural validation without a second range index.

Raw chunks have equal stored and decoded sizes. Unknown required types,
versions, or transport codecs fail. Unknown optional resources can be inspected
and skipped but cannot be consumed through an unsupported decoder. Semantic
encoding support and resource references must be checked by the scene decoder.

Zstandard chunks contain exactly one ordinary frame with an explicit content
size matching the directory's decoded byte length. Dictionary IDs, skippable
frames, unknown content sizes, concatenated frames, and trailing bytes are
rejected. The reader checks frame window/workspace sizes before creating a
static decoder, and bounds input staging plus that workspace with
`ReadLimits::maxCodecBytes`. `Package::Read` can impose a smaller scratch limit;
the scene reader uses it to keep decoder storage within its remaining aggregate
allocation budget. Codec calls advance in bounded output slices for cancellation.
The chunk checksum covers stored compressed bytes.

Metadata resource version 2 appends the requested transport codec and signed
32-bit Zstandard level to its build records. Version 3 appends a one-byte
meshoptimizer boolean to those settings. Version 4 appends processing settings:
vertex-fetch optimization as a one-byte boolean, position grid bits as `u32`,
then relative and absolute position error bounds as IEEE `float32`. Version 5
appends texture settings: mip generation and BC7 compression as one-byte
booleans, encoder quality and maximum dimension as `u32`, then six `float32`
limits (color minimum PSNR, color maximum RMSE, scalar maximum error, normal
RMS and maximum angular error in degrees, and alpha coverage error).
Version 6 appends BC5-normal, BC4-occlusion and BC6H-environment activation
booleans, then the HDR relative RMSE limit as `float32`. Version 7 appends
array-page activation as a one-byte boolean, followed by maximum page dimension,
layers per page, pages and eligible input images as four `u32` values. Readers
accept metadata versions 1 through 8. Version 8 appends spatial-page activation
as a one-byte boolean and maximum page dimension, pages and eligible images as
three `u32` values. The writer retains v7 when these four values are defaults.
Absent settings default to raw transport,
disabled mesh coding, and disabled geometry/texture processing.
Actual storage is always indicated by each directory entry because an encoder
may choose raw fallback for an incompressible resource.

Texture resource version 2 appends a `u32` interpretation after `flatNormal`.
Zero is ordinary channel sampling; one identifies linear BC5 normal XY with
positive-Z reconstruction. Version 1 defaults to zero. Validation rejects
unknown interpretations and mismatches between the normal marker and BC5.
Texture resource version 3 appends `arrayLayer` as `u32`; versions 1 and 2 default
to layer zero. Image resource version 2 appends `arrayLayers` as `u32` after the
existing width, height, format, base bytes and mip arrays; version 1 defaults to
one layer. Every level stores complete tightly packed layers in ascending order.
Arrays require explicit mips, a positive layer count, correct per-level storage
sizes and in-range texture layer references. Normal one-layer records retain
Image v1 and layer-zero views retain Texture v2. Older readers fail explicitly on
required array image records instead of misinterpreting a concatenated payload.
Texture resource version 4 appends four `u32` fields after `arrayLayer`:
`atlasRegion.x`, `atlasRegion.y`, `atlasRegion.width`, `atlasRegion.height`.
Versions 1–3 default all four to zero, meaning the full image. A nonzero region
selects an interior rectangle within the referenced physical page; authored
material UV transforms retain their existing meaning. Current spatial records
require one explicit RGBA8 level, power-of-two page/tile dimensions, aligned
guarded origins and explicit linear/clamp sampling. Arrays and spatial regions
cannot be combined in this schema. Detailed validation and integration limits
are in [SpatialTextureRegions.md](SpatialTextureRegions.md). Older readers reject
the required Texture v4 record. The current ZenEngine adapter preserves these
regions, and its bounded spatial runtime path has a
[two-device checkpoint](../../ZenEngine/Doc/ZenAssetProcessorAtlasPageExtentVerification.md).
That historical engine result does not qualify later processor changes or expand
the declared spatial sampling contract.
Environment resource version 2 retains name, intensity, rotation and typed
irradiance coefficients, then writes `PixelFormat` as `u32`, size and mip count
as `u32`, and the face array. RGBA32Float stores nested typed `float32` RGBA
arrays; BC6HUfloat stores nested byte arrays. Faces use mip-major order with
six faces per mip (`+X,-X,+Y,-Y,+Z,-Z`). Version 1 omits the format field and
always contains typed RGBA32Float arrays. Readers preserve those float bits.
Only the active payload representation is serialized; unknown formats,
incorrect face/mip footprints and reserved BC6H block modes are rejected.
Other raw resource versions remain unchanged.

## Lossless mesh encoding

Semantic encoding zero is the ordinary scene field schema. Encoding one is
valid only for `VertexStream`, and encoding two only for `IndexStream`. The
writer first applies semantic coding and then optional Zstandard transport.
The directory's decoded size describes the intermediate semantic payload,
not its eventual expansion into canonical vertex or index records.

Both mesh encodings start with this 24-byte little-endian envelope:

| Offset | Type | Field |
| --- | --- | --- |
| 0 | u32 | Envelope version, one |
| 4 | u32 | Element stride: 112 bytes for vertices, 4 for indices |
| 8 | u64 | Total element count |
| 16 | u64 | Canonical schema bytes: `8 + count * stride` |

The remainder is a sequence of blocks. Each has a `u32` element count, a `u32`
encoded byte count, and that many meshoptimizer bitstream bytes. Vertex blocks
contain at most 4096 elements; index blocks at most 32768. Counts must be
nonzero, sum to the envelope count, and consume the payload exactly. A zero
total count has no blocks. Unsupported bitstream versions and trailing bytes
are rejected. Writers emit bitstream version one; readers support versions zero
and one. Index sequence coding preserves arbitrary index ordering and primitive
topologies; it does not rotate triangles. Vertex coding preserves all 112 bytes
of each canonical vertex without numerical conversion or quantization.

The decoder validates types, strides, count multiplication, configured limits,
and required output size before allocating expansion buffers. Index blocks use
at most 128 KiB of temporary native integer storage and explicit little-endian
conversion. Semantic decoding polls cancellation between blocks. Scene loading
charges transport buffers, expanded schema buffers and retained records to its
aggregate allocation limit, and checks combined vertex/index runtime geometry
bytes before allocating canonical records. The low-level `Package::Read`
returns transport-decoded bytes; `ReadScene` performs this semantic expansion.

Mesh coding falls back to ordinary schema bytes when the full envelope would
not save storage. The requested policy remains in metadata, while each
directory entry records the encoding actually used. This decision precedes
transport compression: mesh encoding plus Zstandard can be larger than
Zstandard alone for some data. Compare the complete packages for a workload.

`OpenPackage` reads only the header and directory. `Read` validates the requested
payload checksum and returns independently owned bytes. `ValidatePayloads`
checks all stored payload checksums with bounded scratch memory; it does not
substitute for semantic scene validation or validate compressed streams that the
reader does not support. The open file remains owned by the package, and reads
are serialized over that handle. Directory views require the package to remain
alive. Callers own budgets for simultaneous results across package instances.
