# Spatial page cooking

`CookScene` and `zenassetc --atlas spatial` can construct bounded spatial pages
from eligible source images. This is an opt-in development path. The standalone
Reader and ZenEngine CPU scenes preserve the regions; engine previews crop the
logical tiles. ZenEngine's bounded GPU path now shares native pages and samples
separate per-binding regions. Cooked scenes pass 24 exact spatial/standalone
frame pairs on the tested GPU. Allocation grows in that fixture, so this does
not close M6's workload or broader rendering gates. See the
[GPU checkpoint](../../ZenEngine/Doc/ZenAssetProcessorM6SpatialGpuVerification.md).

The later engine [exact guide capture checkpoint](../../ZenEngine/Doc/ZenAssetProcessorGuideCaptureVerification.md)
also passes eight bounded spatial/reference GI pairs and eight identical-package
repeats using software/hardware queries in both execution modes. It uses an
opt-in engine capture mode with guide learning retained; it does not broaden
this cooker's sampling eligibility or establish workload benefit.

```sh
zenassetc cook model.glb --output model.zenasset --profile desktop --atlas spatial --atlas-max-dimension 2048 --compression zstd
```

## Settings and identity

| `TextureSettings` field | Default | Meaning |
| --- | --- | --- |
| `spatialPages` | `false` | Enable spatial pages; mutually exclusive with `arrayPages` |
| `spatialMaxDimension` | `2048` | Maximum physical page width and height, including gutters/padding |
| `spatialMaxPages` | `64` | Maximum number of constructed pages |
| `spatialMaxImages` | `8192` | Maximum eligible input image count |

All limits must be positive, with dimensions at most 16,384. An enabled spatial
mode requires a power-of-two dimension of at least 16. CLI dimension, page and
image limits apply to the selected atlas mode regardless of argument order;
`--atlas-max-layers` remains an array setting. The same four spatial fields are
accepted under manifest `processing.textures`.

Processing identity layout 4 includes every new field even while disabled.
Metadata v8 appends the spatial activation boolean and three `u32` limits.
The writer still uses Metadata v7 when all spatial values are defaults. Earlier
metadata initializes the added fields to those defaults. Image and texture
records determine actual storage independently of build metadata.

## Eligibility and publication

The shared texture-page processor examines every texture alias and material
binding, mesh primitive, node material override and material variant. All uses
must have known effective authored UVs inside `[0,1]`. UV animation, UV morphs,
unbound material/catalog views, missing coordinates, non-linear filtering,
non-clamp wrapping and conflicting sampler policies exclude the image.
Compatibility groups include sampler policy, view formats, interpretation and
the complete set of material roles. Authored transforms and texture IDs remain
unchanged.

Spatial input additionally requires power-of-two RGBA8 UNORM/sRGB images and
compatible views. A tile must fit the dimension limit with four texels of gutter
on each edge. It can have one explicit mip, or no explicit mips if every sampler
view disables mipmaps. An implicit runtime-generated mip chain is never replaced
with one level. Multiple authored levels and block-compressed storage are
retained standalone with reasons. Existing spatial and array pages are retained.

The packer copies exact source texels, extrudes all edges and corners, aligns
interiors to four texels and rounds page extents to powers of two. Pages have one
explicit physical mip. Each accepted texture receives a Texture v4 integer
region separately from its authored UV transform. Singleton pages are discarded
because they add padding without reducing image count. Mixed standalone/page
output preserves shared image owners and aliases. No density or visual threshold
is silently substituted for the remaining workload measurements.

Analysis, page storage, padding, publication scratch and diagnostics use the
existing cooking budget. Per-page ownership charges allow unused singleton
storage to be refunded. Publication completes fallible work before changing
image references. Failure or cancellation exposes no cooked scene and leaves
the input immutable. Count limits include constructed singleton pages before
they are discarded, and their temporary storage must fit the budget.

