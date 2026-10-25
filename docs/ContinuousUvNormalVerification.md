# Continuous UV normal-map qualification

The processor now qualifies all ideal bilinear UV phases within each mip level.
A confirmed BC7 image passed the quarter-grid checks but exceeded the unchanged
**1 degree RMS / 5 degrees maximum** limits between those phases. The new
qualifier retains the complete authored image when it observes a violation or
cannot certify the limits within its work budget.

This certificate covers within-level bilinear sampling. The separate
[continuous LOD checkpoint](ContinuousMipNormalVerification.md) covers all mip
fractions at its existing sixteen UV phases. Those two certificates do **not**
prove combined continuous UV and continuous LOD. Hardware decoding/filtering
precision, anisotropy, and full rendered M5 acceptance remain separate work.

## Confirmed counterexample

The source is an original 4x4 RGBA8 image with identical rows and alternating
columns `(255,128,128,255)` and `(64,128,128,255)`. Both stored directions have
positive Z. Their lengths differ; the existing normal metric normalizes the
filtered vector, and no unit-length source requirement is introduced here.

The frozen SDK fingerprint
`8dab1dee1728140f528ca7064591115d05933b894c9bae71f41dd459efad72a1`
accepts the image at BC7 quality 4 with repeat addressing, bilinear filtering,
no mipmaps, and the default angular limits. Its quarter-grid RMS is
0.02856849227240727 degrees and maximum is 0.04011309664192082 degrees.
The emitted BC7 block is:

```text
c0 3f 08 08 04 02 ff 7f f0 f0 f0 f0 f0 f0 f0 f0
```

The production decoder and independent Pillow decoder both restore the first
color as `(254,128,128)` and preserve the second. At the exact horizontal
phase **2/3**, independent ideal bilinear sampling gives **18.756256882231657
degrees RMS / 26.525352016609705 degrees maximum**. One normalized-UV witness
is `(0.29166666666666663, 0.15925)`; identical rows make y phase irrelevant.
A 32,771-point dense/offset scan also finds a nearby 26.526164-degree maximum,
but the exact 2/3 witness already establishes the missed violation.

The [reproduction directory](../build/normal-uv-audit/README.md) contains the
source bytes, actual encoder output and settings, independent Pillow/NumPy
script, decoded values, SHA256 hashes, and both installed-SDK probe records.
The new SDK keeps the original after ten UV rectangles: its adaptive samples
observe 2.231697 degrees RMS while the old quarter-grid metrics remain unchanged.
It does not report the independent peak as a production observation.

## Bilinear direction bound

For one texel cell, signed reference RGB and BC7 decoded RGB are bilinear
patches with four corner vectors `a_ij` and `b_kl`. Restricting to any UV
subrectangle produces another bilinear patch with its four restricted corners.
Products of the two patches have tensor Bernstein degree `(2,2)`. Their nine
cross-product controls accumulate all sixteen corner pairs with weight:

```text
1 / (C(2, i+k) * C(2, j+l))
```

The dot-product controls use the same weights. The Bernstein convex hull bounds
the cross norm above by the largest control-vector norm and the dot product
below by its smallest scalar control. For positive dot lower bound `D`, the
angle is bounded by `atan2(C,D)`, where `C` is the cross-norm upper bound.
Otherwise the rectangle keeps the conservative 180-degree bound.

A bilinear vector remains in its corners' component-wise box. Distance from
that box to the origin is a conservative vector-length lower bound. Both
reference and decoded patches must remain above the existing `1e-12`
defined-direction threshold. Even exactly equal patches can use a zero-angle
shortcut only after this check; equal curves can still cross zero between
sampled positions. Subdivision tightens the component boxes.

## BC5 reconstruction bound

BC5's signed XY patch is bilinear; positive Z is reconstructed as
`sqrt(max(0,1-q))`, with `q=x*x+y*y`. The qualifier compares the reference patch
to the bilinear patch through reconstructed corner vectors, then bounds the
reconstruction's Z deviation from that corner patch.

The XY component box gives a conservative q minimum. Convexity gives q maximum
at most the largest squared corner length. Their outward-bounded range gives
`z_min` and `z_max`. With `zc_min` and `zc_max` the reconstructed corner-Z
extrema, a general deviation bound is:

```text
max(z_max - zc_min, zc_max - z_min)
```

This remains valid across the reconstruction cusp at q=1. If the entire patch
is outside the unit disk, Z is identically zero. When the entire patch is
strictly inside, a tighter bound uses the Hessian of `sqrt(1-|xy|^2)`, whose
spectral norm is `1/z^3`. Bilinear XY has zero pure second derivatives; each
local edge derivative's norm is bounded by its two opposite-edge norms.
Applying one-dimensional interpolation error in each axis gives:

```text
epsilon <= (max |du_edge|^2 + max |dv_edge|^2) / (8 * z_min^3)
```

The tensor decomposition is `(F-Iu F) + Iu(F-Iv F)`, and interpolation is a
convex combination. The edge deltas already use the local subrectangle's
`[0,1]^2` coordinates; no second interval-width factor is applied. The
implementation uses the smaller valid range/curvature bound with outward
numeric slack.

If the deviation epsilon is smaller than the minimum length L of the
bilinear reconstructed-corner patch, its direction changes by at most
`asin(epsilon/L)`. This is added to the bilinear cross/dot angle bound. Other
cases retain the 180-degree bound until subdivision or fallback.

## Traversal, metrics, and ownership

