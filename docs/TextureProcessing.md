# Texture cooking

`CookScene` applies texture processing to an immutable imported scene and
returns an owning cooked scene. Pass that same owner to the runtime adapter or
`WriteScene`; writing and reading a package do not repeat texture cooking.
The current development implementation processes RGBA8 material images with
optional offline mips, BC7 storage, BC5 normals and BC4 occlusion. BC4/BC5 have
focused CPU, independent corpus, engine, preview and native Vulkan evidence;
image-space limits alone do not settle rendered visual acceptance. BC6H
environment cooking uses immutable cube storage and preserves authored IBL
mips. The [final processor checkpoint](ProcessorCompletionVerification.md) includes
a correction for uninitialized encoder lanes, passes both standalone suites, and
passes independent HDR decoding for 90 authored face/mip checks across twelve
cases without relaxing quality limits. Earlier engine/native and raster/GI
reports retain their historical source scope. Current rendered acceptance and
engine integration remain separate from these image-space checks.

## Settings and command line

Texture operations are disabled by default. The CLI requires `--profile desktop`
to enable them; `reference` preserves imported image values and authored mips.
Selecting desktop alone does not activate lossy operations.

```sh
zenassetc cook model.glb --output model.zenasset --profile desktop --texture-mipmaps on --texture-compression bc7
```

| `TextureSettings` field | CLI option | Default |
| --- | --- | --- |
| `generateMipmaps` | `--texture-mipmaps off\|on` | `false` |
| `compressBC7` | `--texture-compression none\|bc7` | `false` |
| `bc7Quality` | `--texture-quality 0..4` | `4` |
| `maxDimension` | `--texture-max-dimension N` | `16384` |
| `colorMinPsnr` | `--color-min-psnr DB` | `40` |
| `colorMaxRmse` | `--color-max-rmse FRACTION` | `0.01` |
| `scalarMaxError` | `--scalar-max-error FRACTION` | `8/255` |
| `normalRmsAngleDegrees` | `--normal-rms-degrees DEGREES` | `1` |
| `normalMaxAngleDegrees` | `--normal-max-degrees DEGREES` | `5` |
| `alphaCoverageMaxError` | `--alpha-coverage-error FRACTION` | `0.01` |
| `compressBC5Normals` | `--normal-bc5 off\|on` | `false` |
| `compressBC4Occlusion` | `--occlusion-bc4 off\|on` | `false` |
| `compressBC6HEnvironment` | `--environment-bc6h off\|on` | `false` |
| `hdrMaxRelativeRmse` | `--hdr-relative-rmse FRACTION` | `0.02` |
| `arrayPages` | `--atlas array` (`off` disables pages) | `false` |
| `arrayMaxDimension` | `--atlas-max-dimension N` | `2048` |
| `arrayMaxLayers` | `--atlas-max-layers N` | `256` |
| `arrayMaxPages` | `--atlas-max-pages N` | `64` |
| `arrayMaxImages` | `--atlas-max-images N` | `8192` |
| `spatialPages` | `--atlas spatial` | `false` |
| `spatialMaxDimension` | `--atlas-max-dimension N` in spatial mode | `2048` |
| `spatialMaxPages` | `--atlas-max-pages N` in spatial mode | `64` |
| `spatialMaxImages` | `--atlas-max-images N` in spatial mode | `8192` |

The maximum dimension is an eligibility limit, not a resize request. Oversized
images retain their original representation with a diagnostic. Quality values
must be finite and nonnegative, normalized errors cannot exceed one, and normal
angles cannot exceed 180 degrees; RMS tolerance cannot exceed the maximum.

All fields participate in `ProcessingSettingsHash`, including values for
disabled operations. Build metadata version 5 records the initial BC7 texture
policy; version 6 adds BC4/BC5/BC6H settings and version 7 adds array-page
activation and limits. Version 8 adds spatial activation and limits; all-default
spatial values retain v7 output. Identity layout 4 includes all spatial fields.
Absent fields in earlier metadata
default to disabled operations. The hash uses
explicit little-endian fields and float bit patterns, independent of native
structure padding.

