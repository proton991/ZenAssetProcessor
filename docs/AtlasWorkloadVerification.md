# Natural-asset atlas workload verification

Checkpoint: 25 October 2026, frozen installed SDK at
`build/normal-uv-audit/install`, fingerprint
`bd4084418dee69e76e49fecc0d6dcec61220ae3fd42b91cd93cac6fa0c376b42`.

Preflight passed for four imported assets and both array/spatial profiles.
DiffuseTransmissionPlant creates actual pages in both profiles. The other six
branches produce no pages and are retained as eligibility evidence only.
Both eligible profiles passed seven alternating off/on rounds: **28 measured
processes**, four warmups and **32 full postflight package comparisons**. All
outputs match their validated expected package SHA-256. Recorded original/copy,
SDK, probe, helper-source and canonical-input identities remain unchanged.

## Sources and matched settings

The fixed inputs are the plain `glTF/<Model>.gltf` variants from the existing
`E:/Dev/glTF-Sample-Assets/Models` directory. Original glTF files and their
referenced buffers/images are copied byte-for-byte into the audit output.
The original files, samplers, UVs and texture transforms are not edited to make
packing eligible. Original/copy SHA-256 values identify every source file; the
local source directory has no Git metadata. All imports use two workers and
complete without diagnostics.

| Model | Physical source files | Source bytes | Imported images / textures / materials |
| --- | ---: | ---: | ---: |
| ChronographWatch | 10 | 7,469,250 | 8 / 8 / 29 |
| PlaysetLightTest | 21 | 16,444,152 | 19 / 19 / 6 |
| DiffuseTransmissionPlant | 9 | 5,780,668 | 7 / 7 / 3 |
| USDShaderBallForGltf | 6 | 1,325,973 | 4 / 4 / 3 |

The source README files are archived with hashes under `final/provenance/`.
They retain the complete attributions: Watch credits Darmstadt Graphics Group,
graphiccompressor and the included logo owners; Plant credits Darmstadt
Graphics Group; ShaderBall credits Eric Chadwick. Their stated asset licenses
include CC BY 4.0, with separate logo terms in Watch. Playset is an IKEA asset
under CC BY-NC-SA 4.0. These are local verification inputs, not new redistributed
processor fixtures.

Array comparisons first run a common, role-aware `CookScene` with
`generateMipmaps=true` and all atlas processing disabled. The resulting
canonical package, including retained-image decisions, is the identical input
to both timed modes. Each timed call performs page processing only; it does not
repeat mip generation. Common preparation is reported separately below.

Spatial comparisons use the imported authored image/mip state, with mip
generation disabled. Preflight checks full equality between that input and
the disabled common cook. Both profiles preserve authored sampler and UV state;
unspecified glTF filter fields are resolved by the importer, not patched by this
probe. Array and spatial policies are separate comparisons, so differences
between them are not atlas savings.

Within each off/on pair, only its atlas flag changes. RGBA8 pixels stay
uncompressed by a GPU block codec; geometry optimization, quantization and mesh
encoding are disabled. Both outputs use Zstandard level 3 and ordinary
non-durable `WriteScene` publication. Defaults remain 2048 maximum page extent,
256 maximum array layers, 64 pages and 8192 candidate images. Spatial pages use
the supported four-texel gutter profile. No limits are enlarged to force pages.

## Common array preparation and retained images

These are single preflight observations, not repeated timing estimates. Source
import and common mip preparation are excluded from the later page-stage times.

| Model | Import ms | Common array preparation ms | Prepared images / textures | Common outcome |
| --- | ---: | ---: | ---: | --- |
| ChronographWatch | 74.633 | 370.264 | 9 / 9 | Role split; non-unit or animated normal scale retains authored image/mip behavior |
| PlaysetLightTest | 169.794 | 1,626.339 | 19 / 19 | No common diagnostic |
| DiffuseTransmissionPlant | 79.329 | 272.747 | 7 / 8 | Binding split; image 0 retains its original incomplete mip chain |
| USDShaderBallForGltf | 11.256 | 14.192 | 4 / 4 | No common diagnostic |

Plant's image 0 produces two explicit common diagnostics: exact alpha requires
preserving the original incomplete chain, and a generated mip cannot preserve
alpha-mask coverage. That image remains unchanged and is excluded from arrays;
other compatible images still form pages. Watch's normal-scale diagnostic is
also retained verbatim with its object ID in `results.json` and preparation logs.
A successful common cook is not a claim that every image acquired full mips.

Plant's exact canonical input identities are:

- Array: 63,650,906 bytes; SHA-256
  `7d2bd0c1d9292efdd4df10afba44be0c23b25af3d8272853e97d5b2ef3522777`.
- Spatial: 34,290,202 bytes; SHA-256
  `fdf393115f4de14af59b1f57a0635d839aa1070db16949a3a563cc9904f463d8`.

