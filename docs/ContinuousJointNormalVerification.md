# Continuous joint UV and LOD normal qualification

The processor now qualifies simultaneous UV phase and mip fraction for each
adjacent normal-map mip pair. The previous checkpoint certified each mip's
continuous UV domain and continuous LOD at sixteen fixed UV phases separately.
An actual BC7 counterexample passed both certificates but exceeded the unchanged
**1 degree RMS / 5 degrees maximum** limits between their sampling planes.
The new joint qualifier closes that gap in the CPU decoded-channel model.

This checkpoint keeps hardware decoder/filter precision and anisotropic
sampling outside the claim. RMS is defined over the shared-phase fine-texel
population described below; it does not cover arbitrary independently chosen
UV populations. Per-level UV qualification remains active for bilinear
magnification and minification, including images without linear mip blending.

## Reproduced actual-codec defect

The frozen predecessor SDK is
`bd4084418dee69e76e49fecc0d6dcec61220ae3fd42b91cd93cac6fa0c376b42`.
The complete authored RGBA8 chain is:

- 16x1 fine mip: `(255,207,128,255)` everywhere.
- 8x1 coarse mip: `(0,0,127,255)` first, then `(0,255,127,255)` elsewhere.
- 4x1, 2x1 and 1x1 tails: `(0,0,127,255)` everywhere.

BC7 quality 4, normal-role bilinear minification, linear mip blending and clamp
addressing accepted this image. The first pair's old fixed-UV LOD RMS bound was
0.998671484646 degrees. Actual BC7 decoding changes the coarse first RGB to
`(0,0,128)` and the next to `(1,255,127)`.

At normalized U `0.08603941176470588`, V `0.5`, and coarser-mip weight `0.5`,
independent decoding measures **88.597528251056 degrees** error. The reference
normal length is `0.00008`, well above the existing `1e-12` defined-direction
threshold; this is introduced codec error at a defined sample. The same phase
across all sixteen fine cells gives **22.151067204670 degrees RMS**. The old
four X phases, sampled at 4,097 LOD fractions, stay below 1 degree RMS and
5 degrees maximum. The complete source chain is only 124 bytes.

The [frozen counterexample](../build/normal-joint-audit/README.md) retains exact
source generation, encoded payloads, SDK/header identities, independent
NumPy/Pillow decoding and rerun commands. A second exact-encoding fixture has
an undefined joint normal between sampled planes. It tests the singularity
contract but is not used as evidence of introduced visible error.

## Complete sampler-domain partition

Each global box contains phase U, phase V and mip fraction in `[0,1]`. At a
fine-level texel `(x,y)`, the normalized position is
`((x+0.5+phaseU)/fineWidth, (y+0.5+phaseV)/fineHeight)`. Every cell uses the same
phase and mip fraction for RMS. For repeat this covers a full period. For
clamp, the omitted left half-fine-texel strip is constant in both mip levels
and represented at the first phase; the extra right strip is likewise
constant. Mirrored addressing folds into this same pointwise domain.

Bilinear minification splits each cell's phase rectangle at coarse-level
texel-center boundaries. The fine linear taps and each coarse piece's taps
are fixed from its interior. Evaluating its continuous corner limits produces
eight trilinear stored-channel vectors, including both LOD endpoints. Odd
extents use the actual coarse/fine ratio; unchanged singleton axes use `1 -> 1`.
Small fractional coordinates avoid subtracting large texel indices. Calculated
cut endpoint fractions are clamped to their piece's `[0,1]` limits, with the
geometric bounds' outward slack covering floating-point roundoff.

Nearest minification has discontinuities in both levels. For each axis the
qualifier enumerates correlated fine/coarse tap pairs at every cut, each open
interval midpoint, and both exact endpoints, then takes the Cartesian product
of X and Y states. It unions both sides of numerical cut uncertainty and
never merges states approximately. Fine `7 -> 3` can have separate cuts at
phase `1/2` and `5/6`; fine `5 -> 2` can select a new coarse tap exactly at phase
1. These states are included. Nearest UV states are constant, so each uses the
reviewed one-dimensional continuous-LOD bound.

## Conservative tensor bounds

