# Optional geometry cooking

`CookScene` keeps the imported scene immutable and applies requested processing
to an owning copy. Default settings preserve the input. The cooked scene is
validated before publication and is the same representation used for runtime
adaptation and package writing. Processing settings are recorded separately
from transport and semantic compression settings.

## Vertex fetch order

`optimizeVertexFetch` arranges each primitive's vertex storage in stable first
index-use order. Unused vertices follow in their original order. Primitive
order, corner order, point/line/triangle topology, material slots, animation
targets and material-variant bindings retain their meanings.

Primitives with identical vertex ranges share one permutation. Their index
ranges are merged before rewriting so shared indices are transformed once.
Partially overlapping vertex ranges retain their original data and produce
diagnostics identifying the mesh and primitive.

The same permutation applies to complete vertex records, every global UV set,
full joint-influence arrays, existing source-vertex maps, and custom attributes
whose domain is `PrimitiveVertex`. Authored morph and `SourceVertex` custom
arrays retain their original order. When these authored arrays rely on an
implicit identity map, cooking materializes a source-vertex map before
reordering. In-place permutation cycles avoid duplicating nested UV or joint
arrays during remapping.

## Position quantization

`positionQuantizationBits` is zero to disable quantization, or 1 through 24 to
round positions onto a uniform grid within each shared vertex range's bounds.
The canonical record remains a float position; optional mesh coding and
Zstandard compression operate on that cooked representation. Position `w`,
normals, tangents, UVs, colors and weights are preserved.

Candidate positions are checked against every static instance, including parent
transforms, nonuniform scales and affine base matrices. The accepted Euclidean
world-space error is at most:

```
max(positionMaxAbsoluteError,
    positionMaxRelativeError * originalWorldBoundsDiagonal)
```

Each instance must satisfy its own bound. Uninstanced meshes are checked in
identity space. A failed check retains the original positions for the entire
shared range and emits a diagnostic. Accepted ranges receive updated bounds.
Skinning, morphs, animated instance or ancestor transforms, and partially
overlapping vertex ranges conservatively retain their original positions with
diagnostics. Projective transforms also retain original positions because the
affine error check does not cover perspective division.

## Storage and cancellation

The cooking allocation budget includes the copied scene, persistent source maps
and diagnostic records, and the peak temporary range/remapping/quantization
storage. Immutable shared image pixels remain shared. Scratch storage is
released after each operation. Counts describe requested element storage rather
than allocator bookkeeping or fixed stack objects.

Long remapping and quantization loops poll cancellation. Failure or cancellation
does not publish the partially processed copy through `CookScene`.
