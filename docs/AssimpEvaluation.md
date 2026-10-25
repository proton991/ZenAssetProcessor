# Assimp PLY and STL evaluation

Assimp 6.0.5 can provide useful static PLY and STL geometry, but these backends
remain disabled in ZenAsset. A standalone parser probe met 14 of 16 fixture
expectations. Assimp's structure validator rejected a valid point-only PLY and
an STL containing an empty solid. A future adapter must handle those cases
explicitly and establish canonical units, materials, dependencies and limits
before either format is advertised as supported.

This evaluation uses the official
[6.0.5 source](https://github.com/assimp/assimp/tree/392a658f9c271be965271f45e7521a1b80ea4392),
commit `392a658f9c271be965271f45e7521a1b80ea4392`. The isolated Release build
enables only the PLY and STL importers. It is not part of ZenAsset's dependency
graph, installation, importer registry or runtime Reader. No Assimp binaries
or upstream model fixtures are redistributed by this change.

## Fixture results

The probe requests `aiProcess_ValidateDataStructure` for every case and enables
triangulation only where stated. Newly generated triangle fixtures have known
positions, normals and colors. Upstream fixtures are read directly from the
pinned checkout; their paths and SHA-256 hashes are retained in the results.

| Case | Observed result |
| --- | --- |
| Authored ASCII, little-endian and big-endian PLY triangles | Exact three positions, three normals and RGB vertex colors; one triangle each |
| Authored binary STL triangle | Exact positions and normal; one triangle |
| Invalid PLY index / truncated binary STL | Both rejected, as expected |
| Upstream ASCII STL triangle | Exact three positions |
| Upstream two-solid STL | Two named meshes and two root children preserved |
| Upstream STL with empty solid | Rejected by validation: empty mesh has no vertices |
| Upstream PLY point cloud | Rejected by validation: mesh has no faces |
| Upstream PLY cube with UVs | 24 vertices, normals and UVs; six quads before triangulation, twelve triangles afterward; inspected UVs preserved |
| Upstream binary PLY cube | Accepted with eight vertices and twelve triangles |
| Upstream float-color PLY | Accepted with a vertex-color channel |
| Upstream ASCII / binary Spider STL | Both accepted with 4,104 vertices and 1,368 triangles; matching counts do not establish exact geometry equivalence |

The two rejected reference fixtures remain failures in the evidence; the harness
does not relax its expectations to report a full pass. The other fourteen cases
include the two intentional malformed-input rejections. No ASAN/fuzzer, engine
adapter, package roundtrip or rendered-parity claim follows from this probe.

## Adapter decision

Start a future backend with a declared static subset rather than enabling every
Assimp importer. Preserve multiple solid nodes, supported primitive topology,
normals, colors and UV domains. PLY points need an explicit canonical point-index
path; empty STL solids need a documented empty-node/mesh policy before validation.
Removing the validator without equivalent canonical checks is insufficient.

Specify an input-unit convention, default material behavior and normal repair
policy. Enumerate supported PLY properties and diagnose discarded semantics;
this probe does not establish arbitrary custom-property preservation. Audit
external texture resolution through a dependency-tracking I/O layer before
accepting textured PLY. Binary STL color conventions also need fixtures:
the pinned importer selects a Materialise color path using a `COLOR=` header
marker, and this evaluation covers uncolored STL only.
[Pinned STL implementation](https://github.com/assimp/assimp/blob/392a658f9c271be965271f45e7521a1b80ea4392/code/AssetLib/STL/STLLoader.cpp)

Adoption also requires explicit allocation and cancellation boundaries around
Assimp's private parsing and postprocessing, conversion of parser diagnostics,
the license/dependency inventory, source-free package tests and engine acceptance.
These are concrete adapter requirements, not evidence that PLY/STL are already
part of ZenAsset's supported-format list. OBJ/MTL remains the delivered additional
format; glTF/GLB and FBX retain their existing backends.

## Reproduction and evidence

Obtain the pinned Assimp checkout separately and run from the ZenAsset root:

```powershell
cmake -S tools/evaluation -B build/assimp-evaluation/build -G Ninja `
  -DCMAKE_BUILD_TYPE=Release -DASSIMP_SOURCE_DIR=<absolute-assimp-source>
cmake --build build/assimp-evaluation/build --target AssimpProbe -j 2
python tools/evaluation/run_assimp.py `
  --probe build/assimp-evaluation/build/AssimpProbe.exe `
  --source <absolute-assimp-source> --output build/assimp-evaluation/verified
```

The recorded Windows MSVC Release run is in
`build/assimp-evaluation/verified/results.json`, with configure/build logs in
its parent. The runner exits nonzero while either reference expectation fails.
Probe SHA-256:
`a4ef2645ff84c3cf3c8d2866be913e1425affbd6b4caa674cabba9b8c9914c24`.
An initial sparse checkout omitted `cmake-modules`; fetching that directory
resolved configuration without modifying Assimp source.
