# Spatial atlas coordinate precision and bounded page construction

M6 remains open. FP32 half-up rounding of tile coordinates followed by native
filtering is exact on the tested power-of-two, single-level images with
anisotropy disabled. Odd dimensions and anisotropic sampling still fail the
existing gates. This evidence permits continued work on that bounded candidate;
it does not establish a general spatial sampler or enable spatial scene cooking.

Subsequent [region records](SpatialTextureRegions.md), [cooking](SpatialCooking.md)
and [engine GPU integration](../../ZenEngine/Doc/ZenAssetProcessorM6SpatialGpuVerification.md)
now carry this bounded candidate through cooked raster comparisons. The results
below retain the earlier probe scope; broader sampling and workload gates remain
open.

The subsequent [two-device diagnosis](../../ZenEngine/Doc/ZenAssetProcessorAtlasRoundingDiagnosis.md)
finds that the supported power-of-two subset requires round-to-even on the
tested AMD GPU and half-up on the tested NVIDIA GPU. Revision 10 adds
`ZEN_VULKAN_DEVICE` selection and layout 14 (truncation) to reproduce those
measurements. An unmatched device request fails. This does not establish a
vendor-wide rule or change production sampling.

## Coordinate experiments

The follow-up to [single-level sampling](SingleLevelAtlasEvaluation.md) retains
all six coordinate grids, source pixels, reference UVs, linear/clamp sampling,
and the one-byte channel gate plus zero `alpha >= 0.5` decision disagreements.
Page mapping happens after coordinate generation; the reference is never snapped
to make the comparison pass. Each operation compares a 129x129 grid. The hardware
is the same RTX 5080, driver value 2588573696, Vulkan SDK 1.4.357.0, with eight
reported subtexel precision bits and Khronos validation enabled.

Layouts 10 and 11 multiply the original float UV by the tile extent in FP64
before rounding to subtexel precision. Layout 12 uses FP32 multiplication and
half-up rounding, in contrast with layout 9's round-to-even. Layout 13 first
truncates the normalized coordinate to 24 fractional bits in FP64, then uses
half-up rounding. All map to power-of-two physical pages and retain hardware
filtering. These are measured candidates, not inferred Vulkan rounding contracts.

| Candidate, original six dimensions | Operations | Channel failures | Operations changing alpha | Alpha decisions changed |
| --- | ---: | ---: | ---: | ---: |
| FP32 even, layout 9 | 432 | 52 | 88 | 5,068 |
| FP64 even, layout 10 | 432 | 58 | 92 | 6,536 |
| FP64 half-up, layout 11 | 432 | 46 | 76 | 5,568 |
| FP32 half-up, layout 12 | 432 | 62 | 84 | 7,180 |
| Normalized fixed24, layout 13 | 432 | 46 | 72 | 5,256 |

Every candidate still reaches a two-byte channel difference. The 1x17 checker
diagnostics record original UV float bits and native/candidate values at selected
rounding boundaries. A fixed-point coordinate hypothesis fits selected 1D
observations but fails the complete 2D matrix. It does not identify the texture
unit's internal algorithm, justify a device-specific production rule, or improve
the acceptance threshold.

Revision 7 expands the matrix to fourteen dimensions. The ten power-of-two sizes
are 1x1, 2x2, 4x8, 8x4, 16x64, 32x32, 128x256, 256x128, 512x1024 and 1024x512.
The four other sizes remain 13x7, 1x17, 257x129 and 127x255. Each combines three
patterns, UNORM/sRGB, six coordinate grids and explicit/oblique-gradient sampling:
1,008 operations total. No authored levels are removed: these fixtures are
generated with one level. The two spatial neighbors retain heterogeneous sizes.

| Candidate / actual sampler | Power-of-two failures / 720 | Other-dimension failures / 288 | Channel failures, total | Alpha-changing operations, total | Largest byte difference |
| --- | ---: | ---: | ---: | ---: | ---: |
| FP32 half-up, no anisotropy | 0 | 84 | 62 | 84 | 2 |
| Fixed24, no anisotropy | 0 | 72 | 46 | 72 | 2 |
| Array layer view, no anisotropy | 0 | 0 | 0 | 0 | 0 |
| FP32 half-up, 16x anisotropy | 358 | 168 | 511 | 324 | 61 |
| Array layer view, 16x anisotropy | 0 | 0 | 0 | 0 | 0 |

Both non-anisotropic spatial candidates match every floating output channel
exactly across their 720 power-of-two operations, not just after byte rounding.
Their 11,981,520 paired samples include overlapping/repeated values; this count
does not imply independent coverage. The anisotropic spatial run changes 255,436
alpha decisions. All five expanded runs complete with zero validation errors,
finite samples and empty stderr. Spatial runs exit 1 because the full matrix
fails; array controls exit 0. The dimension split never substitutes for the
complete-matrix result.