Array packing runs after texture and environment processing, copying finalized
compatible mip chains without changing encoded blocks or authored UVs. It is
opt-in and does not implicitly generate mips. See [array cooking](ArrayCooking.md)
for eligibility, limits, CLI/manifest examples and current engine restrictions.
Optional [spatial cooking](SpatialCooking.md) shares the all-use analysis and
publishes single-level power-of-two RGBA8 regions. Engine spatial runtime
integration and broader sampling/workload gates remain open.

## Shared images and material roles

The processor collects five core material bindings, seventeen feature bindings,
and the specular/glossiness aliases. It distinguishes color, scalar data, and
normal uses, the actual texture view's color transfer, mip requirements, and
opacity handling. Color/data/normal uses of one source image can produce
separate cooked images because their mip filters or quality limits differ.
Bindings with equal source image and processing requirements share a result.
BC4 selection introduces a separate role for core occlusion R. It does not
discard other channels from metallic/roughness or other packed bindings.

Original texture IDs remain stable; additional uses append texture variants.
Image IDs are compacted after processing so fully replaced RGBA images do not
remain as unused payloads. Unbound texture records retain their original pixels
for browsing and later material edits. Node/material IDs, typed animation targets,
material variants, UV sets, UV transforms, and sampler records are preserved.

## Mips and quality decisions

Existing mip levels remain the reference for their levels. Missing tails use
area-weighted filtering, RGB conversion through the declared transfer function,
linear alpha, and normalized normal directions. BC7 encoding of a mipmapped
binding always generates a complete offline tail, even when the explicit mip
generation flag is off. A sampler that does not use mips needs no implicit tail.
Explicit mip generation requests a complete tail independently of the sampler.

BC7 keeps four-channel sampling, including normal Z. BC5 stores normalized XY
and carries `TextureInterpretation::NormalXYPositiveZ` on its texture record;
the consumer reconstructs positive Z before applying normal scale. BC5 rejects
negative-Z source directions and sRGB normal views. BC4 stores only the consumed
linear occlusion R channel. These two flags take precedence over BC7 for their
eligible uses; rejection retains the original image rather than changing codec.
Blocks clamp at image edges, while every stored mip
retains its original texel dimensions. The encoder output is decoded with a
separate decoder and checked for each image and each mip:

- Color compares sampled linear RGB with both RMSE and PSNR limits.
- Scalar data compares maximum channel error; alpha is checked independently
  for all roles.
- Normal maps compare normalized directions with RMS and maximum angular limits.
  Linear-filtered normal uses also compare channels filtered before normal
  reconstruction, at 15 bilinear phase pairs from `{0, 0.25, 0.5, 0.75}` excluding
  the already checked texel phase `(0, 0)`. Each mip and phase must meet the same
  bounds; averaging phases cannot hide a failure. These finite phases remain
  an initial check. Qualifying candidates then receive a continuous bilinear UV
  bound across every texel neighborhood, with adaptive phase rectangles covering
  `[0, 1]^2`. Each rectangle's per-texel angular bounds must meet the same RMS and
  maximum limits. Endpoint/midpoint observations are separate from certified
  bounds. Depth 12 and 127 rectangle scans per mip bound the work; unresolved
  intervals retain the original whole image with an uncertified diagnostic.
  Repeat seams are included; clamp and mirror edges use the edge texel.
  Nearest-only min/mag filtering keeps the texel checks within each level.
  Sampler behavior participates in normal-use classification when encoding is
  enabled, so shared source images can retain different results. See the
  [continuous UV checkpoint](ContinuousUvNormalVerification.md) for evidence and
  the independent decoded-payload audit.
- Linear mip filtering compares adjacent levels at identical normalized UV
  coordinates and qualifies UV phase and mip blend jointly over `[0,1]^3`.
  Each level uses the sampler's minification filter. Linear minification splits
  each texel cell at coarse-level tap boundaries and bounds every resulting
  trilinear piece. Nearest minification enumerates correlated fine/coarse tap
  states, including exact boundaries and endpoints, and bounds continuous LOD
  for each state. Stored channels blend before normal reconstruction.
  Each box aggregates one angular upper bound per fine-level texel cell into
  the unchanged RMS limit and separately checks the maximum-angle limit.
  The 27 common endpoint/midpoint combinations provide observed errors; they
  do not replace the conservative bound. Depth 12 and 127 box scans per adjacent
  pair bound the work. An observed violation or an unresolved bound retains the
  complete original image and authored chain, with distinct diagnostics.
  Observed errors, certified bounds and the complete-domain certificate are
  separate private metrics on the coarser level. Nearest-mip and non-mipmapped
  uses skip adjacent-level blending qualification. Sampler modes participate in
  normal-use grouping, so different uses of a shared source can retain different
  results. See [joint UV/LOD qualification](ContinuousJointNormalVerification.md)
  for the bound, regression and independent decoded-payload evidence. The earlier
  [fixed-UV LOD checkpoint](ContinuousMipNormalVerification.md) remains historical.
