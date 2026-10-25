# Atlas sampling evaluation

Verified 24 October 2026 on NVIDIA GeForce RTX 5080, Vulkan driver value
2588573696, using Vulkan SDK 1.4.357.0 and the Khronos validation layer.
The two spatial mip layouts tested exceeded the existing one-byte channel-error
gate. Native array layers preserved identical sampling, including with 16x
anisotropy. Array pages are therefore the current path for compatible mipmapped
images; spatial atlas support and measured workload benefit remain open.

A subsequent [single-level evaluation](SingleLevelAtlasEvaluation.md) also rejects
the proposed cropped and power-of-two spatial mappings, including a `precise`
arithmetic diagnostic. Additional coordinate grids expose two-step channel
errors that the original evenly spaced grid missed. An array control remains
exact. Single-level spatial sampling must not be treated as an accepted fallback.

## Probe and scope

`tools/evaluation/atlas_sampling` is an isolated Vulkan evaluation executable.
Vulkan is not added to any shipped library target. A fullscreen fragment shader
writes paired reference/candidate samples into a host-readable storage buffer.
The reference uploads the exact same authored mip bytes as a standalone image.
Each query samples a 129 by 129 grid, including domain boundaries. Fixtures use
high-frequency channel patterns, black/white checkers and alternating constant
mips, in RGBA8 UNORM and sRGB. Alpha is checked both numerically and against 0.5.
These deliberately independent authored levels are valid mip data; they are not
claimed to be representative natural-image downsampling.

Array/logical-layout fixtures cover 32x32, 13x7, 1x17 and 257x129 images, full
chains and partial two-level chains. Twelve explicit LOD queries include
magnification, fractional levels and the tail. Six gradient queries rotate the
footprint and increase its major/minor ratio to 32. The additional six hardware
LOD diagnostic queries repeat the native/array sampling operations and are
excluded from distinct-query counts for those layouts. Three contrasting images
share each array, with the subject in the middle layer. The sampler clamps to
edge; this experiment does not establish wrap, animated UV, compressed sampling,
material shading, engine lifetime or whole-scene performance behavior.

The native spatial prototype uses power-of-two cells and separately extruded
content at every physical mip. A base gutter of `1 << (mipCount - 1)` keeps the
tail isolated. Its fixtures use 32x32, 8x32, 256x256 and 256x128 images; rectangular
cases use partial two-level chains. It has 36 fixtures rather than 48.

## Results

All completed runs below have zero Vulkan validation errors. Failure means a
rounded eight-bit channel difference above one (or incorrect bound logical mip
in the manual layout). The same threshold was retained for all layouts.

| Layout | Logged queries | Distinct queries | Failed logged queries | Largest byte error |
| --- | ---: | ---: | ---: | ---: |
| Separate logical mip rectangles, ideal LOD | 864 | 864 | 194 | 19 |
| Power-of-two logical pages, quantized blend and LOD diagnostics | 1152 | 1152 | 258 | 19 |
| Guarded native spatial mip chain | 864 | 648 | 62 | 2 |
| Native array, sampler2DArray | 1152 | 864 | 0 | 0 |
| Array with individual sampler2D layer view, linear/trilinear | 1152 | 864 | 0 | 0 |
| Array with individual sampler2D layer view, actual 16x anisotropy | 1152 | 864 | 0 | 0 |
| Production PackArrayPages output and layer view, actual 16x anisotropy | 1152 | 864 | 0 | 0 |

The power-of-two diagnostic run includes both ideal-gradient and hardware-query
variants. Hardware LOD queries reduce the maximum gradient error from 19 to 3
bytes, but still fail the gate. Every checked integer-mip page sample matches its
corresponding original texture sample exactly. Manually blending integer-mip
samples from the original texture reproduces the remaining error, so changing
the rectangle copy alone cannot resolve it. This does not establish an exact
driver-internal cause or a driver defect.

Array runs have zero floating-point reference/candidate difference and zero
alpha-threshold changes, including the production-packer run. Diagnostic
`native_blend_error`, `wrong_bound_levels` and queried-LOD differences describe
the unused manual-blend alternative. They are not the native array's sampling
algorithm and are not evidence of an array rendering failure.

