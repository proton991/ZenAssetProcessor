# Optional array-page cooking

`CookScene` can pack compatible material images into native texture-array pages.
It runs after texture compression and environment processing. Every layer keeps
its original dimensions, mip count and complete mip bytes. Texture IDs, sampler
references, material bindings, UV sets and authored UV transforms remain unchanged;
only image references and `TextureAsset::arrayLayer` change. Image records are
compacted, and the first source image in a page supplies its name.

ZenEngine's CPU adapter carries page/layer ownership and its editor previews
select individual layers. The GPU loader uploads each immutable array page once
per scene load and binds compatible individual 2D layer views. Native images
remain owned until graph/GPU retirement.
The feature is disabled by default and is not yet exposed in the editor.

## Configuration

```sh
zenassetc cook model.glb --output model.zenasset --profile desktop --texture-mipmaps on --texture-compression bc7 --atlas array
```

`--atlas off` preserves the existing image layout. `--atlas on` is not a supported
mode: `array` explicitly selects the layout. Array packing itself does not
generate mips; inputs without explicit finalized levels are retained with a
diagnostic. Full and partial authored chains are supported.

The corresponding manifest fragment inside a desktop job is:

```json
"processing": {
  "textures": {
    "generateMipmaps": true,
    "arrayPages": true,
    "arrayMaxDimension": 2048,
    "arrayMaxLayers": 256,
    "arrayMaxPages": 64,
    "arrayMaxImages": 8192
  }
}
```

The four limits also have CLI options `--atlas-max-dimension`,
`--atlas-max-layers`, `--atlas-max-pages` and `--atlas-max-images`. Dimensions must
be 1–16384, layers 2–2048, and page/input limits positive `u32` values. Validation
checks disabled settings too. Oversized images remain standalone; exceeding the
eligible-input or required-page limit fails the cook without publishing a scene.
Layer limits split compatible groups into pages, leaving singleton remainders
standalone. No resizing or texel-density conversion is performed.

All five settings participate in processing hash layout 3, including disabled
values. Metadata v7 stores them; older metadata defaults array packing off.
The shared page processor also implements [spatial cooking](SpatialCooking.md).
The two modes are mutually exclusive; default spatial settings retain v7 output.
Changing any limit invalidates batch reuse. The reference profile rejects active
array processing, consistently with its other processing restrictions.

## Eligibility and identity

The pass examines all mesh material assignments, node overrides and material
variants. Compression and array analysis share the same enumeration of five
core, seventeen feature and two legacy texture bindings. Every material use and
texture view of an image must qualify. The initial policy requires:

- Explicit clamp-to-edge S/T addressing and linear minification, magnification
  and mip filtering, with identical sampler policy across the image's views.
- Referenced indexed geometry with available UVs; effective coordinates after
  each authored transform must be finite and remain within `[0,1]`.
- Static UV state, including no texture-transform animation or UV morph targets.
- Finalized explicit mip levels and dimensions within the configured limit.

Catalog/unbound images and bindings, unspecified or repeating samplers,
incompatible sampler uses, missing UVs, animated UVs and existing array pages
remain standalone or retain their existing page. Diagnostics report the first
deterministic exclusion reason per image. Non-UV material-factor animation does
not disqualify unchanged image bytes.

Groups match sampler policy, material-role set, view-format set and interpretation
set. The packer additionally requires identical storage format, dimensions and
mip count. IDs determine deterministic layer placement. Compatible aliases retain
their separate texture IDs and select the same image/layer. Existing compressed
BC7, BC4 and BC5 bytes are copied without re-encoding. Ordinary standalone views
can coexist with pages in the same package.

Analysis storage, mappings, diagnostics and page copies use the cook allocation
budget. Scratch is refunded; retained pixel storage remains charged. Earlier
processing charges are conservatively retained while the working copy exists.
Allocation failure and cancellation return no cooked scene and leave the input
owner unchanged. Publication happens only after fallible planning, copying and
diagnostics finish, followed by CookScene's validation and cancellation checks.

## Evidence and remaining work

On 2026-10-24, the complete Release suite passed 33/33 in 14.01 seconds.
Additional multi-page and compatible-alias coverage then passed 910 focused
array-cooking checks in Release. Array cooking, texture processing and format
settings suites passed Debug (3/3 in 0.19 seconds). The CLI fixture covers
raw/BC7 source cooking, array schema versions, bounded-option errors preserving
existing outputs, reference-profile rejection, manifest reuse/invalidation and
source-free package validation. Library tests also cover BC4/BC5, raw/Zstandard
round trips, legacy metadata, variants, overrides, exclusions, allocation
boundaries and cancellation.

Logs are under `build/processor-port/`: `m6-array-cooking-full-tests.log`,
`m6-array-cooking-alias-tests.log`, `m6-array-cooking-debug-tests.log`, and their
build logs. `build/m6-array-cooking-identities.json` records this checkpoint.

ZenEngine's Release CPU integration passes 38 adapter/package-I/O/cache tests
and four editor-preview tests, with no tracked CPU leaks. These check retained
RGBA8/BC7/BC4/BC5 layer bytes, shared immutable engine owners, selected-layer
previews, local overrides, malformed layers and v2 destination preservation.
Engine logs are `build/asset-integration-release/m6-array-cpu-tests.log` and
`m6-array-preview-tests.log`. This CPU checkpoint alone does not establish native
upload or rendering support.

The subsequent engine runtime checkpoint passes 713 RenderCore tests, eight
editor regressions and two Vulkan sampling cases (inline/threaded). RGBA8, BC7,
BC4 and BC5 samples match standalone image references exactly at the tested
coordinates and mips. These native fixtures construct engine records directly.
A subsequent source-absent cooked-package test passes 16 exact PBR frame pairs
for raw and BC7/BC5/BC4 pages, variants, transformed UVs, shared instances and a
standalone repeating mask in both execution modes. Seven authored native images
become four. Broader visual and workload acceptance remains open; details and
identities are in ZenEngine's `Doc/ZenAssetProcessorM6RuntimeVerification.md`.

The expanded engine checkpoint passes 64 cooked frame pairs with minification
and grazing views, plus native 16x anisotropic gradient/mip-tail comparisons for
RGBA8/BC7/BC4/BC5. Actual native texture allocations are equal at 16x16 and slightly
larger for arrays at 128x128; binding counts are unchanged. See ZenEngine's
`Doc/ZenAssetProcessorM6SamplingMeasurements.md`. This is additional correctness
and allocation evidence, not a demonstrated workload speedup.

ZenEngine's `Doc/ZenAssetProcessorM6UploadMeasurements.md` adds paired isolated
graphics-queue measurements at 128/1024: equal payload bytes, fewer images/copy
regions, more native views and slightly larger allocations. It also verifies
package aliases through an independently installed reader/writer consumer.
Small timing differences in that harness do not establish production async
upload or frame-time benefit, so the default remains off.

The earlier [GPU sampling evaluation](AtlasSamplingEvaluation.md) used production
packer output and individual 2D layer views with actual 16x anisotropy. It does
not validate ZenEngine's runtime integration. Arrays reduced native image count
in that probe but did not reduce allocation bytes. Real-scene quality, memory,
upload and frame-cost comparisons remain required; cooking does not claim a
performance win. [Repeated processor measurements](AtlasCookingMeasurements.md)
now record separate cooking/publication time, package size and process peaks on
matched off/on workloads; some packed cases are larger and more expensive.
Spatial cooking has a separate [bounded single-level contract](SpatialCooking.md).
Its later runtime checkpoint supersedes the initial rejected sampling candidates
only within that declared contract.
