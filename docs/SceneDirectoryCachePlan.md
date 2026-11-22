# Per-model directory cache and parallel scene processing

Proposed for ZenAssetProcessor and ZenEngine. Each source model owns one named directory under `E:\Dev\ZenEngine\Cache\SceneAssets`. The engine can open a complete scene through one manifest. Geometry and image payloads live alongside it and can be cooked, reused and loaded independently.

## What appears on disk

Importing `E:\Dev\main_sponza\NewSponza_Main_glTF_003.gltf` creates the following layout.

```text
Cache/SceneAssets/
  NewSponza_Main_glTF_003/
    source.json
    reference-<settings-key>.zenasset
    geometry/
      mesh-<digest>.zenpart
    textures/
      image-<digest>.zenpart
      ...
```

- Use the source filename stem as the directory name. Preserve Unicode, sanitize names for Windows, and store the normalized source identity in `source.json`. If two different sources claim the same name, keep the existing directory and append a stable source-path hash to the newcomer. Resolve existing ownership before claiming a folder, including concurrent first imports and case-insensitive collisions.
- Give each import configuration its own manifest in the same model directory. The key covers the import settings, selected glTF scene, processing profile and processor identity. Editing a source in place republishes that variant at the same path. Parts with identical encoded contents can be shared by variants of this model. A custom cache root uses the same layout.
- The manifest contains the scene graph, small records, build metadata, source dependencies and an index mapping existing resource IDs to parts. The geometry part contains mesh records and vertex, index, skin, morph and custom attribute streams. Each distinct cooked image gets one part containing all its mips and array layers. Materials, texture views and samplers keep their existing references, so several views can use one image.
- Give the manifest a versioned required format marker. Reuse the current resource encodings, limits and checksums inside parts. Part references are relative to the model directory, stay inside it after resolution, and carry the expected identity and sizes. A directory with its manifest and referenced parts can be moved and opened without the source model. The multi-model `.zenscene` assembly format keeps its existing role.

## Implementation order

1. **Establish the format in ZenAssetProcessor.** Add directory output as an explicit `zenassetc cook` mode. Start with a serial writer and reader. Extend `inspect` to report parts and byte totals, and `validate` to check the complete scene. Share the existing [resource traversal](../src/Format/SceneResources.h) and scene validation across both layouts. Read the same fixture through each layout and compare geometry, materials, animation, image pixels, mips and bounds before introducing concurrency.
2. **Cook and save with bounded parallelism.** Once the importer establishes indices and dependencies, schedule geometry work and independent image jobs on the existing worker pool. A free worker takes the next ready job. Each worker encodes, compresses and writes its own temporary part. Keep stable resource IDs and manifest ordering regardless of completion order. Bound workers by both the caller's worker limit and reserved bytes for input, output and codec scratch. A large image can use the full available budget on its own.
3. **Reuse unchanged work on recook.** Record the inputs and settings that affect each output. Look up reusable parts before expensive decoding or compression. A mesh-only edit should preserve every unaffected image file. A texture-only edit should preserve geometry and other independent images. Include texture usage, mip generation, compression and atlas or array membership in the relevant keys. When the importer cannot prove a narrower dependency, rebuild the affected group. Keep the current source-content freshness checks, including edits that preserve size and timestamp.
4. **Publish and evict complete scenes.** Stage new parts inside the model directory, verify them, then atomically replace the variant manifest last. Coordinate concurrent publishers and recheck input freshness before replacement. Cancellation, a failed write or a process exit must leave the previous manifest usable. Pin the generation used by an active reader or deferred image lease. Account for each physical part once across variants. Eviction removes old manifests and then unreferenced parts after active readers and writers release them. Extend the existing cache budget and cleanup path to cover both layouts.
5. **Parallelize cached reads in ZenAssetProcessor.** Read the manifest first, then dispatch geometry and individual images to the worker pool. Independent file handles let workers read, verify and decompress their own parts. Preallocate result slots by resource ID, bound all in-flight buffers against one read budget, and join before validating and publishing the scene graph. Preserve the eager reader's resident pixels and the deferred reader's payload validation and lease lifetime. A corrupt or missing required part must identify the failing part and prevent a partial scene from being returned.
6. **Connect ZenEngine and the editor.** Let the scene cache select directory mode and pass the manifest path through cache lookup, direct package opens, editor Run and ZenApp's `.zenrender` `packagePath`. Any code that copies a scene for a run must carry its referenced parts or retain the original generation for the run. On the first directory-mode open, convert a valid flat cache into the named directory and publish its manifest before returning the new cache path. Stream or resolve its payloads with the same byte budget. A stale entry goes through the normal source cook. Direct opens of existing flat packages remain supported. Report completed geometry and image jobs to the editor, with byte progress for large transfers and distinct validation and graphics-preparation stages.

## Verify on New Sponza

The comparison point is the optimized single-file implementation at ZenAssetProcessor `51a0494` and ZenEngine `30c423a3`. New Sponza has 115 meshes and 72 images. Its image records occupy 2,322,745,007 of the 2,395,363,740 stored bytes. The measured release medians were **11.09 s** for a fresh cook, **4.88 s** for ZenApp's CPU scene load and **12.07 s** until ZenApp was ready. These are the baselines for the proposed work.

| Case | Evidence to collect |
| --- | --- |
| Fresh cook | Compare the existing file, serial directory mode and parallel directory mode. Use the same source, reference profile, Zstandard level 3 and meshoptimizer settings. Record wall time, peak memory, total stored bytes, allocated disk space and file count. |
| Cached load | Compare the layouts with 1, 2, 4 and 8 workers. Use the release `build/x64-windows-msvc-release/bin/ZenApp.exe` with `--setup=<file.zenrender> --frames=1 --windowed --width=1280 --height=720 --async-compute=0 --backend=compute --no-ui`. Record CPU scene load, time until ready and peak memory. Match the earlier warm-up and three timed runs. Report any cold-cache run separately. |
| Incremental recook | On a scratch copy, change geometry only, then one external texture only. Record elapsed time, which jobs ran, bytes written and which part files were reused. Run an unchanged cook and check that it rewrites no payloads. |
| Compatibility and recovery | Exercise same-name sources, two variants, a moved directory, first-use migration and direct opens of legacy files, corrupt or missing parts, cancellation, concurrent recooks and a deferred read held through recook and eviction. Compare rendered output with the same camera and lighting. |

Keep executable, setup and input hashes, exact arguments, worker counts and actual render configuration with the results. Compare a small fixture as well to expose per-file overhead. Ship directory mode as a selectable layout. Judge its speed against both the single-file baseline and the single-worker directory run, and report the memory and space tradeoffs with the result. Publish the processor changes from the ZenEngine submodule checkout first, then update the engine integration and its submodule pointer.
