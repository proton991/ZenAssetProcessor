# FBX source import

`ImportScene` from `ZenAsset::Processor` accepts ASCII and binary `.fbx` files
through the private ufbx adapter and returns an owning canonical `Scene`.
Public headers contain no ufbx, renderer or graphics-API objects. Parser and
worker lifetimes end before return. See [third-party notices](../THIRD_PARTY_NOTICES.md)
for the pinned dependency version and license.

Coordinates are converted to right-handed, Y-up meters; animation time is in
seconds. Import retains source axes/unit metadata in `Scene.sourceDocument`.
FBX has one scene: `sceneIndex` must be `UINT32_MAX` or zero. Import does not
center/normalize the model for a viewer, apply a renderer's material model, or
create GPU resources. Optional cooking and `.zenasset` writing are separate.

## Canonical support matrix

| Source feature | Imported representation and boundary |
| --- | --- |
| Nodes and instances | Names, parents/children, visibility, shared meshes and per-instance material-slot overrides are retained. ufbx helper nodes represent geometric transforms, pivots and unusual scale inheritance. Canonical local transforms can retain an affine base matrix. |
| Polygon geometry | Polygons are triangulated with independent corner normal, tangent, color and UV indices. Expanded vertices keep source control-point mappings. Missing normals use flat triangle normals. Empty material parts produce no primitive. |
| UV/color sets | Slot zero is a mesh's default UV set; named sets have scene-wide compact slots. Extra color sets are retained as primitive-vertex attributes. V is reversed for canonical top-left UV/image convention, with corresponding texture-transform and tangent-handedness conversion. |
| Material factors | Base color/opacity, metallic, roughness, emission, double-sided/unlit flags, IOR/specular color/weight, clearcoat, transmission, sheen and anisotropy factors are mapped where ufbx exposes them. Legacy Lambert/Phong conversion is an approximation with a warning. |
| Material textures | Base color, tangent-space normal, emission and occlusion maps are mapped directly. Separate roughness/metalness maps pack into G/B; opacity multiplies base-color alpha. Mapped opacity can select alpha blending. |
| Texture interpretation | Color and data views share immutable decoded images. Positive-Y tangent-space normal maps receive a derived green-channel reversal when V is reversed; source pixels remain unchanged. This does not interpret a height/bump image as a normal map. |
| Cameras | Perspective and orthographic projection parameters are retained after axis/unit conversion; invalid or unrepresentable projection values fail. |
| Lights | Point, directional and spot records are retained. Relative FBX intensity maps numerically with a warning; non-quadratic local-light decay is approximated by inverse-square falloff with a warning. Area/volume lights fail. |
| Linear/rigid skins | Every positive influence is retained and normalized, with geometry-to-bone bind matrices. Corner splitting retains control-point/influence correspondence. Unweighted vertices receive a fallback joint following the first source mesh instance's geometry transform. |
| Morphs | Position offsets and authored normal offsets are retained. Each in-between shape becomes a morph target with an effective weight. Absent normal offsets remain absent; topology-based source normal regeneration is not reproduced. |
| Combined deformation | Canonical data represents morphs before one linear/rigid skin. A source connection ordering with a blend deformer after a skin is rejected instead of reordered silently. Consumers still evaluate tracks and apply morph/skin deformation. |

Implementation sources are [FbxSceneImporter.cpp](../src/Importers/FbxSceneImporter.cpp),
[FbxDeformation.cpp](../src/Importers/FbxDeformation.cpp) and the public
[scene records](../include/ZenAsset/Format/Scene.h). These are data-preservation
claims, not guarantees of pixel-equivalent rendering in another application.

## Images, UV compatibility and packing

Embedded images and external texture files use the shared
[image decoder](../src/Importers/ImageDecoder.cpp), including PNG/JPEG/TGA,
WebP and supported KTX2/Basis content. Material images become RGBA8, including
decoded Basis mip chains; general DDS/TIFF support, KTX2 arrays/cubemap payloads
and arbitrary source bit-depth preservation are not claimed. Unsupported or
corrupt images fail with the decoder's reason.

External resolution tries ufbx's resolved source-relative path, the authored
absolute path, then the basename beside the FBX. This supports relocated assets
with stale recorded paths. Network/device paths outside the source's own folder
are skipped. The selected file revision and earlier missing preferred candidates
are retained as dependencies, allowing an incremental consumer to detect later
resolution changes. Referenced image decoding is shared rather than repeated
for each material view.

An active binding requiring a missing named UV set uses that mesh's first set
with a warning when `SourceSettings.fallbackMissingNamedUVSets` is true (the
default). Set it false for strict rejection. A mesh with no UV coordinates
always fails such a binding. Unused material slots impose no UV requirement.
Warnings from mesh jobs are published in source order.

Channel packing requires matching UV sets, transforms and sampler settings;
incompatible bindings fail with `UnsupportedFeature`. Different image dimensions
are resampled with nearest sampling to the maximum width/height of the pair.
Derived packed images contain a base level; optional cooking can generate mips.
Packing here normalizes source material channels; it is not spatial atlas packing.

## Animation baking

FBX curves are evaluated with ufbx during import and converted to canonical
tracks. Mapped node TRS (including helpers), visibility, effective morph weights,
material factors, camera parameters and light parameters are baked. Only
represented elements animated by some stack are evaluated into track layouts.
Static elements retain authored values; properties active in another clip keep
constant tracks where needed to restore their values when clips change.

Clip names and duration are retained, with times shifted to start at zero.
Empty takes without curves are accepted as static content. The processor does
not ship an FBX evaluator for playback; consumers use the canonical samplers
and typed targets after import.

[AnimationBakeSettings](../include/ZenAsset/Format/Animation.h) defaults to:

