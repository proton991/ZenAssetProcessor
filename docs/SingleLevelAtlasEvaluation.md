# Single-level spatial atlas evaluation

The proposed clamp-to-edge, linear-filtered, single-level spatial path is **not
ready for runtime integration**. All three initial spatial candidates below exceed the
existing one-step eight-bit channel gate. Native array pages remain exact in the
expanded coordinate control. This does not complete M6 or remove spatial atlas
support from its scope.

The later [coordinate-precision checkpoint](AtlasCoordinatePrecisionEvaluation.md)
finds exact FP32 half-up sampling for 720 power-of-two, single-level operations
with anisotropy disabled. Other dimensions and anisotropic sampling still fail.
That checkpoint also moves bounded power-of-two page construction into the
packer and verifies it without requiring FP64. It retains all failures below
and leaves spatial runtime integration and the full M6 gate open.

## Test and results

Verified on 24 October 2026 with the isolated Vulkan probe in
`tools/evaluation/atlas_sampling`, MSVC Release, Vulkan SDK 1.4.357.0, NVIDIA
GeForce RTX 5080, driver value 2588573696 and Khronos validation. The device reports
eight subtexel precision bits and eight mipmap precision bits. Anisotropy is
disabled on the actual sampler, which uses linear minification/magnification,
linear mip filtering and clamp-to-edge. Both reference and candidate images have
one explicitly authored level; the test does not discard an authored mip chain
or change its minification policy.

The 36 fixtures combine six dimensions (32x32, 13x7, 1x17, 257x129, 127x255 and
1x1), three channel patterns and RGBA8 UNORM/sRGB. Patterns include high-frequency
channels, black/white checkers and constant channels. `PackAtlasTiles` supplies
actual spatial pages and guarded regions. Two neighboring images use different
dimensions (140x80 and 67x99) and contrasting patterns. The power-of-two candidate
pads the completed page in the probe; this is not a new production packer option.

Each operation samples a 129x129 grid. The three coordinate sets are:

- Boundary-inclusive coordinates `cell / 128`, retained from the earlier probe.
- Non-dyadic centers `(cell + 0.5) / 129`.
- Deterministic dispersed coordinates using different modular integer sequences
  for U and V, normalized by 65520.

| Candidate | Logged operations | Operations exceeding channel gate | Operations changing alpha decision | Largest byte difference |
| --- | ---: | ---: | ---: | ---: |
| Cropped spatial page | 1944 | 252 | 360 | 2 |
| Power-of-two spatial page | 1944 | 54 | 252 | 2 |
| Power-of-two page, `precise` mapping arithmetic | 216 | 6 | 28 | 2 |
| Production array page, individual 2D layer view | 216 | 0 | 0 | 0 |

All runs completed with **zero Vulkan validation errors**, empty diagnostic
stderr and finite sampled values. Spatial runs intentionally exit 1 for failed
comparisons; the array control exits 0. The original boundary-inclusive coordinate
set is exact for both initial spatial runs. The new coordinate sets expose the
failures, so that original grid alone is insufficient sampling evidence.

The initial runs logged twelve explicit LODs and six gradients per coordinate
set. With a single level and anisotropy disabled, these repeated the same sampled
values. Probe revision 2 retains one explicit level-zero operation and one
oblique minifying gradient per set. Its 216 operations cover 108 fixture/grid
combinations and 3,594,456 paired samples. Do not treat repeated operations or
overlapping coordinates as independent coverage. The reduced operation count
does not improve the threshold or remove a failing coordinate set.

The `precise` diagnostic separates scale, offset and normalization operations;
the compiled SPIR-V contains `NoContraction` decorations. Its aggregate failures
remain unchanged after accounting for repeated initial operations. Comparing
selected initial operations against their precise counterparts gives identical
reported comparison metrics in 214 of 216 cases; the remaining two do not alter
the result. This rejects that arithmetic change as a remedy. It does not identify
the driver's internal filtering algorithm or establish a driver defect.

The array control uses `PackArrayPages`, the same subject image/coordinates and
two contrasting equal-sized neighbors. Every sampled channel matches exactly,
with zero alpha-decision changes. Different neighbor dimensions are required by
the two constructions, so their allocation measurements are not a paired
array-versus-spatial workload comparison.

## Thresholds and limitations

The unchanged numerical gate is a rounded eight-bit channel difference of at
most one. Alpha-decision changes additionally count `alpha >= 0.5` disagreements.
The single-level probe's `pass` requires both the channel gate and zero such
disagreements; reports list these separately so this additional mask diagnostic
cannot be confused with the numerical gate. Each spatial candidate fails the
channel gate independently of its alpha diagnostic. A one-step alpha error can
also flip a discard decision and produce a much larger rendered difference.
No material rasterization or ray-query alpha test is claimed here.