Each rectangle covers a common UV phase range across every texel cell, including
repeat seams or duplicated clamp neighbors. A 3x3 endpoint/midpoint phase grid
provides actual observations. Per-cell angular upper bounds are squared and
summed with upward rounding, so the resulting RMS bounds every common phase
inside that rectangle. Maximum-angle bounds cover every cell and phase.
Only certified leaf bounds contribute to the final certificate.

The original quarter-grid fields `normalFilteredRmsAngleDegrees` and
`normalFilteredMaxAngleDegrees` remain unchanged. New private diagnostics are:

- `normalUvObservedRmsAngleDegrees` and `normalUvObservedMaxAngleDegrees` for
  adaptive observations.
- `normalUvRmsUpperBoundDegrees` and `normalUvMaxUpperBoundDegrees` for certified
  leaf bounds. Partial bounds from a rejected image are not a certificate.
- `normalUvRectangleCount` and `normalUvContinuousCertified` for work and
  completion status.

The qualifier runs only after existing texel/quarter-grid gates pass. It splits
the longer UV side, uses a fixed DFS stack, and stops at depth 12 or 127
rectangle scans per mip. An observed violation and an exhausted certification
budget have distinct diagnostics. Neither publishes a partially qualified image.
Existing ownership handling retains all authored pixels/mips on quality fallback
and discards candidates on errors or cancellation.

Scratch uses two decoded rows plus budgeted fixed traversal/accumulation state.
Cancellation is checked on every scanned row. No public threshold, package
schema, codec policy, or engine code changes.

## Verification checkpoint

The current UV SDK fingerprint is
`bd4084418dee69e76e49fecc0d6dcec61220ae3fd42b91cd93cac6fa0c376b42`.
It is installed separately under `build/normal-uv-audit/install`; the prior
continuous-LOD SDK and its evidence remain frozen.

- All 37 Release and all 37 Debug CTest suites pass. Final focused texture
  suites in both configurations also pass after adding the reconstruction-cusp
  and work-cap regression. Full suite logs are preserved separately from those
  final focused reruns.
- The confirmed BC7 witness now retains its original pixels under repeat and
  clamp addressing, while preserving its old quarter-grid diagnostics.
- Non-flat BC5 and BC7 controls qualify for square, odd, and one-dimensional
  extents, including repeat/clamp edges. Their certified bounds cover observed
  errors and satisfy the unchanged thresholds.
- An exactly decoded BC7 patch crosses an undefined normal at a non-dyadic UV
  location. Bounded qualification rejects it without fabricating an observed
  angular error.
- A BC5 block with one `(255,128,143,255)` texel and fifteen
  `(255,128,128,255)` texels demonstrably crosses q=1 after decoding. Its sampled
  errors remain below the limits, but conservative qualification exhausts 127
  rectangles and retains the original with an uncertified diagnostic. This is
  not a proof that its full continuum satisfies the limits.
- Cancellation covers every poll for a directly certified control and selected
  mid/final polls during subdivided work. Resource-limit and budget-refund checks
  pass.

- The independent actual-payload audit passes all 54 nonconstant 4x4 cases:
  32 remain compressed and 22 retain their source, including the known BC7
  witness. Ten accepted BC7 cases have nonzero compression error. Each accepted
  image is checked at 10,201 UV phase pairs across all sixteen texels using the
  independent BC5 endpoint decoder or Pillow BC7 decoder. All sampled RMS/max
  errors satisfy the unchanged quality limits. Bound comparisons alone allow
  0.00001 degrees for the independent float64 endpoint decoder versus the
  production float decoder; the quality thresholds are not relaxed. See the
  [independent audit results](../build/normal-uv-audit/independent/results.json).
  This audit models ideal bilinear repeat sampling, not hardware precision,
  anisotropy, or combined continuous UV and LOD.

A finite audit is implementation evidence; the rectangle bounds supply the
within-level continuity argument. Joint UV+LOD qualification remains unclaimed.

## Paired cooking cost

Three alternating-order pairs compare the frozen LOD-only SDK (`8dab...`) with
the UV-qualified SDK (`bd408...`). Both use an identical copy of ClearCoatTest
with authored trilinear minification, `--workers 2 --profile desktop`, mipmaps
on, and BC7 compression. Each invocation creates a fresh package. No competing
agent build, test, or heavy experiment ran during the timing window.

| Processor | Median full cook | Observed range |
| --- | ---: | ---: |
| LOD-only | 24.981957 s | 22.803642–26.958285 s |
| UV-qualified | 28.864109 s | 26.409037–29.440580 s |

The median increase is **15.54%** for this fixture. These are full-cook wall-clock
measurements with warm source files, not an isolated qualifier profile or
cold-cache claim. Three pairs establish a bounded cost observation, not a
general throughput guarantee.

Every resource payload is identical except metadata; no image resource changes.
Both outputs contain four BC7 images and two RGBA images, totaling 5,330,368 GPU
mip bytes. Fallback diagnostics also remain identical: image 0 preserves its
incomplete chain for exact alpha, and image 3 exceeds the existing normal-angle
limit. The timing change therefore does not reflect earlier UV fallback or
reduced texture work on this fixture. Binary hashes remain unchanged throughout
all six runs.

The [paired results](../build/normal-uv-audit/benchmark/results.json) preserve
all commands, run timings, executable/source hashes, output image summaries,
resource-difference records, and diagnostic messages. The script and logs live
under `build/normal-uv-audit`; prior LOD-only evidence is not overwritten.