## Construction change and verification

`AtlasPackingOptions::powerOfTwoPages` now optionally rounds each used page
extent up to a power of two within a power-of-two maximum dimension. It retains
the same regions, page assignments, tile pixels, gutters, group separation and
authored logical mips. Added page space is zero; both retained base/explicit-mip
copies are charged before allocation. Failure and cancellation still publish no
partial output and refund reservations. The default remains cropped pages.

The Vulkan probe now uploads these pages directly. Its former unbudgeted
`PadPage` copy has been removed. Revision 8 matches every revision-7 device,
allocation, sample, weight-diagnostic and terminal record for layout 12; only
the logged revision differs. Thus the 720 exact cases use actual packer output.
This construction option remains internal and is not a new CLI/scene profile.

Revision 9 makes FP64 experiments an explicit CMake option. The default shader
contains no SPIR-V Float64 capability, enables no `shaderFloat64` device feature,
and rejects layouts 10/11/13 before initialization. Its completed layout-12 run
matches every revision-8 allocation, sample, diagnostic and terminal record.
The optional FP64 build also compiles; historical FP64 measurements above retain
their matching frozen executable/shader versions.

Verification on 24 October 2026:

- Debug and Release packing suites each pass 1,327 checks, including exact
  interiors/gutters, all unoccupied bytes, minimal rounded extents, region and
  permutation stability, full/partial mips, format/groups, page bounds, storage
  accounting, cancellation and a budget that fits cropped storage but rejects
  padded storage without partial output.
- Full standalone Release suite: 33/33 pass in 16.19 seconds. Three affected
  Debug atlas/array suites pass in 0.24 seconds. These are the post-change runs;
  the earlier full Debug checkpoint remains separately recorded.
- Revision-8 and revision-9 GPU runs each complete 1,008 operations: 720
  power-of-two operations exact, 84 other-dimension comparisons fail, zero
  validation errors and empty stderr.
- The summarizer checks the complete expected single-level fixture/query matrix
  and rejects missing or duplicate operations. It records dimension subsets
  without changing either gate. C++/GLSL formatting checks pass.

## Runtime scope and remaining work

ZenEngine's `RenderScene::LoadSceneTextures` applies authored filter/wrap/mipmap
settings to a sampler whose anisotropy defaults to disabled. That supports
investigating a non-anisotropic path; it does not establish all-use eligibility.
Any integration must preserve the separate authored UV transform and tangent
derivatives, ray-query alpha behavior, previews, page ownership and failure paths.

This is RGBA8 sampling on one device, without material rendering, BC encoding,
normal reconstruction or full logical mip chains. Neither power-of-two rounding
nor the single-level restriction resolves the broader mip/filter gate. Native
allocation observations are not evidence of profitable packing or frame-time
improvement. Spatial scene cooking, runtime metadata and shaders remain disabled
pending the corresponding integration/visual evidence; array cooking remains
the existing optional path. No production FP64 requirement is introduced.

## Evidence and reproduction

The revision-4/5/6 snapshots are `build/m6-tile-double-initial`,
`build/m6-tile-weight-initial` and `build/m6-tile-fixed24-initial`. Revision 7's
expanded sources, binary, shaders and five runs are preserved under
`build/m6-tile-precision-evidence`; revision 8 under
`build/m6-power-two-pages-evidence`; final revision 9, CPU logs and current source
identities under `build/m6-power-two-final-evidence`. These are scoped evidence
snapshots, not complete standalone source distributions.

From the standalone checkout in the MSVC development shell:

```powershell
cmake -S tools/evaluation/atlas_sampling -B build/m6-atlas-sampling -G Ninja -DCMAKE_BUILD_TYPE=Release -DZENASSET_PROBE_FLOAT64=OFF
cmake --build build/m6-atlas-sampling --parallel 2
build/m6-atlas-sampling/atlas_sampling.exe build/m6-atlas-sampling/atlas_sampling.vert.spv build/m6-atlas-sampling/atlas_sampling.frag.spv 1 0 12 0
```

The final four arguments are power-of-two pages, manual-blend quantization,
layout and anisotropy. Use `0 0 6 0` for the array control and change the last
argument to `1` for actual anisotropy. For layouts 10/11/13, configure a separate
build with `-DZENASSET_PROBE_FLOAT64=ON` and use its executable and both shaders.
Preserve stdout JSONL and stderr separately. Run `summarize.py` on completed
logs; its report includes device features, full results and dimension classes.