## Actual page creation and exclusions

Image counts include both packed pages and retained standalone images.
Exclusion counts below summarize the emitted first reason per candidate image;
they are not an exhaustive list of every eligibility condition each image fails.

| Asset / profile | Images off â†’ on | Pages / mapped textures | Package bytes off â†’ on | Reported exclusions |
| --- | ---: | ---: | ---: | --- |
| Watch / array | 9 â†’ 9 | 0 / 0 | 19,079,278 â†’ 19,079,278 | 4 UV-domain, 1 sampler, 1 missing finalized mips, 3 without compatible partner |
| Watch / spatial | 8 â†’ 8 | 0 / 0 | 9,886,030 â†’ 9,886,030 | 1 sampler, 4 implicit-mip semantics, 3 tile/gutter dimension limits |
| Playset / array | 19 â†’ 19 | 0 / 0 | 50,902,725 â†’ 50,902,725 | 7 sampler, 12 without compatible partner |
| Playset / spatial | 19 â†’ 19 | 0 / 0 | 23,277,621 â†’ 23,277,637 | 1 implicit-mip semantics, 18 power-of-two/tile/gutter dimension limits |
| Plant / array | 7 â†’ 5 | 2 / 4 | 24,104,859 â†’ 24,121,339 | 1 missing finalized mips, 2 without compatible partner |
| Plant / spatial | 7 â†’ 6 | 1 / 2 | 12,310,347 â†’ 13,927,819 | 5 without a compatible partner fitting the same page |
| ShaderBall / array | 4 â†’ 4 | 0 / 0 | 1,289,587 â†’ 1,289,587 | 1 UV-domain, 1 sampler, 2 without compatible partner |
| ShaderBall / spatial | 4 â†’ 4 | 0 / 0 | 1,150,435 â†’ 1,150,451 | 1 sampler, 3 implicit-mip semantics |

The two 16-byte zero-page package differences accompany changed processing
metadata and no generated pages. No zero-page branch is timed or treated as a
packing benefit.

Plant's array packing preserves 55,574,520 retained image-vector bytes in both
modes: 26,214,400 base bytes plus 29,360,120 explicit mip bytes. The package grows
by 16,480 bytes (0.068%). Spatial packing grows retained image-vector bytes from
26,214,400 to 54,525,952, including gutters/page padding and an explicit duplicate
base level. Its package grows by 1,617,472 bytes (13.14%). Fewer image resources
do not imply fewer stored pixel bytes or a smaller compressed package.

## Correctness and timing method

Preflight cooks every branch twice, checks full public-scene equality between
repeats, and writes/reads every expected output through the installed SDK.
It compares all non-image scene records and authored texture fields, permitting
only image/layer/region remapping. It compares every logical array mip byte and
every spatial tile interior against the common canonical input. The full scene
comparator is `tests/SceneValueComparison.h`, independent of the serializer's
field visitor. Processing metadata is also checked on readback.

Only Plant's two actual-page profiles enter repeated measurements. Each gets
one warmup and seven fresh-process repetitions per off/on mode. Case order
rotates and off/on order reverses each round. Input `ReadScene`, page-only
`CookScene` and `WriteScene` have separate in-process clocks. Writing includes
serialization, Zstandard compression, temporary-file I/O and publication; it
is not a pure encoder or physical durable-storage measurement. Import and
shared mip preparation are outside all three measured calls.

The input owner remains alive through writing. Process memory is the absolute
lifetime peak for input loading, retained input, cooked output, writer scratch
and runtime overhead; it is not a stage peak, allocation-budget use or minimum
caller footprint. Full output equality comparisons run afterward in separate
processes, so comparison references do not contaminate measured peaks.

The existing Windows measurement helper records peak working set/private commit
with 1 ms polling plus a final OS query, wall time and kernel-plus-user CPU time.
Short CPU times have OS accounting granularity. Every sample and min/median/max
is retained. A no-operation process baseline runs once per round and is not
subtracted. OS file caches are warm from preflight and warmups; there is no engine
asset cache. CPU clocks and unrelated host activity are uncontrolled. Other
agents' builds and heavy benchmarks are held during the measurement window.

Host/toolchain: AMD Ryzen 9 9950X3D, 16 cores / 32 logical CPUs; Windows 11 build
26300; MSVC 19.51.36256.0 x64 Release /O2 /MD; CMake 4.4.2; Ninja 1.13.2;
Python 3.14.7. clang-format 22.1.3 passes. Archived compile/link commands use
installed SDK headers/libraries, plus only the checkout's test comparator;
there are no engine or dependency-build paths.

## Repeated stage results

All stage values are **min / median / max** of seven measured processes, in
milliseconds. Each individual sample, including process wall/CPU time and peak
memory, is retained in `final/results.json` under `cases[].modes[].runs[]`.

