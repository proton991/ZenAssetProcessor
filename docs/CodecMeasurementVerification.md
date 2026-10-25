# Matched lossless-codec measurement checkpoint

Verified 25 October 2026 on Windows Release. The standalone installed-SDK probe
passed 24 full public-scene comparisons and 84 measured reads across three
scenes and four codec policies. This closes the missing matched codec
read/size/process-memory measurement for this corpus. It does not establish
overall M4 completion, texture-quality acceptance, atlas benefit or release readiness.

The frozen SDK is `build/normal-uv-audit/install`, fingerprint
`bd4084418dee69e76e49fecc0d6dcec61220ae3fd42b91cd93cac6fa0c376b42`.
Subsequent production changes are outside this checkpoint.

## Inputs and correctness

Self-contained GLBs from the local glTF-Sample-Assets checkout are copied into
the evidence directory before importing. No source asset is modified.

| Scene | Vertices | Indices | Canonical image-vector bytes | Additional coverage |
| --- | ---: | ---: | ---: | --- |
| DragonAttenuation | 139,379 | 404,985 | 33,554,432 | Largest geometry workload; two meshes |
| DamagedHelmet | 14,556 | 46,356 | 83,886,080 | Five images; texture-dominated storage |
| Fox | 1,728 | 1,728 | 4,194,304 | One skin, three animations, two custom attributes |

Image-vector bytes count base bytes and any explicit mip vectors, including
duplicate storage if present. They are neither GPU allocation nor process RSS.

Each imported scene is written unchanged as raw, Zstandard level 3,
meshoptimizer only, and meshoptimizer plus Zstandard. There is no `CookScene`
pass, vertex-fetch optimization, quantization, texture encoding or mip generation.
Import uses two workers; the package encoder uses its single-threaded codec policy.

The separate preparation process compares each decoded package against its
imported scene with the existing `tests/SceneValueComparison.h`. This compares
the public object model independently of the serialization field visitor:
geometry, full influences, morph/custom domains, hierarchy, materials, images,
samplers, animation, variants, environment, provenance and remaining scene fields.
The copied source is then renamed away from its recorded path. A second
correctness process compares every package against the raw package without that
source path. All 12 imported/package and 12 source-absent comparisons pass.

Directory inspection confirms actual vertex encoding 1 and index encoding 2 in
every mesh-enabled package. The other variants have no semantic encoding.
Zstandard is actually present in 15 Dragon, 14 Helmet and 41 Fox chunks in each
transport-enabled variant; raw transport contains none. Per-resource identities,
versions, stored sizes and transport-decoded sizes are retained. A directory's
transport-decoded size is the intermediate semantic payload when mesh coding is
enabled; it must not be described as expanded canonical mesh bytes.

## Measurement method

`tools/evaluation/codec_measurements` is an optional standalone CMake project
using only the installed Processor/Reader libraries. Its only checkout include
is the existing test comparator. Compile/link evidence contains no ZenEngine or
dependency-build paths. It adds no dependencies to shipped SDK targets.

Every measured process executes exactly one `ReadScene(path)`. It does not
import sources, write packages, load a reference scene or compare complete scenes.
Read time covers file open/read, checksums, transport and semantic decoding,
object construction and validation. It excludes process startup and scene
destruction. It is **not isolated decoder time**. Whole-process elapsed and
kernel-plus-user CPU time are separate measurements.

There are seven measured repetitions per scene/policy, plus one unreported
warmup for each package. Each repetition rotates variant order, reverses it on
odd rounds, and interleaves scenes. Every read starts a fresh process with no
asset cache; OS file caches are warm from copying, correctness checks and
warmup. OS caches are not flushed. CPU caches, frequencies and background host
activity are uncontrolled. Other agent builds and heavy experiments were paused
during this short campaign.

Windows lifetime peak working set and private commit use the existing validated
`GetProcessMemoryInfo` helper, polled at 1 ms and queried after exit. Memory
results include process/runtime overhead and the one decoded scene, with no
reference scene retained. They are absolute process peaks, not allocation-budget
usage or GPU memory. Preparation/comparison process peaks are retained as
preflight records and excluded from measurement summaries.

The same executable's no-read baseline runs once per measured round. Its median
whole-process time is 4.20 ms, peak working set 4.55 MiB and peak private commit
0.89 MiB. These are reported without subtracting them from read-process peaks.
Process CPU time is coarse here (often 0 or 15.625 ms for short runs), so wall
`ReadScene` duration is the useful timing for Fox. Each sample and min/median/max
for every metric are in `results.json`; warmups remain separate.

Host: AMD Ryzen 9 9950X3D, 16 cores/32 logical CPUs, Windows 11 build 26300.
Probe: MSVC 19.51.36256.0 x64, Release /O2 /MD; CMake 4.4.2, Ninja 1.13.2,
Python 3.14.7. The C++ source passes clang-format 22.1.3.

