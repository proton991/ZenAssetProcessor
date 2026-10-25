# Canonical CPU scene contract

`zenasset::Scene` in `ZenAsset/Format/Scene.h` owns the processor's canonical
CPU representation. It preserves the currently supported glTF/FBX semantics
before optimization. It is neither a native-memory file format nor a mutable
runtime scene. Package serialization must write fields explicitly, independent
of C++ padding, pointers, container layout, and host byte order.

## Ownership and public boundary

The initial distribution is a C++20 static library, with source compatibility
rather than a stable cross-compiler DLL ABI. The records use standard C++
containers and independent float math records. Public headers need no ZenEngine,
GLM, Vulkan, parser, or encoder headers.

A scene owns its strings and vectors. Copying a scene copies those containers;
`ImageAsset::pixels` shares immutable `ImagePixels` through `std::shared_ptr`.
Processor code constructs mutable pixels before publishing a const shared owner.
Holding an image owner keeps its bytes alive independently of the scene. Image
bytes cannot point into a parser or mapped source file. Borrowed spans/references
into scene vectors require the scene to remain alive and unmodified. Runtime
adapters must copy or retain the relevant owner before asynchronous use.

Typed resource IDs are `uint32_t` indices into a scene's corresponding vectors.
`UINT32_MAX` means an absent optional reference, not an index. These IDs are local
to one scene and do not provide persistent editor identities. Historical signed
references retain `-1` for absence where explicitly documented below.

All enums specify `uint32_t` storage. `PixelFormat` is library-owned; adapters
must map it explicitly to device formats. An enum declaration describes storage,
not availability of an encoder or a GPU feature.

## Geometry and hierarchy

Coordinates are right-handed, Y-up, in meters. Time is seconds; angles are
radians. `Mat4` contains four columns and is indexed `[column][row]`. `Quat`
names its components `w,x,y,z` and constructs in that order; sampled quaternion
animation values retain the source conversion contract of `x,y,z,w`.

Nodes retain parent/child references, names, mesh and per-instance material
bindings, skins, cameras, lights, visibility/selectability/hoverability, and
instance morph-weight sources. Local transforms are `T * R * S * baseMatrix`,
preserving affine/sheared nodes. `renderMesh=false` allows an instancing parent
to carry mesh state without an extra draw. Roots select the active scene while
the full definition graph remains available.

Meshes contain primitive records; primitives address shared scene vertex/index
arrays with ranges. Indices are absolute indices into `Scene::vertices` and must
stay within the primitive's vertex range. Topologies include points, lines, and
triangles. Bounds are local axis-aligned extrema. Empty bounds begin with
`min=FLT_MAX`, `max=-FLT_MAX`; `SetMin` and `SetMax` expand those extrema.

The canonical vertex retains position, normal, tangent, two conventional UVs,
first-four joint indices/weights, and vertex color. Complete primitive influence
arrays preserve additional skin influences. `sourceVertices` maps final vertices
to authored vertices for morphs/custom attributes. `vertexTexCoordSets` maps
compact UV stream slots to authored set numbers; `vertexTexCoords` can retain
sets beyond UV0/UV1.

Skins retain their skeleton index (`-1` absent), node-index joints, and inverse
bind matrices. Morphs retain position/normal/tangent deltas, UV sets, colors,
additional color sets, and default weights. A morph primitive index of
`UINT32_MAX` means shared control-point data for every primitive in that mesh.
Custom attributes retain source component metadata, normalization, doubles,
source-vertex versus primitive-vertex domains, and optional morph-target indices
(`-1` identifies base attributes). No geometry optimization may discard or
remap these streams independently of the vertex data.

## Materials and images

Materials preserve metallic/roughness and specular/glossiness workflows, alpha
mode/cutoff, sidedness, emissive/unlit state, core texture slots, per-slot UV
sets/transforms, and all existing extension factors and bindings: IOR,
dispersion, specular, clearcoat, sheen, transmission, volume, iridescence,
anisotropy, diffuse transmission, multiscatter color, and retroreflection.
`MaterialParametersT` and `CopyMaterialParameters` let adapters resolve texture
references without maintaining a second list of factor-copy operations. Legacy
`m_p...Texture` fields hold typed IDs here, never pointers.

