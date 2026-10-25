# Atlas construction checkpoint

`src/Processing/AtlasPacking.h` is an internal page-construction primitive for
M6. It is compiled into Processor and has no Reader dependency. It is not yet
called by `CookScene` or exposed as a processing option. The standalone format
can now serialize logical [spatial region records](SpatialTextureRegions.md)
and crop their previews; ZenEngine explicitly rejects those records pending
runtime integration. The complete atlas milestone remains open.

## Representation and construction

The caller supplies immutable, already-finalized RGBA8 images with unique stable
tile IDs and compatibility-group IDs. The caller must qualify every material
use and assign groups for role, sampling, interpretation and quality requirements.
The packer additionally separates exact pixel formats, including sRGB versus
linear. It does not infer material eligibility from pixels.

Each authored logical mip gets its own guarded rectangle in a physical level-zero
page. Partial mip chains stay partial, and the 1x1 tail retains its own rectangle.
No mip pixels are generated, resampled, rotated, or compressed here. Four-texel
alignment and a configurable multiple-of-four gutter prevent a compression block
from crossing between unrelated rectangles. Every gutter, corner and alignment
padding texel repeats the closest tile texel. Empty page space is deterministic
zero. Compression must occur after complete page construction and per-tile quality
assessment; the packer does not claim that encoded pages pass those gates.

Placement uses deterministic first-fit decreasing-height shelves. The ordering is
compatibility group, exact format, descending guarded height/width, stable tile ID,
then logical mip. Tile records are sorted by stable ID independent of input order.
Pages crop to used four-texel-aligned extents by default; rotation is never used. This packing
heuristic is bounded, not an optimal area solver. The defaults limit dimensions
to 2048, pages to 64, tiles to 8192 and logical mip regions to 65536. Dimensions
cannot exceed 16384. The caller supplies a requested-storage budget; all retained
arrays, image records, duplicated base/explicit-mip bytes and planning arrays are
charged before allocation. Borrowed input, allocator overhead and fixed stack
storage are excluded. Planning scratch is refunded after success. Failures and
cancellation expose no partial atlas and refund all charges.

`AtlasPackingOptions::powerOfTwoPages` optionally rounds the used width and height
up to their smallest containing powers of two, preserving the same regions and
page assignments. This option requires a power-of-two `maxPageDimension`, so
rounded pages still obey that limit. All added bytes in both retained copies
count toward the budget before allocation; empty padded space is zero. The
option is internal construction policy, not a new public cooking/CLI mode.

Physical pages contain exactly one explicit mip to prevent accidental whole-page
mip generation. Their logical-mip mapping is not a hardware mip chain. The tested
manual LOD/blending candidate exceeded the one-byte GPU error gate; do not use
this mapping as the general mipmapped package representation. See
[GPU sampling results](AtlasSamplingEvaluation.md). A spatial single-level path
still needs its own eligibility and integration evidence.
The subsequent [single-level GPU evaluation](SingleLevelAtlasEvaluation.md)
rejects the tested cropped and power-of-two mappings under the unchanged channel
gate. The construction kernel's exact copies do not establish runtime sampling
equivalence.
The later [coordinate-precision checkpoint](AtlasCoordinatePrecisionEvaluation.md)
reports 720 exact power-of-two single-level GPU comparisons using actual packer
pages and FP32 half-up coordinate rounding, with anisotropy disabled. Other
dimensions and anisotropic sampling still fail. The updated CPU packing suite
passes 1,327 checks in Debug and Release; the full Release suite passes 33/33,
and three affected Debug atlas/array suites pass. These results do not establish
the remaining scene/runtime, full mip/filter or workload gates.
Authored UV transforms and normal-basis derivatives remain separate from the page
mapping. Native images must be shared by logical textures referencing the same
page. Profitability, compression, metadata, previews and rendering are subsequent
integration steps, not established by this kernel.

## Verification on 24 October 2026