- BC5 compares reconstructed directions and ignores unused alpha; BC4 compares
  only consumed R. Other bindings of a shared source retain their own channels.
- Alpha-mask mips preserve coverage using the material's effective cutoff.
  Final encoded generated mips are checked against their intended coverage.

If a level fails, the whole image retains its original owner and emits a
diagnostic. Generated mips whose mask coverage cannot be represented within the
limit also cause whole-image fallback. Authored levels are not silently rebuilt.
Blended opacity and animated opacity/cutoff require exact alpha; when their
original mip chain is incomplete, the current conservative path retains it.
Negative alpha factors in mask comparisons retain original pixels as well.
Masked bindings also retain original pixels when the scene contains non-unit
vertex alpha or morph-color alpha changes; the image-only coverage check cannot
bound their combined effect. Non-unit or animated base/clearcoat normal scales
retain the original normal image because scaling can amplify angular error.
Clamp-to-border normal uses also retain their original image: the border color
is selected by the runtime and is outside the package's quality contract.
Other eligible uses of the same source image can still be compressed.

These are image-space limits. They do not establish a rendered-image tolerance
or replace GPU comparisons of materials, shadows, GI, or editor previews.
Within-level UV and joint adjacent-mip qualification use ideal stored-channel
interpolation. They do not establish anisotropic error bounds or device filtering
precision. Within-level checking charges two decoded rows (48 bytes per texel
of width), plus its bounded rectangle stack and RMS accumulators. Joint checking
charges two decoded rows from each level (`48 * (fineWidth + coarseWidth)` bytes),
a per-cell upper-bound row (`8 * fineWidth` bytes), and bounded box, tensor and
accumulator storage. Both check cancellation per row and refund temporary storage
on success and failure. Large row offsets use address-sized arithmetic. This adds
offline work and makes no runtime-performance claim.

At BC7 quality 4, linear normal maps with filtered sampling now receive an
additional mode-6 refinement after all baseline blocks in a level are encoded.
It compares the sixteen bilinear phases including texels, using the currently
decoded neighboring blocks. A replacement must reduce total angular error
without increasing any phase's squared error sum, the maximum angular error,
or valid-texel maximum alpha error. A `1e-12` squared-radian comparison epsilon
handles numerical accumulation; the public quality thresholds are unchanged.
Undefined interpolated directions retain the final gate's 180-degree penalty.
Whole-image and whole-mip quality checks still run after refinement.

This search uses a fixed neighborhood of at most nine decoded blocks and a
64-entry cache with full-key equality. Its scratch allocation is charged before
allocation and refunded on success, rejection, or cancellation. Cancellation is
checked per image block, endpoint-search pass, and scored candidate. Cache
collisions affect work only; they cannot select a different candidate. Repeat
seams and the edge rules used by clamp/mirror sampling are included. Levels
0 through 3, nearest-only normal uses, sRGB views, and other texture roles keep
their existing encoder selection. No public setting or package layout changes.

The quality-4 search can substantially increase offline cooking time, including
time spent on images that the final quality gate retains uncompressed. Current
verification establishes bounded storage and improved sampled/rendered results;
it does not establish acceptable production throughput or broader device and
filtering coverage. Texture compression remains opt-in.

The endpoint search avoids packing rejected trials, shares results for identical
quantized seeds, skips redundant p-bit toggles, and stops scoring when a partial
nonnegative error sum already cannot improve the current result. Candidate
ordering and quality guards remain unchanged. The distance calculations use
contiguous component arrays while preserving selector tie-breaking and pixel
summation order. Paired Windows Release measurements show a speedup over the
initial refinement implementation; the remaining offline cost still requires
throughput work before accepting a shipping profile.

