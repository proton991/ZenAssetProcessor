# Atlas cooking and publication measurements

Verified 25 October 2026 against the frozen installed SDK at
`build/normal-uv-audit/install`, fingerprint
`bd4084418dee69e76e49fecc0d6dcec61220ae3fd42b91cd93cac6fa0c376b42`.

All four matched off/on scenarios passed seven measured repetitions: **56 timed
processes**, plus eight warmups. All **64 warmup/measured packages** were
byte-identical to their validated expected package and passed full-scene readback
comparisons in separate processes. Actual page creation was required.

This fills the processor-side repeated atlas cooking/publication measurement
gap for these original synthetic workloads. It does not establish representative
production workload profitability, runtime draw/frame benefit or overall M6
completion. Later processor changes are outside this checkpoint.

## Original workloads and matched policy

`tools/evaluation/atlas_measurements/main.cpp` constructs original deterministic
canonical scenes. Each has 36 distinct RGBA8 sRGB images, 36 logical textures
and materials, 36 indexed primitives sharing one triangle, and two instances of
the shared mesh. Every image is used. Alternate material bindings carry authored
UV scale/offsets whose effective coordinates remain inside the eligible domain.

Each case retains its original generator recipe as `source.json`, also stored
in the scene's source document, and its exact canonical `input.zenasset`.
The recipe records image count, dimensions, mip policy, pixel formula, geometry
and UV convention. Source snapshots and hashes identify the generator and
comparator. The fixtures are authored resource workloads, not imported natural
scenes or a new image-quality corpus.

| Scenario | Image extent | Authored levels | Address/filter policy | Off images | On pages |
| --- | --- | --- | --- | ---: | ---: |
| Array 128 | 128 x 128 | Complete chain, 8 levels | Clamp, linear/trilinear | 36 | 3 arrays of 12 layers |
| Array 512 | 512 x 512 | Complete chain, 10 levels | Clamp, linear/trilinear | 36 | 3 arrays of 12 layers |
| Spatial 128 | 128 x 128 | One level | Clamp, linear, mipmapping disabled | 36 | 1 spatial page |
| Spatial 512 | 512 x 512 | One level | Clamp, linear, mipmapping disabled | 36 | 4 spatial pages |

The array layer limit is 12; page dimensions are bounded by 2048. Other settings
remain the frozen SDK defaults. All 36 spatial bindings receive regions; all
array bindings select actual layers. No image is excluded and no diagnostic is
produced. Logical texture/material counts remain 36.

Within each off/on pair, the input package, pixel bytes, authored mips, sampler
policy, transforms and geometry are identical. Both use Zstandard level 3,
mesh encoding disabled and ordinary non-durable `WriteScene` publication.
There is no BC encoding, mip generation, geometry optimization or quantization.
Only the active atlas flag changes. Inactive option values, including the array
layer limit, are the same within each pair.

Array and spatial scenarios are separate comparisons. Their different authored
mip policies must not be compared as an atlas saving. Mip pixels are prescribed
by the deterministic fixture formula; this experiment does not qualify their
filtering or visual quality.

## Correctness and determinism

Preflight checks validate each input, cook both modes twice, compare repeated
cooks for full public-scene equality, and require the expected page counts.
Checks compare every preserved non-image scene field, preserve authored texture
fields apart from image/layer/region selection, and compare every logical array
mip or spatial tile interior against the original pixels. The original scene is
compared against its saved input after cooking to detect mutation.

Each of the eight expected outputs is written and read through the installed
SDK, then compared with `tests/SceneValueComparison.h`, including its processing
settings and compression metadata. This comparator inspects public records
independently of the serializer's field visitor.

Each measured/warmup output has the exact SHA-256 of its expected package.
After all timed runs, separate verification processes read and fully compare
all 64 outputs against those validated scenes. Comparison references and
output-read scenes are never allocated in the timed processes.

These checks establish that the measurements exercise deterministic, eligible
packing without changing the authored logical bytes. Existing
[shared cooking tests](../tests/ArrayProcessingTests.cpp),
[array documentation](ArrayCooking.md) and [spatial documentation](SpatialCooking.md)
remain the broader eligibility, exclusion, gutter, failure and sampling evidence.

## Timing and memory method

Each fresh measured process reads one canonical input, calls `CookScene` once
and calls `WriteScene` once. Separate clocks surround the input read, cooking
and writing calls. `WriteScene` timing includes serialization, Zstandard
encoding, temporary-file output and publication; it is not isolated encoding,
disk-transfer or durable-storage latency. The output is not read in that process.

`CookScene` off uses the normal disabled-processing path, which may share the
input owner. Atlas on constructs new page storage. The input scene remains alive
through writing in both cases. Absolute lifetime memory therefore includes
input loading, retained input, cooking output, writer scratch and runtime
overhead. It is not per-stage peak memory, a minimum achievable application
footprint, GPU memory or allocation-budget usage. A caller that releases its
input before writing can have a different lifetime footprint.

The runner reuses the existing Windows process-measurement helpers. It records
OS lifetime peak working set and private commit with 1 ms polling and a final
query, whole-process elapsed time, and kernel-plus-user CPU time. The latter
is coarse for short runs. Every sample and min/median/max statistic is retained.