The first run named `array-view-anisotropic.jsonl` requested anisotropy on the
device but left it disabled on the sampler. It is retained under
`array-view-linear-baseline/` as linear/trilinear evidence only. The corrected
run records `sampler_anisotropy_enabled: true` and `sampler_max_anisotropy: 16` in
its device record; it is archived under `array-view-16x-baseline/`.

## Allocation measurements

These are sums of `VkMemoryRequirements.size` for three standalone images versus
one page, with dedicated allocations. They exclude staging, VMA allocation
policy, descriptor memory and frame timing. Both color-transfer formats gave
the same values. Full and partial array-chain requirements also matched here.

| Extent of each of three array layers | Separate images, bytes | Array image, bytes |
| --- | ---: | ---: |
| 32x32 | 21504 | 24576 |
| 13x7 | 6144 | 6144 |
| 1x17 | 12288 | 12288 |
| 257x129 | 972288 | 983040 |

Arrays reduce the native image/allocation count from three to one in this probe,
but do not reduce allocation bytes. The guarded native spatial chain is much
larger: full 32x32 chains use 351232 versus 21504 bytes; full 256x256 chains use
22371328 versus 1053696. Neither object-count reduction nor a packed file proves
a frame-time or memory win. Real scenes must establish when enabling pages pays.

## Reproduction and evidence

From a Visual Studio x64 developer environment with Vulkan SDK available:

```powershell
cmake -S tools/evaluation/atlas_sampling -B build/m6-atlas-sampling -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build/m6-atlas-sampling --parallel 2
build/m6-atlas-sampling/atlas_sampling.exe build/m6-atlas-sampling/atlas_sampling.vert.spv build/m6-atlas-sampling/atlas_sampling.frag.spv 0 0 3 1
```

The final two options select layout 3 (individual 2D views of packed array
layers) and enabled anisotropy. Layouts 0, 1 and 2 select manual logical mips,
native spatial mips and sampler2DArray. The two preceding options select page
power-of-two padding and manual blend quantization. Historical compute-only and
earlier graphics baselines have different command signatures; their executable,
shader and source snapshots are retained alongside their output.

Evidence is under `build/m6-atlas-sampling/`: `manual-lod-baseline/`,
`logical-mips-diagnostic/`, `native-baseline/`, `array-baseline/`,
`array-view-linear-baseline/`, `array-view-16x-baseline/`, and
`production-array-16x.jsonl`. `comparison-summary.json` aggregates completed logs,
and `tools/evaluation/atlas_sampling/summarize.py` regenerates it. The production
source, executable, shaders and result hashes are in
`build/m6-array-evidence/identities.json`. Historical snapshots establish their
recorded experiment; only the production run exercises the new array packer.

## Remaining integration

`PackArrayPages` now copies finalized compatible image levels, including encoded
blocks, into explicit native array-layer storage. Package Image v2 and Texture
v3 describe the page/layer relationship while ordinary images retain their
previous wire schemas. [Array cooking](ArrayCooking.md) now provides all-use
eligibility, optional CLI/API/manifest settings, bounded page publication and
CPU checks for mixed scenes, variants, overrides and unchanged UV semantics.
Engine CPU page/layer records and bounded layer previews pass their focused tests.
Engine native page ownership/uploads and layer-view descriptors are now connected.
Its separate runtime checkpoint passes 713 RenderCore tests, eight editor
regressions and two Vulkan sampling cases with exact RGBA8/BC7/BC4/BC5 references
in inline/threaded execution. Those fixtures construct engine records directly;
they complement this production-packer probe. A subsequent cooked-package test
passes 16 exact PBR frame pairs with source files absent, covering raw and
BC7/BC5/BC4 pages, variants, UV transforms, shared instances and a standalone
repeating alpha mask. See ZenEngine's `Doc/ZenAssetProcessorM6RuntimeVerification.md`
for this bounded mixed-scene evidence and the broader visual gaps.
Profitability and workload measurements remain M6 work.
The subsequent engine sampling checkpoint passes 64 cooked frame pairs under
minification/grazing views and native 16x anisotropic gradient/mip-tail tests.
Its production texture allocations are equal at 16x16 and slightly larger for
arrays at 128x128, with unchanged binding counts. See ZenEngine's
`Doc/ZenAssetProcessorM6SamplingMeasurements.md` for measured values and limits.
Spatial atlas support remains open for cases with demonstrated sampling parity.