Endpoint scoring also keeps 128 entries per source block (4 KiB on the verified
Windows build), included in the image refinement's charged scratch allocation.
Full RGB endpoint keys prevent collision-dependent results. Completed scores
retain their selectors; an incomplete score is only a lower bound and can only
reject a trial whose cutoff is no larger. A larger cutoff forces reevaluation.
The cache resets for every source block, and alpha remains checked by the image
quality guards. This removes repeated scoring without changing candidate order,
selector ties, floating-point sums or the accepted package payloads in the
verified corpus. See the engine's BC7 score-cache verification report for paired
complete-cook measurements and the scope of that byte-equivalence check.

The 24 October filtered-normal checkpoint found previously accepted BC5
ClearCoatTest maps exceeding the unchanged 5-degree maximum after filtering:
`RibsNormal` reached 16.4316 degrees and `PlasticWrap_normals` reached 6.02433
degrees, including authored repeat seams. Both now retain original pixels. Fresh
independent audits pass 15 BC4/BC5 and 11 BC7 cases; 34 Release suites and the two
affected Debug suites pass. ZenEngine captures match the raw clearcoat reference
exactly for the new BC4/BC5 package in raster, compute GI and hardware GI. BC7
clearcoat and BC5 shared-odd glossy differences remain; this checkpoint does not
enable lossy defaults or close rendered acceptance.

## Prepared spatial pages

Prepared spatial atlas regions require their own tile-aware processing path.
Current ordinary mip/BC7/BC5/BC4 cooking rejects scenes containing nonzero spatial
regions with an actionable result and no partial scene; recook original sources.
No-op cooking and optional array processing preserve those records. This does
not enable spatial scene cooking; see [SpatialTextureRegions.md](SpatialTextureRegions.md).

## HDR environments

`ImageBasedLightAsset::specular` owns immutable `EnvironmentCubePixels`.
RGBA32F cubes use typed `rgba32FloatMipFaces`; BC6H UFLOAT cubes use
`blockMipFaces`. Exactly one payload array is active, indexed by mip then
`+X, -X, +Y, -Y, +Z, -Z`. Scene copies share the original owner until a
successful cook replaces it; compressed output retains no decoded float copy.
Irradiance coefficients, intensity and rotation remain unchanged.

BC6H is opt-in and requires finite RGB in `[0,65504]`, alpha exactly one,
and dimensions within the configured limit. Every authored face and mip must
satisfy `sqrt(sum(RGB errorÂ²) / sum(source RGBÂ²)) <= hdrMaxRelativeRmse`
after independent block decoding. Zero source energy requires exactly zero
decoded energy. Padded edge pixels do not contribute to the metric.
An eligibility or quality failure retains the complete original cube and
reports a diagnostic. Invalid/nonfinite input fails scene validation.

Authored specular mips encode roughness filtering. The cook preserves their
count and order, including partial chains; material mip-generation settings
never generate or replace environment levels. Recooking an existing BC6H cube
keeps its owner and block bytes. Every stored block's mode prefix is checked
before the scene is exposed. These checks do not establish device support
or substitute for rendered environment comparisons.

## Ownership and limits

Input pixels are immutable and shared until all required variants are cooked.
New image payloads, mip working buffers, record tables, quality records, and
diagnostics count against the cooking allocation budget. Temporary storage is
released between image variants. Cancellation is checked during traversal,
filtering, and block encoding; failure returns no partially cooked scene.

The renderer must support the stored format. ZenEngine uploads supported BC7
mips directly without ordinary runtime mip generation. Unsupported formats fail
scene preparation and retain the previously published scene. Reader previews
decode bounded samples from the stored image rather than retaining a full RGBA
copy solely for editor thumbnails. Scene validation rejects reserved BC7 block
modes across every stored mip before upload; preview sampling alone is not used
as full-payload validation.
BC4 previews display grayscale; BC5 previews require the explicit normal
interpretation and reconstruct the direction. Unknown or mismatched markers
fail clearly. Texture record version 1 defaults to ordinary channel sampling;
version 2 stores the marker.