These observations show that eliminating mip blending is insufficient to make
the tested spatial mapping pass. They do not establish a universally safe
alternative from one GPU, or prove that all spatial layouts are unsuitable.
Anisotropic spatial sampling, compressed pages, normals, authored UV transforms,
previews, runtime lifetime and complete scenes remain unverified.

The probe also records dedicated Vulkan image memory requirements, not production
VMA allocation or frame performance. These particular spatial fixtures do not
save allocation bytes. For example, the 127x255 subject plus its two neighbors
requires 245,760 bytes as separate images, versus 565,248 for the cropped page or
1,048,576 for the power-of-two page. This is a fixture result, not a workload-wide
profitability decision.

## Evidence and reproduction

Initial sources, executable, SPIR-V and both long logs are frozen under
`build/m6-single-level-initial/`. Final diagnostics, control, source snapshots,
binary and hashes are under `build/m6-single-level-evidence/`. Working logs and
the final summary are in `build/m6-atlas-sampling/single-level-*`.

From the standalone checkout, configure/build the existing probe with the MSVC
development environment and Vulkan SDK available:

```powershell
cmake -S tools/evaluation/atlas_sampling -B build/m6-atlas-sampling -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build/m6-atlas-sampling --parallel 2
```

The executable arguments are vertex SPIR-V, fragment SPIR-V, power-of-two padding
(0/1), manual-blend quantization (0/1), layout and anisotropy (0/1). Use the two
SPIR-V files in that build directory. Relevant argument suffixes are `0 0 4 0`
(cropped), `1 0 4 0` (padded), `1 0 5 0` (precise padded) and `0 0 6 0` (array
control). Frozen revision 2 runs 216 operations for each of these layouts; the
frozen initial executable reproduces the 1944-operation runs. Redirect stdout to
JSONL and stderr separately, and summarize completed logs with `summarize.py`.

## Tile-coordinate filtering and rounding-boundary follow-up

The next experiment retained the same source bytes and added three candidates:
four clamped integer texel fetches with shader bilinear blending (layout 7), the
same blending with weights rounded to the reported subtexel precision (layout
8), and original tile coordinates rounded before adding the page offset while
retaining hardware filtering (layout 9, power-of-two page). The shader fetches
use integer positions within the tile, avoiding selection of a neighboring tile.

The Vulkan specification defines linear weights from fractional texel
coordinates and associates their precision with `subTexelPrecisionBits`.
Round-to-even was tested as a candidate; that reported limit alone was not
treated as proof that the shader reproduces the device's native rounding and
filtering. See the [Khronos sampling specification](https://docs.vulkan.org/spec/latest/chapters/textures.html#textures-unnormalized-to-integer).

On the initial three grids, layout 9 meets the one-step channel gate across all
216 operations, but 24 operations still change alpha decisions. This apparent
numerical improvement is insufficient: revision 3 adds coordinate sets at
half-subtexel rounding boundaries and the immediately preceding/following
representable normalized coordinates. Each set spans both coordinate axes.
These grids are in addition to all three earlier sets; the reference input is
unchanged for every paired sample. All six grids are now used for single-level
layouts 4–9, with one explicit and one oblique-gradient operation per grid.

| Candidate, six coordinate sets | Operations | Channel-gate failures | Operations changing alpha decision | Largest byte difference |
| --- | ---: | ---: | ---: | ---: |
| Tile-coordinate shader bilinear | 432 | 156 | 200 | 3 |
| Tile-coordinate quantized shader bilinear | 432 | 118 | 176 | 3 |
| Tile-coordinate quantized native filtering | 432 | 52 | 88 | 2 |
| Production array page control | 432 | 0 | 0 | 0 |

All four runs complete with zero validation errors, empty stderr and the full
expected dimension/pattern/format/query matrix. The array control is exact for
all 7,188,912 logged paired samples. The quantized native candidate's newly
exposed failures are why it remains rejected even for the numerical gate.
Software blending does not improve that gate either. Do not infer a universally
matching hardware reconstruction or enable an opaque-only path from the earlier
three-grid result.

Initial layout-9 evidence is frozen under `build/m6-tile-native-initial/`. Final
four-run logs and summary are `build/m6-atlas-sampling/tile-*-boundaries*` and
`tile-boundaries-summary.json`; sources, executable, shader binaries, logs and
identities are preserved under `build/m6-tile-boundary-evidence/`. Reproduce the
four final runs with argument suffixes `0 0 7 0`, `0 0 8 0`, `1 0 9 0` and
`0 0 6 0`. Revision 3 runs 432 operations per single-level layout. These are
sampling diagnostics, not rendering or performance acceptance.

Keep spatial cooking and runtime metadata disabled until a sampling design and
all-use eligibility policy pass the original visual gate, including the new
coordinates. A cropped rectangle, a power-of-two page or a single-level-only
restriction is not sufficient evidence by itself. Subsequent work must retain
the mip/filter, material, compression and workload gates in the original plan.
