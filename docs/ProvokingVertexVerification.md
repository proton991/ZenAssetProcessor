# Provoking-vertex preservation checkpoint

Verified on 25 October 2026. This closes the standalone indexed-triangle
provoking-vertex check within M4. It does not close all geometry, texture, atlas,
performance, or release requirements in the implementation plan.

## Result

The NVIDIA GeForce RTX 5080 and AMD Radeon(TM) Graphics both passed core
first-vertex and optional last-vertex rendering with zero Vulkan validation
errors. Seven package variants per mode and device produced 28 exact frame
comparisons, plus the two imported-source controls per mode/device (36 frames
total). Each frame also passed 64 independent interior-pixel checks against the
expected provoking corner's authored color.

Vulkan normally takes flat attributes from the first vertex; the optional
[`VK_EXT_provoking_vertex` extension](https://docs.vulkan.org/refpages/latest/refpages/source/VK_EXT_provoking_vertex.html)
allows the last vertex. The probe uses a separate render-pass instance for each
mode. Both local devices supported and executed the last-vertex path; the runner
records unsupported hardware explicitly instead of treating it as tested.

The processor SDK fingerprint used by this checkpoint is
`14d66cc26703f8a724c19f02a92033665a42c5f51b3407cb5f27fc7139d558f5`.

## What the check establishes

The processor-owned fixture contains 64 triangles and 192 uniquely indexed
vertices. Vertex storage is shuffled. Every triangle has three different corner
colors, so cyclically rotating its indices changes flat shading while retaining
geometry and winding. Authored normals prevent flat-normal generation from
expanding/reordering the input before the optimization under test.

The evaluation imports this fixture through the installed Processor SDK, cooks
both reference and vertex-fetch-optimized scenes, and requires that optimization
actually changes index storage. It compares every resolved corner's complete
public Vertex fields before serialization and after package reading.

The seven written/read variants are:

| Variant | Fetch remapping | Meshoptimizer | Zstandard | Corner order |
| --- | --- | --- | --- | --- |
| reference-raw | Off | Off | Off | Authored |
| optimized-raw | On | Off | Off | Authored |
| reference-mesh-zstd | Off | On | On | Authored |
| optimized-mesh | On | On | Off | Authored |
| optimized-mesh-zstd | On | On | On | Authored |
| rotated-raw | Off | Off | Off | Cyclic negative control |
| rotated-optimized-mesh-zstd | On | On | On | Cyclic negative control |

The unoptimized compressed case retains shuffled indices, exposing an accidental
triangle codec that rotates corners even if sequential remapped indices would
hide that error. Package-directory inspection confirms both vertex and index
streams actually use semantic encoding. Every combined case also stores both
encoded mesh streams with Zstandard; this is actual payload evidence, not just
requested settings.

Copied source files are deleted before any package is read back. Every normal
case matches the imported reference over all 256 x 256 RGBA bytes. Both rotated
cases match the rotated imported reference exactly and differ from the ordinary
reference in 35,328 channels per frame on both devices and modes. The independent
centroid oracle additionally prevents an identically wrong or blank shader from
passing pairwise comparisons.

For this small fixture, the raw package is 41,389 bytes, fetch-remapped
meshoptimizer-only output is 20,781 bytes, and fetch-remapped meshoptimizer plus
Zstandard output is 3,639 bytes (91.2% smaller than its matching raw package).
These are fixture-specific file sizes, not general performance or GPU-memory
claims. Incidental source-path provenance may change package bytes across runs.

## Reproduce

Use a current installed Processor/Reader SDK, a Vulkan SDK with glslc and the
Khronos validation layer, and an MSVC x64 developer shell on this Windows host.
The test is an optional standalone CMake project; it does not add Vulkan to any
shipped ZenAsset target or require building ZenEngine.

```powershell
Set-Location E:/Dev/ZenAssetProcessor
cmake -S tools/evaluation/provoking_vertex -B build/m4-provoking-vertex -G Ninja `
  -DCMAKE_BUILD_TYPE=Release `
  -DCMAKE_PREFIX_PATH=E:/Dev/ZenAssetProcessor/build/current-consumer-verification/release/install
cmake --build build/m4-provoking-vertex --parallel 4
python tools/validation/asset_processor_provoking_vertex.py `
  --build build/m4-provoking-vertex --output build/m4-provoking-vertex/reproduction `
  --device 'NVIDIA GeForce RTX 5080' --device 'AMD Radeon(TM) Graphics' `
  --executable-alias 7zFM.exe
```

Use a new output directory for each run. The optional executable alias is a
local RTSS overlay-exclusion workaround; it copies the same executable and does
not change rendering. Omit it on hosts without that injection issue. The runner
sets `VK_LAYER_VALIDATE_SYNC=1`, disables implicit layers, and selects each named
GPU without fallback.

The current evidence lives under `build/m4-provoking-vertex/final/`:

- `results.json`: commands, environment, exact input/source/binary hashes,
  device results, package records, comparison records, and artifact hashes.
- `device-0/` and `device-1/`: source-free packages, original/rotated/variant PPM
  frames, `comparisons.tsv`, `packages.tsv`, and `device.txt`.
- `device-0.log` and `device-1.log`: complete validation output, including cleanup.
- Build configuration and final build logs are in the parent build directory.

The validation logs contain expected loader notices about disabled implicit
layers. Neither run reports a validation error. The runner also checks cleanup
messages emitted after the in-process error count was recorded.

## Limits

This checkpoint covers the current triangle-list fetch-remapping and lossless
mesh/transport codecs on these two devices. It does not prove other topologies,
geometry-shader output, transform feedback, lossy quantization quality,
transparency sorting, deformation, custom attribute remapping, or performance.
Those requirements retain their separate existing evidence and outstanding
checks. No ZenEngine source, configuration, or build output was changed for this
checkpoint.
