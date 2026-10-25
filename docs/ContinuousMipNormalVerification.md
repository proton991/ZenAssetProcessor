# Continuous LOD normal-map qualification

The processor now qualifies the complete mip-blend interval at each existing
UV sample phase. A confirmed BC5 counterexample passed all quarter-mip samples
but exceeded the unchanged **1 degree RMS / 5 degrees maximum** limits between
those samples. Uncertified candidates retain the complete authored image.
This checkpoint does not close continuous UV, anisotropic or device-precision
coverage, or the full M5 acceptance gate.

## Confirmed counterexample

An authored 8x8 image contains RGBA `(114,204,229,255)` at every texel; its 4x4,
2x2 and 1x1 levels contain `(103,135,252,255)`. The previous installed processor
accepts BC5 at the default angular limits. Its largest observed RMS is
0.998785569391 degrees. Independent decoding of the actual BC5 bytes gives
**1.093877333941 degrees** at coarser-mip weight **0.36572265625**. Every level is
spatially constant, so that angle is also the spatial RMS at every UV position.
The independent quarter-grid result is 0.998787449978 degrees; decoder rounding
does not account for the missed violation.

The frozen original probe, encoded bytes, independent decoder, commands and
hashes are in [the reproduction directory](../build/normal-continuous-audit/README.md).
The baseline SDK fingerprint is
`14d66cc26703f8a724c19f02a92033665a42c5f51b3407cb5f27fc7139d558f5`.

## Interval bound

At a fixed UV position, the reference signed RGB vector `a(t)` is affine in the
mip fraction. BC7's decoded signed RGB vector `b(t)` is also affine. For an
interval's endpoint vectors `a0,a1,b0,b1`, their cross product has quadratic
Bernstein controls:

- `a0 cross b0`
- `(a0 cross b1 + a1 cross b0) / 2`
- `a1 cross b1`

The dot product has the analogous three scalar controls. The Bernstein convex
hull bounds the cross-product norm above by the largest control-vector norm,
and the dot product below by its smallest control. With a positive dot lower
bound `D` and cross-norm upper bound `C`, their angle is at most `atan2(C,D)`.
A nonpositive lower bound leaves the interval at the conservative 180-degree
bound until subdivision or fallback.

Both affine segments must remain above the existing `1e-12` defined-direction
threshold. Their minimum lengths are obtained by projecting the origin onto
each segment, with outward slack. Equal endpoint curves can have zero error
only when the whole segment remains defined. This prevents an equal-curve
shortcut from hiding an undefined normal at a non-dyadic fraction.

For BC5, the stored XY channels are affine, while reconstructed positive Z is
`sqrt(max(0,1-q))`, with `q = x*x + y*y`. The qualifier uses the affine chord
between the reconstructed endpoint vectors and bounds its Z deviation.
Quadratic q has an exact minimum on the segment and a maximum at an endpoint.
Its outward-bounded range gives a general Z-deviation bound, including the cusp
at q=1. If the whole interval lies outside the disk, Z is identically zero.
Inside the disk, Z is concave and a tighter interpolation bound is `M/8`, where
`M = |d|^2/z_min + max(|xy dot d|)^2/z_min^3`; d is the XY difference over the
local interval parameter `[0,1]`. It is not multiplied by interval width again.
For chord deviation epsilon smaller than the minimum chord length L, the
angular deviation is at most `asin(epsilon/L)`. That deviation is added to the
affine cross/dot bound. Other cases retain the conservative 180-degree bound.

The implementation adds outward numeric slack to geometric bounds and rounds
positive squared-bound accumulation upward. These are bounds in the existing
CPU decoded-channel/filter model; hardware decoding and interpolation precision
are separate requirements.

## Traversal, diagnostics and ownership

Each interval observes its two endpoints and midpoint at the **same normalized
UV** in both mip levels, using the authored minification filter and wrap mode.
Coarse-level endpoint UV positions are therefore checked even when they differ
from that level's own texel-phase grid. Per-pixel angular upper bounds are squared
and accumulated into a separate RMS bound for each of the existing sixteen
UV phases. The maximum-angle bound spans every pixel and phase.

An interval is certified only when both bounds satisfy the unchanged limits.
An observed violation rejects the candidate. Otherwise the interval is split,
up to depth 12 and at most 127 interval scans per adjacent mip pair. Exhausted
work or a singular interval produces a distinct "could not certify" diagnostic
and retains all original pixels and authored mips. Partially qualified chains
cannot be published.

