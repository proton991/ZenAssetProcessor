# Spatial texture region records

The standalone format and Reader can now preserve and preview a bounded spatial
atlas region. Optional [spatial cooking](SpatialCooking.md) now publishes these
records from qualified source images. ZenEngine now retains regions in CPU
assets and scene components and crops editor previews. Its bounded GPU path now
uploads shared spatial pages and maps separate material regions. See the
[engine GPU checkpoint](../../ZenEngine/Doc/ZenAssetProcessorM6SpatialGpuVerification.md)
for native/frame comparisons and remaining sampling and workload limitations.
Existing standalone textures and optional array cooking retain their behavior.

## Representation

`TextureAsset::atlasRegion` contains an `ImageRegion`: four unsigned 32-bit
integers `x`, `y`, `width`, `height`. All zero means the full referenced image.
A nonzero region describes the interior of one logical tile in a physical page.
The image ID still selects the immutable shared pixel owner. Texture IDs, view
formats, samplers, linear-view aliases and authored material UV transforms remain
independent. No material or geometry UV is rewritten by serialization.

Texture resource schema **4** appends those four fields after the schema-3
`arrayLayer`. Earlier texture schemas default to an all-zero region. The writer
uses v4 only for a nonzero region; ordinary layer-zero textures keep v2 and
nonzero array layers keep v3. The outer package remains version 3. The new
required record is rejected by older readers rather than being read as a
full-image binding. This schema is still part of the unreleased v3 development
contract.

Current nonzero-region validation requires:

- RGBA8 UNORM/sRGB storage-compatible views, one image layer and texture layer
  zero, and exactly one explicit physical mip including level zero.
- Power-of-two logical tile and physical page dimensions; page dimensions at
  most 16,384. This restriction follows the current sampling evidence and does
  not claim that all power-of-two sampling modes are supported.
- A nonempty contained rectangle, four-texel-aligned interior origins, and at
  least four physical texels outside each interior edge. Bounds are checked
  before subtraction, preventing overflowing coordinates from passing validation.
- An explicit sampler with linear minification, magnification and mip filtering,
  and clamp-to-edge in both axes. The sampler's existing mipmap flag is retained.

Scene validation checks the complete page payload and its existing image
invariants. Rectangle checks do not prove extruded gutter contents, material
eligibility, nonoverlapping packing or visual equivalence. Those remain
construction/cooking/runtime responsibilities. The packer tests separately
verify copies and gutters. No spatial-plus-array or compressed spatial profile
is enabled by these records.

## Bounded previews and recooking

`ImageMipView::region` selects a crop inside the complete supplied physical mip.
The all-zero default preserves the prior preview API behavior. Output dimensions,
aspect ratio and sample taps use the logical region; RGBA byte offsets and BC
block strides still use the physical page. Every tap stays inside the region.
Only the bounded output is allocated; no intermediate tile or full-page decode
is allocated. Input byte-size validation still requires the whole physical mip.

This preview crop is a general rectangle operation and can handle BC blocks
crossed by an unaligned crop. It does not enable compressed spatial scene
records: those remain rejected by the scene contract above. Invalid/overflowing
regions, insufficient output budget, truncated storage and cancellation return
failure with empty output.

No-op cooking retains the original scene owner. Array processing recognizes
spatial page owners, preserves every alias/region and reports an exclusion.
Mip generation or ordinary BC7/BC5/BC4 texture processing on a scene containing
a spatial region returns an actionable `UnsupportedFeature` and no cooked
scene; callers must recook original sources. This prevents whole-page mip or
encoding operations from changing logical tile semantics before spatial cooking
has its own quality checks.

## Verification on 24 October 2026

`SceneIOTests.cpp` constructs actual power-of-two packer output from three
differently sized source tiles and a compatible sRGB alias. It verifies raw and
Zstandard package round trips, all public fields, independent authored UV
transforms, texture schema selection, full/bounded previews and explicit failure
without modifying source scenes or previously published destinations.

Twelve mutations of rectangle fields retain valid container/resource checksums
but fail semantic loading without publishing a scene. Unknown schema 5 and a
schema-3 record with trailing spatial fields also fail. Unsupported mip chains,
page dimensions, arrays, layer selection and sampler policies are rejected.
Clearing the regions writes Texture v2 and preserves existing full-image loading.

`ImagePreviewTests.cpp` additionally crosses both physical block axes with a
2x2 BC4 crop, checks its downsampled value, and exercises malformed/overflowing
regions, complete physical storage, output bounds and cancellation.

| Post-change verification | Result |
| --- | --- |
| Full standalone Release suite | 33/33, 16.98 seconds |
| Debug scene I/O, preview, array packing and array processing suites | 4/4, 0.46 seconds |
| Reader-only Release suite | 4/4, 0.33 seconds |
| Engine adapter, scene I/O and cache suites | 39/39, no tracked CPU leaks |
| Current runtime smoke executable, Texture v4 package | Explicit unsupported spatial-region error |
| Current runtime smoke executable, full-image Texture v2 control | Loads all four texture records |
| Previously frozen installed runtime, Texture v4 package | Explicit unsupported resource-version error |

The affected scene-I/O executable reports 208 checks. Reader-only rebuilding
still has only bcdec source/build/subbuild directories under `_deps`; this change
adds no importer or encoder dependency to the Reader.

The first new fixture omitted its UV-set catalog, causing validation failure
and then a test-side access to absent serialized bytes. The fixture now declares
its UV set and corruption checks require a successfully written/read baseline.
A subsequent existing array test caught changed exclusion wording; its original
diagnostic is preserved and spatial pages now have a separate reason. Both
initial failures and the successful final runs are retained in the evidence.
No validation or quality threshold was relaxed.

Logs are under `build/processor-port/m6-spatial-record-*`,
`build/reader-source-evidence/spatial-record-*` and ZenEngine's
`build/asset-integration-release/m6-spatial-record-*`. The three application
load checks are in the engine's `m6-spatial-region-runtime/results.json`.
`build/m6-spatial-record-evidence/identities.json` records selected source,
binary, package and log snapshots. It is scoped evidence, not a complete source
distribution.

All-use spatial eligibility and publication from `CookScene` have since been
implemented; see [spatial cooking](SpatialCooking.md) for that later checkpoint.
Engine CPU ownership/region propagation and cropped previews are now implemented
as described in the later engine checkpoint above. Native page/view ownership,
region sampling and bounded cooked raster comparisons now pass their checkpoint.
The later [cooked spatial ray checkpoint](../../ZenEngine/Doc/ZenAssetProcessorM6SpatialRayVerification.md)
adds exact alpha-query parity through production software/hardware traversal,
including fragment shader context. The later engine
[exact guide capture checkpoint](../../ZenEngine/Doc/ZenAssetProcessorGuideCaptureVerification.md)
passes bounded full GI comparisons with guide learning retained in an opt-in
capture mode. The subsequent
[page-extent checkpoint](../../ZenEngine/Doc/ZenAssetProcessorAtlasPageExtentVerification.md)
passes bounded cooked raster/GI/ray and repeat comparisons on both AMD and NVIDIA,
resolving the specific AMD failure documented between those checkpoints.
Other devices and sampling profiles remain unverified. The current contract
continues to exclude NPOT tiles, spatial mip chains, block-compressed spatial
pages and spatial anisotropy; these results do not enable those combinations.
[Matched processor costs](AtlasCookingMeasurements.md) are now recorded, while
representative workload and runtime benefit remain open. Historical checkpoints
retain their original fingerprints and do not close the complete M6 gate.