## Results

Times and memory below are medians of seven fresh processes. MiB is 1,048,576 bytes.

| Scene | Codec | Package bytes | Read ms | Peak working set MiB | Peak private commit MiB |
| --- | --- | ---: | ---: | ---: | ---: |
| Dragon | Raw | 54,698,896 | 34.56 | 70.09 | 66.50 |
| Dragon | Zstandard | 8,877,300 | 48.59 | 69.70 | 66.13 |
| Dragon | Meshoptimizer | 41,321,088 | 36.51 | 70.88 | 67.28 |
| Dragon | Meshoptimizer + Zstandard | 8,399,668 | 48.18 | 69.71 | 66.14 |
| Helmet | Raw | 86,116,108 | 31.96 | 102.56 | 99.07 |
| Helmet | Zstandard | 21,943,932 | 69.59 | 102.47 | 98.88 |
| Helmet | Meshoptimizer | 84,679,116 | 31.92 | 102.43 | 98.91 |
| Helmet | Meshoptimizer + Zstandard | 21,943,916 | 69.20 | 102.49 | 98.89 |
| Fox | Raw | 4,674,660 | 2.61 | 13.08 | 9.38 |
| Fox | Zstandard | 138,592 | 3.93 | 13.14 | 9.37 |
| Fox | Meshoptimizer | 4,524,372 | 2.74 | 13.10 | 9.38 |
| Fox | Meshoptimizer + Zstandard | 160,224 | 3.79 | 13.16 | 9.38 |

Combined codecs reduce these raw packages by 84.64%, 74.52% and 96.57%.
Against Zstandard alone, the combination saves 477,632 bytes for Dragon and
16 bytes for Helmet, but **adds 21,632 bytes (15.61%) for Fox**. Semantic fallback
is selected before transport compression; the writer does not compare every
combined policy and choose the globally smallest package.

Zstandard's storage savings have additional read CPU cost in these warm-cache
cases. Mesh coding does not materially reduce decoded process peaks. The small
read differences between compressed variants are observations from seven runs,
not demonstrated general throughput wins. There is no GPU-memory, cold-disk,
runtime adaptation, upload or frame-time claim.

## Evidence and reproduction

All generated artifacts are under `build/m4-codec-measurements/`:

- `final/results.json`: exact invocation, source/binary identities, every
  package/resource size, actual encodings, correctness commands, samples, order
  and min/median/max summaries. `passed` and `identities_unchanged` are true.
- `final/sdk-identities.json`: SHA-256 identities of every frozen installed SDK file.
- `final/dragon/`, `final/helmet/`, `final/fox/`: four packages each and
  retained `source.glb.absent`; recorded `source.glb` paths remain absent.
- `final/logs/`: preparation, source-absent comparison, baseline, warmup and
  all 84 measured-read logs.
- `final/sources/` and `final/codec_measurements.exe`: exact probe, comparator,
  Python helper snapshots and executable used.
- `final/build-evidence/`: CMake cache, compiler information, compile commands,
  Ninja rules and actual compile/link commands; `final/toolchain.json` records host/tools.
- `configure.log`, `build.log`, `format-check.log`, `campaign.log`: successful
  configuration/build/formatting and complete campaign progress.

From an MSVC x64 developer shell, using a new output directory:

```powershell
Set-Location E:/Dev/ZenAssetProcessor
cmake -S tools/evaluation/codec_measurements -B build/m4-codec-measurements/build -G Ninja `
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH=E:/Dev/ZenAssetProcessor/build/normal-uv-audit/install `
  -DCMAKE_FIND_USE_PACKAGE_REGISTRY=OFF -DCMAKE_FIND_USE_SYSTEM_PACKAGE_REGISTRY=OFF `
  -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
cmake --build build/m4-codec-measurements/build --parallel 2
python -B tools/validation/asset_processor_codec_measurements.py `
  --probe build/m4-codec-measurements/build/codec_measurements.exe `
  --sdk build/normal-uv-audit/install `
  --fingerprint bd4084418dee69e76e49fecc0d6dcec61220ae3fd42b91cd93cac6fa0c376b42 `
  --output build/m4-codec-measurements/reproduction --runs 7 `
  --model dragon E:/Dev/glTF-Sample-Assets/Models/DragonAttenuation/glTF-Binary/DragonAttenuation.glb `
  --model helmet E:/Dev/glTF-Sample-Assets/Models/DamagedHelmet/glTF-Binary/DamagedHelmet.glb `
  --model fox E:/Dev/glTF-Sample-Assets/Models/Fox/glTF-Binary/Fox.glb
```

Change the explicit fingerprint when intentionally testing a different SDK; do
not attribute that run to this frozen checkpoint. Source GLBs and generated
package artifacts are local evidence, not newly published redistributable assets.
No engine files or outputs were written and no core processor code was changed.

