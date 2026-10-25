# Processor implementation status

Checkpoint: 25 October 2026. The processor-scoped implementation and verification
work is complete for the Windows x64 configuration recorded below. The subsequent
[ZenEngine development integration](../../ZenEngine/Doc/ZenAssetProcessorIntegrationVerification.md)
passes Debug/Release consumption and focused Vulkan checks. Broader rendering and
release acceptance remain separate gates; the complete engine plan is not closed.

The latest tested production fingerprint is
`8976c628910f91106f0055657a85e9ace6afa3efaf6748639b6cd7804ca04a4f`.
Individual historical reports identify their own source/SDK fingerprints.

| Area | Current evidence |
| --- | --- |
| Source semantics and raw v3 packages | [Reference parity](ReferenceVerification.md): 360 accepted scenes, direct public-field round trips and deterministic rewriting; importer-specific exclusions remain explicit. |
| Lossless geometry/chunk storage | [Provoking vertices](ProvokingVertexVerification.md): actual compressed/remapped packages on two GPUs; [codec measurements](CodecMeasurementVerification.md): matched raw/Zstandard/mesh/combined read, size and process peaks. |
| Normal-map qualification | [Joint UV/LOD check](ContinuousJointNormalVerification.md): 37 Release and 37 Debug tests pass; 240 independent actual-payload cases cover over 94 million samples. The previous accepted bad image now retains its source chain. |
| Texture cooking cost | The joint checkpoint records three paired full cooks: median 31.757 to 44.296 seconds, with unchanged resource payloads. This is offline cost, not runtime performance. |
| Optional atlases | [Synthetic stage measurements](AtlasCookingMeasurements.md) and [natural workloads](AtlasWorkloadVerification.md) preserve matching inputs, require actual pages and separate exclusions from measurements. Packing can increase file size, cooking cost and process memory; it remains optional. |
| Independent consumption | [Final SDK checkpoint](ProcessorCompletionVerification.md): all eight original/relocated Debug/Release consumer checks pass, each relocated 320-file SDK matches exactly, and actual links have no hidden engine/dependency-build paths. |
| Additional formats and batch builds | [OBJ contract](ObjImport.md), [Assimp evaluation](AssimpEvaluation.md), [batch builds](BatchBuilds.md) and [CMake integration](BuildIntegration.md) describe the enabled paths and explicit limits. PLY/STL remain disabled. |

## Completed final processor work

- [Shared geometry admission](ImportGeometryBudgetVerification.md) now reserves
  processor-owned glTF/FBX geometry, deformation, animation and conversion storage
  before allocation, with explicit parser/RSS exclusions. Batch admission and CLI
  controls use the same policy; the [glTF/GLB](GltfImport.md) and [FBX](FbxImport.md)
  matrices describe it.
- [Separate pipeline measurements](StageMeasurementVerification.md) cover actual
  parse/conversion/cook/encode/write/read paths, with seven paired rounds, full
  scene/metadata parity and unchanged identities. Ordinary builds omit the recorder.
- The same parity check exposed and verified a correction for uninitialized BC6H
  lanes. Final independent HDR decoding passes 90 authored face/mip checks without
  changing the quality limit; the complete initialization fix preserves all tested
  HDR resource payloads from the preceding corrected checkpoint.
- [Final verification](ProcessorCompletionVerification.md) passes 40 Release and
  40 Debug standalone tests plus all eight installed/relocated SDK consumer checks.
  The import/package corpus covers 360 scenes at the unchanged importer/reader/raw
  writer source checkpoint. File progress counts distinct processed physical files
  against the discovered total, including the external-file consumer checks.

## Deferred integration and release gates

The [current ZenEngine integration checkpoint](../../ZenEngine/Doc/ZenAssetProcessorIntegrationVerification.md)
verifies the final live checkout in Debug/Release, source/cache/package behavior,
reader-only linkage, and 28 focused RTX 5080 texture/editor checks. The full
rendered raster/GI acceptance campaign remains open. Image-space normal/HDR
limits are not a rendered-frame tolerance. Atlas correctness and fewer native
images do not establish runtime draw/frame profitability. Historical engine
checkpoints retain their stated device and source scope.

Only Windows x64 Debug/Release is presently verified. Other-host claims, optional
portable profiles when required, release publication, and version pinning are
separate decisions. The development checkout remains unpinned and v3 remains an
unreleased schema. No general anisotropic normal-error certificate or expanded
spatial mip/NPOT/BC sampling profile is claimed.