Texture resources separate image storage, view format, and sampler. Compatible
RGBA8 linear/sRGB and BC7 linear/sRGB views may share storage. `linearSource`
retains the source texture relation and `flatNormal` retains generated-flat-normal
semantics. Unspecified sampling fields use explicit sentinel enum values.

`ImagePixels::bytes` contains tightly packed base-level bytes. If authored mip
data is present, `mips[0]` is also level zero, and subsequent entries halve each
dimension down to at least one texel; a partial chain is permitted. This CPU
shape preserves existing importer behavior. A cooked package may deduplicate
the base level without changing its meaning. Compressed mip sizes round each
dimension up to whole blocks, including tiny mips. `GetImageLevelByteSize`
rejects undefined/unknown formats, zero dimensions, and arithmetic overflow;
callers must additionally apply allocation limits before allocating.

`ImagePixels::arrayLayers` defaults to one. Each level of an array page stores
complete tightly packed layers consecutively in ascending order. Array pages
require explicit mip levels, including mip zero. `TextureAsset::arrayLayer`
selects a layer and defaults to zero; its image ID, sampler, interpretation and
authored material UV transform retain their existing meanings. Validation checks
every level's complete layer footprint and rejects out-of-range layer references.
`GetImageArrayLevelByteSize` checks multiplication, and `GetImageLayerMipBytes`
provides a nonowning view of one validated-size level/layer without decoding or
copying. The latter does not validate compressed block contents.

`TextureAsset::atlasRegion` is an integer `ImageRegion` (`x`, `y`, `width`,
`height`) separate from the authored UV transform. All zero denotes the full
image. Nonzero values describe the logical tile inside a shared physical page.
The current spatial contract is one explicit RGBA8 level with power-of-two
tile/page extents and explicit linear/clamp sampling. `IsValidImageRegion`
provides overflow-safe rectangle containment; scene validation additionally
enforces the spatial policy. Region previews take the complete physical mip and
crop it without allocating a full-page decoded copy. See
[SpatialTextureRegions.md](SpatialTextureRegions.md) for wire versions, previews
and bounded runtime integration, and [SpatialCooking.md](SpatialCooking.md) for
optional scene-level packing and explicit exclusions.

Material variants retain one optional material index per named variant and
mesh primitive; `-1` keeps the primitive's default material. Material slots on
nodes support different material assignments for shared meshes.

## Animation, cameras, lights, and metadata

Clips retain typed node/material/camera/light/environment targets, component
addressing for morph weights, texture slot addressing, source target text for
diagnostics, and bake settings. Samplers retain step/linear/cubic interpolation,
times, component counts, and values. Cubic values store incoming tangent, value,
outgoing tangent triples for each key. Playback must not parse source target
strings. The node/path fields remain alongside typed targets for compatibility
with the existing conversion and validation rules.

Cameras retain perspective/orthographic parameters, infinite far and fixed
aspect flags. Punctual light intensities use candela for point/spot and lux for
directional lights. Image-based lighting retains orientation/intensity,
irradiance coefficients, and linear HDR RGBA specular mip faces in
`mip*6 + (+X,-X,+Y,-Y,+Z,-Z)` order. `imageBasedLight=-1` means no selected IBL.

Source path, source document, and dependency revisions are provenance. Missing
preferred dependencies remain represented with `exists=false` so their later
appearance can invalidate processing. File timestamps do not replace content
validation. Reproducible package payloads must separate machine-specific
provenance from semantic resource data.

## Validation boundary

These records do not make arbitrary input valid. Importers and package readers
must validate references, hierarchy cycles, finite values, skin/morph/animation
domains, image byte counts, and allocation budgets before runtime adaptation.
`ValidateSceneAsset` in `ZenAsset/Format/Validation.h` implements the shared
semantic scene checks. `CookScene` validates both its input and result, the writer
validates before publication, and `ReadScene` validates the decoded scene before
returning it. Individual storage helpers and `OpenPackage` do not replace that
scene-level validation; callers constructing records directly must validate them
before consuming their references or payloads.