Private `TextureMipQuality` fields retain the largest **observed** sample RMS
and maximum angle separately from `normalMipRmsUpperBoundDegrees` and
`normalMipMaxUpperBoundDegrees`. Only certified leaf bounds contribute to the
latter fields; superseded parent bounds do not. `normalMipContinuousLodCertified`
is true only after the whole interval has been certified, and
`normalMipIntervalCount` records the bounded scan count. Bounds on a rejected
record are not a certificate for the entire interval.

Scratch remains width-bounded: two decoded rows per mip plus a fixed DFS stack
and per-phase sums. Both rows and fixed qualification storage are charged before
allocation; cancellation is checked on each scanned row. Existing failure
handling discards candidates and refunds temporary storage. No package schema,
public quality threshold, decoder setting or engine code changes.

## Verification

The frozen LOD checkpoint processor fingerprint is
`8dab1dee1728140f528ca7064591115d05933b894c9bae71f41dd459efad72a1`.

- All **37 Release** and **37 Debug CTest suites pass**, including expanded texture
  regressions. The sequential runs take 20.69 seconds and 431.59 seconds respectively;
  these suite durations are validation records, not before/after benchmarks.
- The confirmed BC5 chain now retains its authored representation. The adaptive
  samples observe about 1.093348 degrees RMS, rather than fabricating a value
  from an uncertified upper bound. Square, odd and one-dimensional extents pass.
- Qualifying BC5/BC7 chains remain compressed under nearest/linear texel filters
  and repeat/clamp combinations. Their certified bounds cover observations and
  satisfy the original thresholds.
- A BC7 fixture has exact equal decoded/reference RGB curves with signed
  endpoints `(3,3,3)/255` and `(-7,-7,-7)/255`. Although finite dyadic observations
  have zero error, the direction is undefined at 0.3. Bounded qualification
  correctly retains the original and reports an uncertified interval.
- Cancellation at every observed poll position covers both direct certification
  and adaptive subdivision. Resource-limit cases and accounting checks pass.
- An independently decoded installed-SDK campaign covers **80 cases**: forty
  authored constant-mip pairs using BC5 and BC7. It accepts 73 compressed cases
  and retains seven. Each accepted mip pair is checked at **16,388 LOD fractions**,
  including dense, offset and non-dyadic points. All accepted samples satisfy the
  unchanged 1-degree RMS threshold and reported upper bounds. A 0.00001-degree
  allowance applies only to the cross-decoder bound comparison, never the asset
  quality limit. See [batch evidence](../build/normal-continuous-audit/batch/README.md).

Focused sources, builds, installation and CTest logs are under
`build/normal-continuous-audit`. The `source` subdirectory freezes the tested
implementation, private header, tests and quality policy with SHA-256 identities
before the separate continuous-UV follow-up. The original failure reproduction
uses a frozen private header whose SHA-256 matches its pre-change manifest, so
later private metric-layout changes cannot change that baseline probe. The quality policy preserves the independent
auditors' finite sample grids while describing the stronger production LOD
qualification separately. Finite audits support the implementation checks;
they do not replace the interval-bound argument.

## Processing cost

A processor-owned copy of Khronos ClearCoatTest selects trilinear minification
(`minFilter=9987`) in every authored sampler. Both frozen public CLI versions
cook that identical source with the desktop profile, generated mips, BC7 and two
workers. Three paired runs alternate execution order after all CTests finish;
other agents suspend heavy builds/tests during timing. Source files are warm.
The host is Windows on an **AMD Ryzen 9 9950X3D 16-Core Processor**, using the same
MSVC Release toolchain and dependency build for both SDKs.

| Full-cook seconds | First | Second | Third | Median |
| --- | ---: | ---: | ---: | ---: |
| Previous fixed fractions | 21.467155 | 20.794209 | 20.758421 | 20.794209 |
| Continuous LOD qualification | 22.855606 | 22.587747 | 21.863496 | 22.587747 |

The measured median increase is **8.6%**. These are whole CLI cook times,
not isolated qualifier CPU time or cold-cache measurements. Every run succeeds;
all decoded resource payloads are identical between versions except build
metadata, and each version is deterministic across its three runs. Both CLI
hashes remain unchanged. This bounded workload does not establish shipping
throughput across other image sizes or materials. See
[benchmark results](../build/normal-continuous-audit/benchmark/results.json) and
the rerunnable `benchmark.py`, which creates a fresh output directory and never
edits the source asset.

Continuous UV between the sixteen phases, anisotropic filtering, device
precision, broad material/device acceptance and shipping throughput remain
open. No engine integration or engine test/build is included in this change.