- `ZenAssetAtlasPackingTests` passed 1172 checks in the MSVC Release build. Tests
  inspect actual output bytes, non-overlapping guarded rectangles, alignment,
  group/format separation, odd/tiny dimensions, implicit base levels, partial/full
  mip chains, permutation determinism, bounded pages, invalid inputs, memory
  accounting and cancellation during planning/copying.
- CPU double-precision bilinear sampling at boundaries and blending logical mips
  match the same source pixels within `1e-10` channel units for the fixtures.
  These are mathematical checks on encoded channel values, not measurements of
  GPU LOD/filter precision, sRGB conversion, anisotropy or rendered alpha coverage.
- The full standalone Release suite passed 30/30 in 19.22 seconds. Logs are
  `build/processor-port/m6-atlas-packing-{build,tests}.log` and
  `m6-atlas-full-{build,tests}.log`.
- The same 1172 checks passed in an unoptimized Clang 22 AddressSanitizer build
  with no sanitizer report. An initial executable launch failed before testing;
  prepending Clang 22's matching runtime directory to the process PATH allowed
  the run. Build/configure/test logs are `build/m6-atlas-sanitizer-*.log`, and its
  isolated CMake harness is `build/m6-atlas-sanitizer-src/CMakeLists.txt`.

`build/m6-atlas-checkpoint-identities.json` records the tested source and executable
hashes. These CPU checks establish placement mathematics. The subsequent GPU
evaluation rejected both the manual logical-mip sampler and tested guarded native
spatial chain under the existing one-channel-step threshold.

## Array page construction

`src/Processing/ArrayPacking.h` provides `PackArrayPages` for finalized images.
It groups exact dimensions, format and explicit mip count within caller-assigned
compatibility groups. Stable IDs determine layer order independently of input
order; output mappings are sorted by ID. Limits bound dimensions, pages, layers
and input count. Singleton groups/remainders retain standalone storage. The
caller still decides all-use eligibility and measured profitability.

Every level is stored as consecutive complete layers. Copies preserve raw RGBA8,
BC7, BC4 and BC5 bytes, including partial chains and tiny/odd block extents.
Compression quality belongs to the preceding role-aware image processing; copying
complete encoded layers cannot share a block with another layer or change its
decoded texels. No gutters, rotation, resampling or mip generation are needed
for array layers. Existing arrays and images without explicit finalized levels
are rejected as inputs to this primitive.

Requested output and planning storage is charged before allocation. Scratch is
refunded, and failure/cancellation clears output and refunds all charges. Copying
polls cancellation at chunks of at most 1 MiB. `ownedBytes` counts retained
requested records, vectors and bytes, including the existing duplicated base/mip
zero representation; borrowed input and allocator overhead are excluded.

The new packing/format suite passes 3175 checks in Debug and Release, including
all preserved bytes, group separation, deterministic permutations, bounds,
cancellation, raw/Zstandard round trips, selected-layer previews and semantically
invalid fields in otherwise valid-checksum packages. The production output also
passes the GPU layer-view comparison with actual 16x anisotropy. These primitive
and format checks are distinct from scene eligibility and engine runtime use.
The subsequent [array-cooking checkpoint](ArrayCooking.md) adds opt-in CLI/API
processing and all-use eligibility; the subsequent engine runtime checkpoint
connects shared native uploads and individual layer views.

The complete standalone Release suite passes 31/31 in 18.85 seconds. Seven
affected Debug suites pass in 203.65 seconds. Logs are
`build/processor-port/m6-array-release-tests.log`, `m6-array-debug-tests.log` and
`m6-array-release-focused.log`; build logs distinguish the Debug and Release
configurations. ZenEngine's 11 adapter tests pass with no tracked CPU leaks; the
boundary at that checkpoint rejected unsupported array pages. The subsequent
CPU integration carries layers into engine assets/previews. Native integration
now passes ownership/failure tests and exact RGBA8/BC7/BC4/BC5 sampling comparisons
in inline/threaded execution. See ZenEngine's
`Doc/ZenAssetProcessorM6RuntimeVerification.md` for evidence and remaining
cooked-package mixed-scene checkpoint and remaining visual/workload gates.