Texture processing precedes page construction. Enabling ordinary mip generation
or BC encoding may make images ineligible for this spatial path; it does not
truncate mips or reinterpret encoded blocks. Transport Zstandard compression is
independent and remains available for spatial packages. Reprocessing a cooked
spatial page with ordinary mip/BC operations still requires original sources.

## Verification scope

`ArrayProcessingTests.cpp` runs the shared material-use and failure checks in
both modes. Spatial cases cover mixed dimensions, exact tile/gutter bytes,
singleton/multiple pages, limits, aliases, role/view grouping, implicit and
explicit mip policies, BC exclusion, deterministic output, immutable inputs,
recooking and raw/Zstandard round trips. `SpatialCliTests.py` covers option
ordering, source-free reads, manifest reuse and invalidation, metadata versions
and invalid requests preserving an existing destination. Settings tests cover
activation, disabled-value identity, limits and mutual exclusion.

These checks establish construction and package behavior. The later engine GPU
checkpoint adds region propagation, native sampling and cooked raster comparisons.
The subsequent [cooked spatial ray checkpoint](../../ZenEngine/Doc/ZenAssetProcessorM6SpatialRayVerification.md)
adds 393,216 exact spatial/standalone ray comparisons through production software
and hardware traversal, in compute and fragment contexts. The later
[25 October engine checkpoint](../../ZenEngine/Doc/ZenAssetProcessorAtlasPageExtentVerification.md)
passes the bounded cooked raster, GI, repeat and ray comparisons on both tested
GPUs, including the previously failing AMD case. These are historical engine
results at the fingerprints recorded there, not verification of every subsequent
processor change. Unsupported mip/filter/dimension/compression combinations
remain excluded, and workload benefit is still unproven.
[Processor measurements](AtlasCookingMeasurements.md) record matched off/on
cooking and publication costs and their scope. See also
[region records](SpatialTextureRegions.md) and the
[sampling evaluation](AtlasCoordinatePrecisionEvaluation.md).

### Verified checkpoint, 24 October 2026

This table records the original cooker checkpoint. The subsequent engine CPU
checkpoint replaces its adapter rejection with retained regions and cropped
previews. The later GPU checkpoint linked above supersedes the temporary upload
boundary; the original results below remain historical evidence.

| Check | Result |
| --- | --- |
| Full standalone Release suite after successful rebuild | 34/34, 26.67 seconds |
| Affected Debug packing, cooking, scene I/O, preview and settings suites | 6/6, 0.71 seconds |
| Reader-only Release suite | 4/4, 0.29 seconds |
| Rebuilt engine adapter, scene I/O and cache suites | 39/39, 6.72 seconds, no tracked CPU leaks |
| CLI cook/read and engine load checks | 9/9 expected outcomes |

The shared cooker executable reports 1,601 checks. The packing suites retain
1,327 spatial and 3,175 array checks; scene I/O reports 208. Reader-only `_deps`
still contains only bcdec source/build/subbuild directories.

The application checks cook three images into one spatial page under raw and
Zstandard transport, then remove the source from its recorded path. Standalone
reading succeeds for both packages. The rebuilt engine reports the explicit
unsupported spatial-region error for both; its ordinary-texture control loads
one node, one mesh, nine vertices and three textures. This is an integration
boundary check, not spatial rendering evidence.

The initial build encountered concurrent CMake regeneration after test
registration was edited during the build. An explicit configure followed by a
complete rebuild succeeded. Both initial logs and final results are retained;
no test or quality threshold was relaxed.

Logs use `m6-spatial-cook-*` under `build/processor-port` and the engine's
`build/asset-integration-release`; Reader logs are
`build/reader-source-evidence/spatial-cook-*`. Selected source, binaries, logs,
packages and runtime results are frozen under `build/m6-spatial-cook-evidence`.
Its identity manifest describes a scoped checkpoint, not a release distribution.
