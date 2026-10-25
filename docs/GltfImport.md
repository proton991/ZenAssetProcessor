# glTF / GLB source import

`ImportScene` from `ZenAsset::Processor` accepts `.gltf` and `.glb` and returns an
owning, immutable canonical `Scene`. Parser objects and import workers are
released before return. This contract concerns imported data, not a renderer's
shaders, animation player, GPU resources or editor behavior. Cooking and
`.zenasset` writing are separate operations.

The implementation uses fastgltf plus native Draco, meshoptimizer and shared
image-decoder paths. Dependency versions and licenses are recorded in
[third-party notices](../THIRD_PARTY_NOTICES.md). Public records use right-handed,
Y-up coordinates, meters and seconds; source units are not automatically centered
or normalized to a viewer-sized model.

## Canonical support matrix

| Source feature | Imported representation and boundary |
| --- | --- |
| Scene selection and hierarchy | `SourceSettings.sceneIndex` selects a scene; `UINT32_MAX` uses the authored default, otherwise the first scene. With no scenes, root-node forests are used. Names, non-mesh ancestors, matrix/TRS transforms and shared mesh instances are retained. Selecting roots does not promise pruning all unused source resources. |
| Primitive modes | Points, lines, line loops/strips, triangles, triangle strips/fans become canonical point, line-list or triangle-list primitives. Missing indices are generated. |
| Accessors | Sparse, interleaved, normalized and quantized input is decoded. `KHR_mesh_quantization` is supported. Invalid accessor types/counts, indices and buffer ranges fail validation. |
| Standard vertex data | Positions, normals, tangents, primary vertex colors and all authored UV sets are converted. Missing or unusable surface normals cause normal generation and, where needed, corner splitting. `sourceVertices` preserves the authored-vertex mapping. |
| UV and additional attributes | Sparse `TEXCOORD_n` numbers map to compact slots recorded in `vertexTexCoordSets`. Additional colors/custom attributes retain decoded values and component metadata in `extraVertexAttributes`; custom morph attributes retain their target index. Retention does not assign application-specific behavior. |
| Skins | Joint IDs, optional skeleton, inverse-bind matrices and complete paired `JOINTS_n`/`WEIGHTS_n` influence data are retained. Missing inverse-bind matrices use identity. The first four influences in `Vertex` are a compatibility view; primitive influence arrays are authoritative. |
| Morphs | Position, normal, tangent, UV and color deltas, extra colors, custom attributes and mesh/node default weights are represented. The consumer applies these data and skinning; import does not provide a runtime deformation player. |
| Core materials | Metallic/roughness PBR, alpha modes/cutoff, double-sided flag, base/normal/occlusion/emissive texture bindings and factors are retained. Color and data texture views preserve their distinct interpretations. |
| Samplers and transforms | Nearest/linear and mip filtering, wrap modes, arbitrary texture-coordinate sets and `KHR_texture_transform` are represented. Equivalent texture views and samplers are deduplicated. An active material binding requires the corresponding primitive UV set. |
| Cameras and punctual lights | Perspective/orthographic camera parameters and `KHR_lights_punctual` point/directional/spot records are imported. Point/spot intensity uses the glTF candela convention, directional intensity lux. No exposure or render pipeline is selected. |
| Variants and instancing | `KHR_materials_variants` names and per-primitive material mappings are retained. `EXT_mesh_gpu_instancing` TRS instances become canonical node/mesh relationships, including the source of morph weights. Arbitrary application-defined instancing systems are not implied. |
| Animation | STEP, LINEAR and CUBICSPLINE sampler values/times, node TRS and morph-weight channels are retained. Cubic in/value/out triples remain available; glTF curves are not resampled using FBX bake settings. |

These rows are implemented in [GltfAssetReader.cpp](../src/Importers/GltfAssetReader.cpp)
and the public [scene](../include/ZenAsset/Format/Scene.h),
[material](../include/ZenAsset/Format/Material.h) and
[animation](../include/ZenAsset/Format/Animation.h) records.

## Extension boundary

| Extension family | Processor behavior |
| --- | --- |
| `KHR_draco_mesh_compression` | Decodes triangular Draco meshes and converts their attributes/indices. This is not a general Draco point-cloud importer. |
| `EXT_meshopt_compression`, `KHR_meshopt_compression` | Decodes supported meshopt buffer modes, strides and filters before accessor conversion; malformed layouts fail. |
| `KHR_materials_unlit`, `emissive_strength`, `pbrSpecularGlossiness` | Retains the unlit flag, emission strength, or diffuse/specular/glossiness workflow fields and supported textures. Material names in this and the next row all use the `KHR_materials_` prefix. |
| `ior`, `specular`, `clearcoat`, `sheen`, `transmission`, `volume`, `iridescence`, `anisotropy`, `dispersion`, `diffuse_transmission` | Retains the corresponding canonical factors and available texture bindings. These records do not establish that a consumer renders the associated optical effect. |
| `KHR_materials_volume_scatter`, `KHR_materials_retroreflection` | The older draft volume-scatter `multiscatterColorFactor`, and retroreflection factor/texture are mapped. The renamed `KHR_materials_scatter` draft is not implemented. |
| `KHR_node_visibility`, `KHR_node_selectability`, `KHR_node_hoverability` | Retains boolean node properties. Interaction behavior belongs to the consumer. |
| `EXT_lights_image_based` | Retains intensity, rotation, nine irradiance coefficients and specular cube mip faces. Specular size must be a nonzero power of two, with six dimension-matching faces per mip and at most 32 levels. This uses explicit image faces, not a general cubemap-container loader. |
| `KHR_animation_pointer` | Compiles supported pointers once to typed `AnimationTarget` records. Material factors and supported texture transforms, node TRS/weights/flags, camera parameters, punctual-light properties and environment intensity/rotation are covered. Only weights support component addressing; unsupported or invalid pointers fail. Rotation/scale pointer targets on matrix-transform nodes are rejected. |
| `KHR_xmp_json_ld`, extras, optional unknown extensions | Source JSON remains in `Scene.sourceDocument` for inspection. XMP is metadata retention, and unknown optional data has no automatic behavioral implementation. |

