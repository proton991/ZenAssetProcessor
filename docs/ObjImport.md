# Wavefront OBJ / MTL import

`ZenAsset::Processor` registers `.obj` (case insensitive) as source format `obj`, importer version 1. `zenassetc cook model.obj --output model.zenasset` uses the same import, cook, validation and package writer as glTF and FBX. ZenEngine's source model discovery and file selector consume that registry. Runtime package readers acquire no OBJ parser dependency.

The backend uses the standard checked [tinyobjloader parser](https://github.com/tinyobjloader/tinyobjloader/tree/45636bdcef1a4fec140346b90c0b50bf0bc3e23b) and [mapbox earcut.hpp](https://github.com/mapbox/earcut.hpp/tree/4811a2b69b91f6127a75e780de6e2113609ddabb), pinned by commit and archive SHA-256. Their MIT and ISC notices are installed with the processor. Both are private build dependencies, with prefixed symbols and no parser headers in the installed public SDK.

## Geometry boundary

| Input | Canonical result |
|---|---|
| `v`, optional RGB vertex colors | Float32 positions and colors; no axis or unit conversion. OBJ lacks a standardized unit/up-axis declaration, so authored values are interpreted as right-handed, Y-up meters. |
| `vn` | Authored finite normals are retained, including their magnitude. |
| `vt` | UV0; V becomes `1-v` to match decoded images. A third coordinate must be zero. |
| Positive and relative negative indices | Checked independently for positions, UVs and normals; source position IDs remain in `sourceVertices`. |
| Triangles | Original corner order is retained. |
| Simple planar polygons, either winding | Validated for planarity and self-intersection, projected onto the dominant plane and triangulated. Triangle winding and total area are checked. Original corner attributes remain attached. |
| `l` polyline | Consecutive line segments in authored order. |
| `p` | Point order is retained. |
| `o`, `g`, `usemtl` | Object parents and contiguous group/material/topology runs. Multiple group names are retained together in the run name. Faces, lines and points retain source order and material assignment. |
| Missing normals and `s` | Smoothing-off faces use their polygon normal; smoothing groups average incident unit polygon normals across group/material boundaries within the object. Authored normals take precedence. |
| Comments and OBJ line continuation | Accepted. |

Vertices are expanded per authored polygon/line/point corner. Independent OBJ position, normal and UV indices therefore survive seams without aliasing unrelated attributes. Optional cook-time vertex-fetch optimization can change storage later.

The importer rejects free-form curves/surfaces, skin-weight extensions, non-unit homogeneous position weights, projective texture coordinates, invalid indices, nonfinite attributes, empty scenes, self-intersecting or degenerate polygons, and polygons farther than `max(1e-7, diagonal * 1e-6)` from their plane. A primitive statement is limited to 4,096 corners. Nonplanar polygons have no unique authored triangle surface; this backend requires the source tool to triangulate them. The upstream Cornell box's nonplanar wall and weighted-position cube exercise these explicit rejection paths.

## MTL boundary

Legacy MTL is approximated by the canonical PBR material; each material reports that conversion. This does not claim a pixel match to a Phong renderer.

Materials are lit unless `illum 0` is explicitly authored. A color map with no corresponding `Kd`, `Ks` or `Ke` uses a multiplier of one; explicitly authored zero factors remain zero. Likewise, `map_Pr` and `map_Pm` use factor one when their corresponding scalar factor is absent.

| MTL property | Mapping |
|---|---|
| `Kd`, `d`, `Tr` | Base color and alpha; tinyobj applies `d` precedence when both opacity forms are present. Transparent material uses alpha blending. |
| `Ke` | Emissive color. |
| `Ks`, `map_Ks` | PBR specular color and color texture. |
| `Ns`, `map_Ns` | Roughness `sqrt(2/(Ns+2))`; a scalar map modulates the exponent before conversion. |
| `Ni` | IOR when at least one; otherwise the canonical 1.5 default. |
| `Pr`, `Pm`, `map_Pr`, `map_Pm` | Roughness and metalness extension factors/maps; packed into the canonical green/blue channels. |
| `map_Kd`, `map_Ke` | Base/emissive color maps; sRGB unless explicitly declared linear. |
| `map_d` | Scalar opacity multiplied into base-map alpha. |
| `norm` | Explicit tangent-space RGB normal extension; V reversal also reverses the green channel. `-bm` maps to normal strength and is rejected on other supported map slots. |
| `-o`, `-s`, `-clamp` | UV offset/scale and repeat/clamp wrapping, conjugated by the V reversal. |
| Scalar `-imfchan r/g/b/m/l` | Red/green/blue/alpha/luminance channel. Without `-imfchan`, scalar maps use luminance. |

Packed maps must have identical dimensions, transforms and wrapping. Incompatible maps fail with `UnsupportedFeature`; the importer does not silently resample or collapse different UV mappings. Packed images retain the base level, and the cook can generate their mip chains. Scalar and normal maps require linear samples. Unsupported texture transforms/options are explicit failures.

Height `bump`/`map_bump` is diagnosed and ignored; it is never relabeled as a normal map. Ambient, displacement, reflection and sheen maps, transmission/coat/sheen/anisotropy factors, and unrecognized material parameters are diagnosed. Procedural/spectral material inputs are unsupported. Duplicate material names fail to avoid ambiguous binding. An unknown `usemtl` uses the default material with a diagnostic; a missing authored material library or required supported texture fails the import.

Each name on an `mtllib` statement loads a library, including escaped spaces. Libraries resolve beside the OBJ. A relative texture first resolves beside its MTL, then beside the OBJ. Missing preferred paths are retained as `exists=false` dependencies so their later appearance invalidates the engine cache. Consumed files carry size and modification stamps; cache publication adds content hashes. External network/device paths are rejected unless they lie within the source's own directory.

## Limits and verification

`SceneImportContext.maxSourceBytes` bounds the cumulative OBJ, MTL and encoded texture bytes consumed by this backend. `maxObjGeometryBytes` bounds conservative normalized-text, corner, run, smoothing and material storage estimates; it does not change glTF/FBX limits. `maxDecodedImageBytes` applies to decoded and converted image buffers. Parser allocator overhead is outside the exact byte accounting, but source size and count preflight bound parser expansion. These are allocation-request limits, not total process RSS guarantees. Cancellation is checked while reading files, scanning statements, processing polygons and converting pixels. Failure publishes no scene.

`ZenAssetObjImportTests` covers mixed topology and material order, both concave windings and area, negative indices, authored normals/UVs, smoothing, multiple MTLs, asymmetric 2×2 image orientation, scalar packing and explicit normal conversion, missing-preferred texture dependencies, deterministic import, lossless meshopt+zstd package round trip, malformed polygons/indices, limits and cancellation. Engine `SceneImporter.ObjSourceCacheAndExplicitPackagePreserveTopologyAndMaterialDependencies` exercises discovery, source/cache/package equality, MTL invalidation and source-independent loading.

This backend is an initial bounded OBJ/MTL subset. It does not add animation, skeletons, morphs, scene variants or an atlas stage to OBJ.