| Setting | Default / meaning |
| --- | --- |
| `sampleRate` | 60 samples/second, augmented with source key times and neighboring representable float times. Valid range is greater than zero through 1,000. |
| `translationTolerance` | `1e-5` meters in local translation. |
| `rotationTolerance` | `1e-4` radians. |
| `scaleTolerance` | `1e-5` local scale. |
| `valueTolerance` | `1e-4` for other represented values. |
| `maxSubdivisions` | 10; supported setting is at most 20. |
| `maxValues` | `64 * 1024 * 1024` values across bake output; must be positive. |

Intervals are compared with the source evaluator at quarter, midpoint and
three-quarter positions and subdivided when needed. Tolerances must be finite
and positive. These are **sampled local-property checks**, not a mathematical
bound on every world-space vertex between samples. Settings are recorded on
baked clips. Budget exhaustion or failure to meet the sampled tolerances fails
the candidate. Cancellation is checked between evaluations. See
[FbxAnimation.cpp](../src/Importers/FbxAnimation.cpp) for the exact track layout
and adaptive bake policy.

## Rejection and degradation policy

| Condition | Outcome |
| --- | --- |
| Layered/procedural textures consumed by supported slots, constraints, polygon holes, point/line polygons, unsupported procedural node attributes or multiple attributes on one node | Named `UnsupportedFeature` failure. Bake constraints or convert unsupported geometry in the source tool. |
| Dual-quaternion/blended skinning, multiple skins, geometry caches, non-normalized cluster link modes, blend-after-skin connections | Named unsupported-feature failure. No silent four-weight truncation or deformer reorder. |
| Animated texture properties, material shading-mode changes, camera projection-mode changes, light type/decay changes, animated gloss factor combined with a gloss texture | Named unsupported-feature failure. |
| Subdivision surfaces | Imports the authored control cage with a warning; no subdivision evaluation. |
| Unsupported material feature textures | Warning; the mapped scalar factor remains. Height/bump textures are warned and not applied. Unmapped animated element properties also produce warnings. |
| Legacy materials, relative light intensity or nonphysical decay | Approximate conversion with named warnings, as described above. |
| Missing texture, unsupported decoder input, invalid transforms/attributes/bind data, nonfinite animation values or changed bake property layout | Failure; no partial scene is published. |

Warnings can accompany a successful import. The shared
[post-import pass](../src/Processing/SceneAssetProcessing.cpp) also clears
mismatched default morph weights with warnings and skips invalid animation
clips individually with their reason; other invalid canonical data fails scene
validation. Callers requiring strict fidelity should inspect all diagnostics.

## Resource limits and verification scope

[SceneImportContext](../include/ZenAsset/Processor/Import.h) currently preflights
the **root** FBX against `maxSourceBytes` (default 16 GiB). This backend does not
apply that field as a cumulative external-texture budget. External encoded
textures separately must be nonempty and no larger than `UINT32_MAX` bytes.

`maxDecodedImageBytes` defaults to 16 GiB of cumulative decoded/derived image
allocation requests, including temporary decoder copies and packed/normal-map
conversions. Parser-private memory is separate. ufbx load and animation-evaluation
temporary/result allocators each have a fixed 8 GiB limit; loading has a node
depth limit of 4,096. Files with skin clusters are reparsed with the retained DOM
to validate cluster link modes. These limits are not a combined process-RSS cap.

`maxGeometryBytes` defaults to 16 GiB and admits processor-owned geometry,
deformation and animation storage **before** allocating it. FBX charges emitted
vertex/index/UV/color buffers, source control-point maps, all skin influences and
bind matrices, morph arrays/weights, and adaptive animation sample/output buffers.
Triangle/corner/UV lookup scratch and simultaneous worker-result/scene append
storage share the same allowance. Capacity growth retains conservative previous
charges to cover replacement overlap; the allowance is requested storage, not
final package size or process RSS. ufbx-owned parsed/evaluated trees, general
metadata/diagnostics, graph-validation scratch and allocator bookkeeping are outside
this allowance;
images retain their separate budget. `maxObjGeometryBytes` applies only to OBJ.

Exceeding the allowance returns `ResourceLimit` with a `maxGeometryBytes`
diagnostic and no asset. Geometry conversion also checks 32-bit vertex/index
limits; final canonical validation retains its separate 16 GiB runtime-geometry
bound. Cancellation is cooperative during parsing/conversion and baking. Queued
mesh workers are drained before reservations are refunded and the import returns.
File progress counts distinct physical inputs, not embedded images or generated
packed maps.

Standalone `ZenAssetImportFixtures`, `ZenAssetImportLimits`,
`ZenAssetProcessingFbxRegression`, `ZenAssetFileProgressFixtures` and
`ZenAssetCookFixtures` exercise owned binary/ASCII, embedded-image and combined
morph/skin fixtures, canonical validation, limits/cancellation and package
round trips. [ProcessingTests.cpp](../tests/ProcessingTests.cpp) explicitly
checks warning-based invalid-clip and morph-default recovery.
[FbxGeometryBudgetTests.cpp](../tests/FbxGeometryBudgetTests.cpp) checks exact
serial declared-reservation boundaries, cumulative parallel expansion, six-weight
skin/morph/animation preservation and cancellation after charged work. These
tests do not establish exhaustive exporter/version coverage or exact
source-evaluator parity for every scene. The focused outcomes and admission
boundaries are recorded in [ImportGeometryBudgetVerification.md](ImportGeometryBudgetVerification.md).

The larger historical semantic and consumer fixtures are owned under
[tests/integration/zenengine](../tests/integration/zenengine/README.md) and remain
opt-in. Renderer, GPU upload and editor behavior are outside this standalone
contract.