The exact pointer allowlist is in
[GltfAnimationBindings.cpp](../src/Importers/GltfAnimationBindings.cpp).
`sourceTarget` is diagnostic metadata; consumers can use typed targets without
interpreting source JSON. Import does not execute interactivity graphs.

Unknown **required** extensions fail with `UnsupportedFeature` and their names.
Deferred cases include Gaussian splatting, interactivity, IES profiles, manifold
semantics, float64 accessors and remaining vendor systems such as MSFT LOD/DDS
and packing extensions. Optional metadata preservation does not make any of
these features supported. The allowlist in the importer is authoritative.

## Images and dependencies

External local buffers/images, data URI contents and embedded GLB/buffer-view
images are consumed. Local external file revisions become `Scene.dependencies`;
changed or unidentifiable consumed revisions fail rather than silently forming
a mixed-revision scene. UTF-8 local paths and Windows file-URI normalization are
handled. No network-resource fetch service is provided by this API.

PNG/JPEG, `EXT_texture_webp` and `KHR_texture_basisu` KTX2/Basis content decode
through [ImageDecoder.cpp](../src/Importers/ImageDecoder.cpp). Ordinary material
images become RGBA8 pixels; Basis mip levels are retained as decoded RGBA8
levels, not preserved as original Basis bytes or automatically GPU-compressed.
The Basis path transcodes layer 0 / face 0. General KTX2 array/cubemap handling
and preservation of arbitrary high-bit-depth image precision are not claimed.
Unsupported/corrupt image data or failed transcoding fails the import; it does
not publish a texture without pixels.

Images are shared immutable allocations, while texture views carry sampler and
color/data interpretation. Referenced image decoding and mesh conversion may
use workers; results are assembled deterministically. Source import does not
perform optional atlas packing or BC texture cooking; see
[texture processing](TextureProcessing.md) for that separate stage.

## Diagnostics and resource limits

A failed import returns no asset. Read `status`, `error` and `diagnostics` from
[SceneImportResult](../include/ZenAsset/Processor/Import.h), rather than treating
a nonempty source document as success. Invalid scene selection, malformed
geometry, invalid texture references, missing required UV sets, unsupported
animation pointers and invalid environment-light data fail by name.

The common [post-import pass](../src/Processing/SceneAssetProcessing.cpp) has
explicit recovery behavior: mismatched mesh/node default morph weights are
cleared with warnings, and an invalid animation clip is skipped individually
with its reason. Other invalid canonical scene data fails validation. These
warnings can accompany an otherwise successful import; callers needing strict
fidelity should inspect them.

Current bounds are distinct:

- `SceneImportContext.maxSourceBytes` defaults to 16 GiB and preflights the root
  glTF/GLB file. It is **not a cumulative external-buffer/image byte budget** for
  this backend.
- `maxDecodedImageBytes` defaults to 16 GiB of cumulative decoded/converted image
  allocation requests, including temporary copies. It is neither encoded-file
  size nor a process-RSS limit. Parser/codec-private allocations are separate.
- Image dimension arithmetic and decoder limits are checked. KTX2 input is
  limited to `UINT32_MAX` encoded bytes, 1-32 mip levels and a base RGBA pixel
  count fitting `UINT32_MAX` bytes. stb input must fit its signed-int size.
- `maxGeometryBytes` defaults to 16 GiB and admits processor-owned geometry,
  skin/morph/custom-attribute/animation buffers and conversion scratch before
  allocation. glTF retains conservative capacity-growth charges through the
  import, including old/new buffer overlap and simultaneous worker-result and
  canonical destination storage. This allowance can exceed both final payload
  bytes and peak live buffer bytes. Parser/codec-private allocations, allocator
  bookkeeping, general scene metadata and graph-validation scratch are excluded;
  it is not a process-RSS limit. Exceeding the allowance returns `ResourceLimit`
  without an asset. `maxObjGeometryBytes` remains specific to OBJ.
- Canonical scene validation also applies a separate 16 GiB runtime-geometry
  bound after conversion. Raising the import allowance does not remove that
  validation limit.

Cancellation is cooperative at import/conversion checkpoints, not an interrupt
inside every third-party decoder call. Callback functions may execute on worker
threads and must obey the public callback lifetime/thread-safety contract.
Physical-file progress counts distinct external files; embedded images and
derived textures are not additional files.

## Verification scope

Standalone `ZenAssetImportFixtures`, `ZenAssetImportGltfLimits`,
`ZenAssetFileProgressFixtures` and `ZenAssetCookFixtures` cover owned glTF/GLB
fixtures, typed animation targets, retained attributes, limits/cancellation and
cooked package round trips. The source-backed matrix is broader than those few
standalone smoke fixtures; it is not a claim that every exporter/extension
combination has been exhaustively tested.

Additional historical semantic/consumer tests are owned under
[tests/integration/zenengine](../tests/integration/zenengine/README.md) and are
opt-in. Their rendering results are not part of this standalone import contract.