| Plant profile | Atlas | Input read ms | Page cook ms | Write ms |
| --- | --- | ---: | ---: | ---: |
| Array | Off | 28.906 / 29.233 / 32.493 | 1.176 / 1.243 / 1.407 | 199.898 / 201.849 / 206.970 |
| Array | On | 29.263 / 29.802 / 30.245 | 11.247 / 11.504 / 12.636 | 202.436 / 202.995 / 208.144 |
| Spatial | Off | 17.570 / 17.748 / 19.572 | 0.495 / 0.512 / 0.559 | 100.863 / 101.413 / 104.197 |
| Spatial | On | 17.417 / 17.774 / 20.228 | 30.588 / 31.160 / 31.474 | 123.627 / 124.445 / 129.125 |

Absolute process peaks below use the same min / median / max order, in MiB
(1,048,576 bytes). The input remains alive through writing.

| Plant profile | Atlas | Peak working set MiB | Peak private commit MiB |
| --- | --- | ---: | ---: |
| Array | Off | 89.434 / 89.484 / 89.492 | 86.004 / 86.059 / 86.078 |
| Array | On | 154.801 / 154.809 / 154.820 | 151.430 / 151.457 / 151.480 |
| Spatial | Off | 53.520 / 53.531 / 53.840 | 49.996 / 50.059 / 50.402 |
| Spatial | On | 240.445 / 240.453 / 240.457 | 237.398 / 237.441 / 237.461 |

The no-operation baseline median is 4.610 ms process duration,
4.590 MiB peak working set and
1.191 MiB peak private commit. No baseline is subtracted.

Array packing adds about 10.3 ms to the median page-cooking call; its measured
write-time ranges overlap the off case. Spatial packing adds about 30.6 ms to
median cooking and about 23.0 ms to median publication. Both packed modes have
larger packages and higher process memory peaks in this retained-input workflow.
These fixed natural inputs demonstrate eligibility, preservation and measurable
processor costs, not a universal packing benefit or a renderer cost result.

The coordinated measurement window was 07:05:48–07:05:58 UTC on 25 October 2026.
Other agents reported their heavy work stopped before it began. No timing runs
were launched for the six zero-page branches.


## Evidence and reproduction

All generated evidence stays under `build/m6-atlas-workloads/`:

- `final/results.json`: invocations, input/SDK/source identities, all common
  diagnostics, eligibility/resource counts, package hashes and campaign results.
- `final/<Model>/source/`: exact copied glTF and referenced files;
  `array-input.zenasset` / `spatial-input.zenasset`: common canonical inputs;
  `*-expected-{off,on}.zenasset`: validated expected outputs.
- `final/logs/`: preparation, baseline, stage and postflight logs.
- `final/sdk-identities.json`, `final/sources/`, `final/atlas_workloads.exe`:
  frozen SDK manifest, probe/helper/comparator snapshots and measured executable.
- `final/provenance/`: source README attributions and their identity manifest.
- `final/build-evidence/`, `final/toolchain.json`, `configure.log`, `build.log`,
  `format-check.log`, `preflight.log`, `campaign.log`: configuration, actual compiler/linker
  commands, host identity, successful build and preflight.

From an MSVC x64 developer shell:

```powershell
Set-Location E:/Dev/ZenAssetProcessor
cmake -S tools/evaluation/atlas_workloads -B build/m6-atlas-workloads/build -G Ninja `
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH=E:/Dev/ZenAssetProcessor/build/normal-uv-audit/install `
  -DCMAKE_FIND_USE_PACKAGE_REGISTRY=OFF -DCMAKE_FIND_USE_SYSTEM_PACKAGE_REGISTRY=OFF `
  -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
cmake --build build/m6-atlas-workloads/build --parallel 2
python -B tools/validation/asset_processor_atlas_workloads.py prepare `
  --probe build/m6-atlas-workloads/build/atlas_workloads.exe `
  --sdk build/normal-uv-audit/install `
  --fingerprint bd4084418dee69e76e49fecc0d6dcec61220ae3fd42b91cd93cac6fa0c376b42 `
  --models E:/Dev/glTF-Sample-Assets/Models `
  --output build/m6-atlas-workloads/reproduction
python -B tools/validation/asset_processor_atlas_workloads.py measure `
  --output build/m6-atlas-workloads/reproduction --runs 7
```

Use a new output directory and reserve a quiet measurement window. Source-path
provenance strings can slightly change package sizes in another directory.
The fingerprint is explicit; later production changes are outside this frozen
checkpoint. These data describe processor costs and eligibility on fixed natural
inputs, without a renderer binding, draw/frame, GPU allocation or visual-benefit
claim. Runtime acceptance remains an integration gate. No profitability threshold
is defined by the plan, and this report does not invent one. It does not establish
overall M6 completion. No processor production or engine source was changed.