For a linear sampler piece, the reference signed RGB vector and BC7's decoded
signed RGB vector are trilinear in local `(U,V,t)`. Their cross and dot products
have tensor Bernstein degree `(2,2,2)`: 27 controls formed from 64 corner pairs.
For each axis, corner indices `i,j` contribute weight `1 / C(2,i+j)`; the full
weight is the product over the three axes. The largest cross-control norm
bounds the cross norm above, and the smallest dot control bounds the dot below.
With a positive dot lower bound D and cross upper bound C, angle is at most
`atan2(C,D)`. Otherwise the conservative bound is 180 degrees until subdivision.

A multi-affine vector remains in the component-wise box of its corners.
Distance from that box to zero bounds the minimum vector length. Both reference
and decoded vectors must stay above `1e-12`, including outward numerical slack,
before an equal-control shortcut or angular bound may certify the piece.
An exactly decoded curve can still be undefined between observations.

BC5 filters stored XY before reconstructing positive Z. XY is multi-affine;
its squared norm q is bounded above by the largest corner squared norm and
below by the component-box distance to zero. The resulting Z range bounds
error against the trilinear interpolation of reconstructed corner Z, including
the reconstruction cusp at q=1. If the whole box is outside the disk, Z is zero.

When the outward q upper bound is below 1, the Hessian norm of
`sqrt(1 - |XY|^2)` is at most `1 / Zmin^3`. Pure second derivatives of XY vanish.
For each axis, its XY derivative is bilinear in the other two axes, so its norm
is bounded by the largest of all four parallel edge deltas. Sequential tensor
interpolation then bounds Z deviation by
`sum(axis maximum squared edge delta) / (8 * Zmin^3)`.
The tighter of this curvature bound and the general range bound is used.
For deviation epsilon below the minimum interpolated-vector length L, the
additional angular error is at most `asin(epsilon/L)`; otherwise the bound
remains 180 degrees. The shared two-/three-dimensional helpers also retain the
prior within-level UV behavior.

The implementation uses outward slack for geometry, dot/cross arithmetic,
Z-range branches and length comparisons, and rounds positive squared-bound
accumulation upward. These bounds apply to the processor's CPU decoded-channel
and interpolation model; finite audit tolerances do not relax quality limits.

## Aggregation, bounded work and diagnostics

For every fine cell, the maximum bound over all its sampler pieces or states
covers its whole global box. Summing those per-cell squared upper bounds and
dividing by the number of fine cells gives a conservative RMS bound at every
common phase and mip fraction in that box. The largest per-cell bound covers
pointwise maximum error. This preserves a separate RMS gate; it does not demand
that every pixel individually meet the RMS threshold.

Each box also observes the 27 shared endpoint/midpoint coordinates. Only these
common-phase populations contribute to observed RMS. Per-cell sampler cuts
differ for odd extents and therefore contribute bounds, not fabricated observed
RMS values. Observations and certified upper bounds remain separate.

Binary depth-first subdivision certifies a leaf only when both bounds meet the
original limits. Actual observed violations reject immediately. Otherwise
channel variation chooses the split axis; it is only a scheduling heuristic,
never evidence of successful qualification. Work is bounded to depth 12 and
127 evaluated boxes per adjacent pair. Exhaustion, singularity or an unresolved
bound retains the complete original image and every authored mip. No partial
certificate authorizes compressed output.

Private per-mip diagnostics store the joint pair on its coarser level:

- `normalMipRmsAngleDegrees` and `normalMipMaxAngleDegrees`: observed joint error.
- `normalMipRmsUpperBoundDegrees` and `normalMipMaxUpperBoundDegrees`: maxima of
  certified leaf bounds, also covering reported observations.
- `normalJointBoxCount`: evaluated global boxes, at most 127 per pair.
- `normalJointContinuousCertified`: true only after every box is certified.

The former private fixed-UV LOD certificate/count fields were renamed. A
rejected record can contain partial leaf metrics; its false certificate flag
is authoritative. Diagnostics distinguish `sampling exceeds normal angular
limits` from `sampling could not certify normal angular limits`.

Scratch consists of two decoded rows per mip, one upper-bound row at fine
width, and fixed-size boxes/control arrays. Charges occur before allocation;
temporary charges are refunded. Cancellation polls occur during observations
and piece/state traversal, clear partial metrics/output, and refund ownership.

## Verification checkpoint

The new frozen Release SDK is `build/normal-joint-audit/install`, fingerprint
`aa21c824877d080bc4c7746498a84ffa73c18456005ab2311f247b6edb720ebd`.
Matching source/private header/tests are under `build/normal-joint-audit/source`;
the baseline source remains in `source-baseline`, and prior 14d/8dab/bd408 SDKs
and evidence remain unchanged.