Each scenario/mode has one separate warmup and seven measured fresh processes.
Scenarios rotate by round; off/on order reverses every round. OS file caches
are warm from fixture creation, preflight and warmup; no engine asset cache is
involved. OS caches are not flushed, CPU clocks and background host activity
are uncontrolled, and no physical durability guarantee is requested. Other
agent builds and heavy experiments were held during the measurement window.

A no-operation baseline from the same executable runs once per round. Its
median process duration is 4.21 ms, peak working set 4.51 MiB and peak private
commit 1.05 MiB. Baseline values are not subtracted from the reported samples.

Host/toolchain: AMD Ryzen 9 9950X3D, 16 cores/32 logical CPUs, Windows 11 build
26300; MSVC 19.51.36256.0 x64 Release /O2 /MD, CMake 4.4.2, Ninja 1.13.2,
Python 3.14.7. The probe compiles against the installed SDK; only the existing
test-comparator include comes from the checkout. Actual compile/link commands
contain no ZenEngine or dependency-build paths. clang-format 22.1.3 passes.

## Results

Stage times and absolute process peaks below are medians of seven runs.
One MiB is 1,048,576 bytes.

| Scenario | Atlas | Cook ms | Write ms | Package bytes | Peak working set MiB | Peak private commit MiB |
| --- | --- | ---: | ---: | ---: | ---: | ---: |
| Array 128 | Off | 0.066 | 11.117 | 183,719 | 12.00 | 8.36 |
| Array 128 | On | 0.793 | 6.382 | 178,887 | 22.75 | 19.04 |
| Array 512 | Off | 1.144 | 89.837 | 2,669,478 | 97.50 | 94.09 |
| Array 512 | On | 13.474 | 112.704 | 4,559,702 | 233.04 | 229.76 |
| Spatial 128 | Off | 0.058 | 10.166 | 138,373 | 11.10 | 7.54 |
| Spatial 128 | On | 1.372 | 6.955 | 227,029 | 37.14 | 33.43 |
| Spatial 512 | Off | 1.134 | 76.924 | 2,029,509 | 84.58 | 81.08 |
| Spatial 512 | On | 21.581 | 104.410 | 3,497,893 | 272.77 | 269.53 |

The array cases preserve exactly the same retained image-vector bytes off/on:
5,504,976 at 128 and 88,080,336 at 512. These totals include both base storage
and explicit mip vectors, including the duplicated base level. Image-count
reduction does not reduce those pixel bytes.

Spatial gutters and power-of-two page padding increase retained image-vector
bytes from 4,718,592 to 8,388,608 at 128, and from 75,497,472 to 134,217,728 at
512. Logical source pixels remain unchanged; the extra bytes belong to page
construction and the duplicated base/explicit-level representation.

At 128, fewer image resources correspond to shorter observed writing times,
despite added cooking cost. At 512, both packing modes take longer to cook/write
and produce larger compressed packages. Array packing can change Zstandard
results even with identical total pixel bytes because image/chunk layout changes;
this campaign does not isolate an encoder-internal cause.

These results do not justify an always-on policy. They quantify processor
costs and local file-size effects for the stated patterns and resource counts.
They provide no new renderer binding, sampler, draw-call, upload, frame-time,
GPU-allocation, quality or real-scene profitability result.

## Evidence and reproduction

Evidence lives under `build/m6-atlas-measurements/`:

- `final/results.json`: exact invocation, cases, settings hashes, actual page and
  byte counts, packages, every sample, execution order, summaries and correctness
  commands. `passed` and `identities_unchanged` are true.
- `final/sdk-identities.json`: SHA-256 of every installed SDK file.
- `final/{array,spatial}-{128,512}/`: original recipe/input package, both
  expected packages, eight output packages per mode.
- `final/logs/`: four preflights, baseline/warmups, all 56 measured runs and
  eight postflight logs, each verifying eight packages.
- `final/sources/` and `final/atlas_measurements.exe`: frozen generator/probe,
  comparator, runner/helper sources and the measured executable.
- `final/build-evidence/`, `final/toolchain.json`: actual configuration,
  compiler, compile/link commands and host identity.
- `configure.log`, `build.log`, `format-check.log`, `campaign.log`: successful
  build/formatting and complete campaign progress.

From an MSVC x64 developer shell:

```powershell
Set-Location E:/Dev/ZenAssetProcessor
cmake -S tools/evaluation/atlas_measurements -B build/m6-atlas-measurements/build -G Ninja `
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH=E:/Dev/ZenAssetProcessor/build/normal-uv-audit/install `
  -DCMAKE_FIND_USE_PACKAGE_REGISTRY=OFF -DCMAKE_FIND_USE_SYSTEM_PACKAGE_REGISTRY=OFF `
  -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
cmake --build build/m6-atlas-measurements/build --parallel 2
python -B tools/validation/asset_processor_atlas_measurements.py `
  --probe build/m6-atlas-measurements/build/atlas_measurements.exe `
  --sdk build/normal-uv-audit/install `
  --fingerprint bd4084418dee69e76e49fecc0d6dcec61220ae3fd42b91cd93cac6fa0c376b42 `
  --output build/m6-atlas-measurements/reproduction --runs 7
```

Use a new output directory. A different source-path provenance string can slightly
change package sizes across directories. Change the explicit fingerprint only
when intentionally verifying another SDK, without relabeling this checkpoint.
No core processor or engine source was modified, and no engine build was run.