Focused texture regressions pass. They verify the known actual BC7 decoded
witness above 88 degrees, preservation of its original mip chain, distinct
observed-versus-uncertified diagnostics, the exact-encoding joint singularity,
56 non-flat codec/filter/wrap/odd/1D controls, accepted joint bounds, cancellation
at every poll of linear and adaptive nearest controls, and budget refunds.
All 37 Release CTests pass in 25.41 seconds and all 37 Debug CTests pass in
515.53 seconds. The full logs are `release-ctest.log` and `debug-ctest.log` in
the checkpoint directory; clang-format dry-run passes for all three C++ files.

The independent installed-SDK audit passes **240 cases**: 201 retain compression
and 39 retain their authored data. It checks 442 accepted adjacent mip pairs at
**94,205,195 actual decoded joint samples**, including 60 lossy BC5 and 45 lossy
BC7 cases. The fixtures cover flat, mild and spatially varying chains across
1x1, 1x9, 9x1, 5x3, 7x5 and 8x8 extents, plus the known 16x1 witness and its
transpose, with nearest/bilinear filtering and clamp/repeat/mixed addressing.

Accepted sampled errors and the reported certified bounds themselves satisfy
the unchanged 1-degree RMS / 5-degree maximum limits. Across this campaign the
largest sampled RMS is 0.809089 degrees and maximum is 1.466487 degrees; the
largest reported bounds are 0.999802 degrees RMS and 3.674829 degrees maximum.
Bound comparisons alone allow 0.00001 degrees for independent float64 decoding versus the production
float decoder; quality thresholds receive no tolerance. Both witness
orientations reject specifically because joint qualification cannot certify
them within bounded work. The observed samples are not misreported as the
independent 88-degree witness. The [independent results and rerun sources](../build/normal-joint-audit/independent/results.json)
retain actual payloads, source hashes, metrics, exact commands and assertions.
Finite audits supplement the conservative bound; they do not establish
continuity by themselves.

A separate [rerunnable sampler audit](../build/normal-joint-audit/review/README.md)
compared 30,730 axis cases to exact-rational
nearest tap states, including extents 1-65, selected odd/even extents through
1,025 and UINT32_MAX, interior and depth-12 boxes, clamp and repeat. It found no
omitted state, at most four cuts and at most three guarded correlated states.
This small mathematical review supports the sampler partition, not a hardware
precision claim.

## Paired cooking cost

Three alternating-order pairs compare the frozen separately qualified UV/LOD
SDK (`bd408...`) with the joint-qualified SDK (`aa21...`). Both use an identical
copy of ClearCoatTest with authored trilinear minification,
`--workers 2 --profile desktop --texture-mipmaps on --texture-compression bc7`.
Every invocation writes a fresh package. Other agents held builds, tests and
heavy experiments throughout this timing window.

| Processor | Median full cook | Observed range |
| --- | ---: | ---: |
| Separate UV and fixed-UV LOD qualification | 31.757377 s | 30.956817-31.940060 s |
| Joint UV/LOD qualification | 44.295870 s | 36.290593-44.363637 s |

The median increase is **39.482%** for this fixture. These are full-cook
wall-clock measurements with warm source files, not isolated qualifier CPU
measurements or a general throughput claim. Three pairs and the observed
variation limit the precision of the cost conclusion.

All resource payloads are identical except metadata; no image resource changes.
Both outputs contain four BC7 images and two RGBA images, totaling 5,330,368 GPU
mip bytes. Fallback diagnostics are unchanged: image 0 preserves its incomplete
chain for exact alpha, and image 3 exceeds the existing normal-angle limit.
The comparison therefore does not trade quality work for an earlier fallback
or reduced texture output. Both binaries retain their recorded SHA-256 hashes.

The [paired results](../build/normal-joint-audit/benchmark/results.json) preserve
exact commands, source and executable identities, timings, per-image summaries,
diagnostics and resource comparisons. `build/normal-joint-audit/benchmark.py`
is the retained runner; it refuses to overwrite an existing evidence directory.
The [checkpoint identity record](../build/normal-joint-audit/checkpoint-identities.json)
identifies the matching source, private header, test executables and SDK libraries.

This completes the joint CPU filtering certificate and its bounded regression
and cost evidence. Device-precision, anisotropic and rendered-frame qualification
remain separate M5 work.
