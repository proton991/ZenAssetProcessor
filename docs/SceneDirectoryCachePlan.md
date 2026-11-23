# Per-model directory cache and parallel scene processing

ZenAssetProcessor and ZenEngine support a selectable directory layout. With this layout each source model owns one named directory under `E:\Dev\ZenEngine\Cache\SceneAssets`. The engine can open a complete scene through one manifest. Geometry and image payloads live alongside it and can be cooked, reused and loaded independently.

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

- Use the source filename stem as the directory name. Preserve Unicode, sanitize names for Windows, and store the normalized source identity in `source.json`. If the name belongs to another source or an occupied export folder, append a stable source-path hash to the new cache directory. Later imports reuse that directory even if the original name becomes available. Resolve existing ownership before claiming a folder, including concurrent first imports and case-insensitive collisions.
- Give each import configuration its own manifest in the same model directory. The key covers the import settings, selected glTF scene, the full processing policy, transport codec, Zstandard level, meshoptimizer setting and processor identity. Editing a source in place republishes that variant at the same path. Parts with identical encoded contents can be shared by variants of this model. A custom cache root uses the same layout.
- The manifest contains the scene graph, small records, build metadata, source dependencies and an index mapping existing resource IDs to parts. The geometry part contains mesh records and vertex, index, skin, morph, texture-coordinate and custom attribute streams. Each distinct cooked image gets one part containing all its mips and array layers. Materials, texture views and samplers keep their existing references, so several views can use one image.
- Give the manifest a versioned required format marker. Reuse the current resource encodings and checksums. Enforce file, directory, resource-count and decoded-byte limits across the entire scene before dispatch. Keep per-chunk limits and the shared allocation limit during decoding. Part references are relative to the model directory, stay inside it after resolution, and carry the expected identity and sizes. A directory with its manifest and referenced parts can be moved and opened without the source model. The multi-model `.zenscene` assembly format keeps its existing role.

## Using the directory layout

The editor stores processed source models in named directories under `Cache/SceneAssets` by default. Clear **Store cache in a model directory** in the source processing settings to select a flat file. The standalone CLI and native source API retain their flat defaults. CLI callers select `--layout directory` and native callers set `SceneImportRequest::directoryCache`. Editor Run uses the loaded scene in process. A standalone ZenApp setup can use the published manifest as its `packagePath`.

```powershell
$cook = zenassetc cook model.gltf --output Cache/SceneAssets --layout directory --profile reference --compression zstd --zstd-level 3 --mesh-compression meshopt --workers 8 | ConvertFrom-Json
zenassetc inspect $cook.output
zenassetc validate $cook.output --read-workers 4
ZenApp --setup=scene.zenrender --read-workers=8
```

The cook response supplies the manifest path, encoded and reused image counts, geometry reuse and staged payload bytes. Inspect reports the manifest plus unique referenced part bytes. To move a generation, copy the manifest and its listed parts under the same relative paths.

## Implementation decisions

The directory layout uses the 88-byte entry marker in the v3 container. Flat packages use 64-byte entries. [Binary layout and integrity checks](PackageContainer.md#directory-backed-scenes).

The writer and reader reserve memory for concurrent jobs and retry with serial access when a job needs the full allowance. Geometry runs on the calling thread while workers claim independent images. Resource IDs fix the order of the manifest and the scene's output arrays.

For reference glTF imports, the source-image receipt allows a matching previously cooked image to supply the pixels. Desktop processing reevaluates material usage, vertex and morph alpha, indexed UVs, and page membership. The writer then looks up the resulting canonical image before transport compression. An unchanged desktop file can therefore be reused even when its processing work runs again.

Each model has a publisher lock and a separate reader gate. The reader gate pins the whole model while a scene or deferred image holds its package. A portable copy without the gate acquires a read-only handle to the model directory. Cleanup honors both kinds of reader. A warm directory lookup reads metadata and scene data from one open package. Trimming can reclaim stale variants and orphaned parts after the last pin is released. The flat-to-directory migration requires the current engine and processor fingerprint and copies encoded records one at a time.

## Implementation sequence

1. **Establish the format in ZenAssetProcessor.** Add directory output as an explicit `zenassetc cook` mode. Start with a serial writer and reader. Extend `inspect` to report parts and byte totals, and `validate` to check the complete scene. Share the existing [resource traversal](../src/Format/SceneResources.h) and scene validation across both layouts. Read the same fixture through each layout and compare geometry, materials, animation, image pixels, mips and bounds before introducing concurrency.
2. **Cook and save with bounded parallelism.** Once the importer establishes indices and dependencies, schedule geometry work and independent image jobs on the existing worker pool. A free worker takes the next ready job. Each worker encodes, compresses and writes its own temporary part. Keep stable resource IDs and manifest ordering regardless of completion order. Bound workers by both the caller's worker limit and reserved bytes for input, output and codec scratch. A large image can use the full available budget on its own.
3. **Reuse unchanged work on recook.** Record the inputs and settings that affect each output. Look up reusable parts before expensive decoding or compression. A mesh-only edit should preserve every unaffected image file. A texture-only edit should preserve geometry and other independent images. Include texture usage, mip generation, compression and atlas or array membership in the relevant keys. Determine masked-texture preservation from vertex and morph alpha, and page eligibility from the indexed UV domain before accepting a desktop image hit. Compare these edits against clean desktop cooks. When the importer cannot prove a narrower dependency, rebuild the affected group. Keep the current source-content freshness checks, including edits that preserve size and timestamp.
4. **Publish and evict complete scenes.** Stage new parts inside the model directory, verify them, then atomically replace the variant manifest last. Coordinate concurrent publishers and bind each job to the bytes consumed by its importer and recheck those revisions before replacement, including edits that preserve file size and timestamp. Cancellation, a failed write or a process exit must leave the previous manifest usable. Pin the generation used by an active reader or deferred image lease. Account for each physical part once across variants. Eviction removes old manifests and then unreferenced parts after active readers and writers release them. Extend the existing cache budget and cleanup path to cover both layouts.
5. **Parallelize cached reads in ZenAssetProcessor.** Read the manifest first, then dispatch geometry and individual images to a bounded worker pool available in the standalone reader SDK. Expose the reader worker count to ZenApp and the CLI. Independent file handles let workers read, verify and decompress their own parts. Preallocate result slots by resource ID, bound all in-flight buffers against one read budget, and join before validating and publishing the scene graph. Preserve the eager reader's resident pixels and the deferred reader's payload validation and lease lifetime. A corrupt or missing required part must identify the failing part and prevent a partial scene from being returned.
6. **Connect ZenEngine and the editor.** Let the scene cache select directory mode and pass the manifest path through cache lookup, direct package opens, editor Run and ZenApp's `.zenrender` `packagePath`. Any code that copies a scene for a run must carry its referenced parts or retain the original generation for the run. On the first directory-mode open, convert a valid flat cache from the same producer fingerprint (including flat mode of the new binary) into the named directory and publish its manifest before returning the new cache path. Stream or resolve its payloads with the same byte budget. Earlier producer fingerprints follow normal cache invalidation. A stale entry goes through the normal source cook. Direct opens of existing flat packages remain supported. Report completed geometry and image jobs to the editor, with byte progress for large transfers and distinct validation and graphics-preparation stages.

## Verify on New Sponza

The comparison point is the optimized single-file implementation at ZenAssetProcessor `51a0494` and ZenEngine `30c423a3`. New Sponza has 115 meshes and 72 images. Its image records occupy 2,322,745,007 of the 2,395,363,740 stored bytes. The measured release medians were **11.09 s** for a fresh cook, **4.88 s** for ZenApp's CPU scene load and **12.07 s** until ZenApp was ready. These are the historical baselines for comparison.

| Case | Evidence to collect |
| --- | --- |
| Fresh cook | Compare the existing file, serial directory mode and parallel directory mode. Use the same source, reference profile, Zstandard level 3 and meshoptimizer settings. Record wall time, peak memory, total stored bytes, allocated disk space and file count. |
| Cached load | Compare the layouts with 1, 2, 4 and 8 workers. Use the release `build/x64-windows-msvc-release/bin/ZenApp.exe` with `--setup=<file.zenrender> --frames=1 --windowed --width=1280 --height=720 --async-compute=0 --backend=compute --no-ui`. Record CPU scene load, time until ready and peak memory. Match the earlier warm-up and three timed runs. Report any cold-cache run separately. |
| Incremental recook | On a scratch copy, change geometry only, then one external texture only. Record elapsed time, which jobs ran, bytes written and which part files were reused. Run an unchanged cook and check that it rewrites no payloads. |
| Compatibility and recovery | Exercise same-name sources, two variants, a moved directory, first-use migration and direct opens of legacy files, corrupt or missing parts, individually admissible parts exceeding a scene-wide limit, a reader-only installed consumer, cancellation, concurrent recooks and a deferred read held through recook and eviction. Compare rendered output with the same camera and lighting. |

Keep executable, setup and input hashes, exact arguments, worker counts and actual render configuration with the results. Compare a small fixture as well to expose per-file overhead. Ship directory mode as a selectable layout. Judge its speed against both the single-file baseline and the single-worker directory run, and report the memory and space tradeoffs with the result. Publish the processor changes from the ZenEngine submodule checkout first, then update the engine integration and its submodule pointer.

## Release results on New Sponza

Measured on 23 November 2026 with a Ryzen 9 9950X3D, RTX 5080, driver 621.35 and Windows 11 build 26300. New Sponza cook times are medians of three new outputs. The worker count applies to source import as well as package encoding. Load numbers are medians of three launches after one warm-up per layout and worker count. Runs used the operating system file cache without an explicit flush. Peak memory is the median process peak working set. [Run arguments, executable and input hashes, setups, and individual samples](SceneDirectoryCacheMeasurements.json).

| Fresh cook | Wall time | Peak GiB | Logical bytes | Allocated bytes | Files |
| --- | ---: | ---: | ---: | ---: | ---: |
| Flat, 8 workers | 11.26 s | 6.91 | 2,395,363,740 | 2,395,365,376 | 1 |
| Directory, 1 worker | 38.05 s | 6.91 | 2,395,375,456 | 2,395,529,280 | 77 |
| Directory, 8 workers | 7.66 s | 6.91 | 2,395,375,456 | 2,395,529,280 | 77 |

File counts include each model’s manifest, parts, ownership receipt and two lock files. The cache root also has one shared ownership lock. Allocation uses Windows FileStandardInfo.

| Read workers | Flat CPU load | Directory CPU load | Flat ready | Directory ready | Flat peak GiB | Directory peak GiB |
| ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 1 | 4.89 s | 4.86 s | 12.10 s | 12.05 s | 1.24 | 1.24 |
| 2 | 4.88 s | 4.72 s | 12.07 s | 11.86 s | 1.24 | 1.24 |
| 4 | 4.86 s | 1.78 s | 12.05 s | 8.94 s | 1.24 | 1.24 |
| 8 | 4.89 s | 0.96 s | 12.09 s | 8.20 s | 1.24 | 1.24 |

The eight-worker cook took 7.66 s, 31% less time than the historical flat baseline and 5.0 times as fast as the single-worker directory cook. CPU load fell from the historical 4.88 s to 0.96 s. Ready time fell from 12.07 s to 8.20 s. Both layouts rendered byte-identical 1280 × 720 PPMs with the saved camera, lights, compute backend and async compute disabled.

| Incremental directory cook | Elapsed | Images encoded / reused | Geometry | Payload bytes written |
| --- | ---: | ---: | --- | ---: |
| Two vertex positions exchanged | 3.95 s | 0 / 72 | Rebuilt | 72,506,376 |
| One source PNG pixel changed | 3.47 s | 1 / 71 | Reused | 50,955,669 |
| Unchanged inputs | 3.35 s | 0 / 72 | Reused | 0 |

All 479 non-metadata Sponza records had identical schemas, codecs, stored and decoded sizes, and stored checksums in the flat and directory packages. The metadata record carries the input receipts. Every pre-existing part kept its modification time across these edits. The geometry and texture edits each added one content-addressed file. The source hash list was rechecked after the runs.

| Small complete-scene fixture | Wall time | Peak MiB | Logical bytes | Allocated bytes | Files |
| --- | ---: | ---: | ---: | ---: | ---: |
| Flat, 8 workers | 8 ms | 5.3 | 6,113 | 8,192 | 1 |
| Directory, 1 worker | 17 ms | 6.1 | 6,956 | 8,808 | 6 |
| Directory, 8 workers | 18 ms | 6.6 | 6,956 | 8,808 | 6 |

The small-fixture rows use seven alternating launches per configuration and a one-millisecond process wait to resolve startup overhead. This fixture exposes the cost of additional files and publication. The CLI keeps the flat default. The editor uses the named directory so source imports can reuse independent parts.

All 46 processor CTest suites passed. The engine asset integration, editor input and ZenApp startup suites passed. One model-catalog test skipped because this Windows account lacks symbolic-link creation privileges. A Windows junction probe separately verified that the directory reader rejects redirected texture parts. Editor Run retained the loaded scene through play, pause, resize and quit in both inline and threaded RHI modes. The release CLI validated the historical flat New Sponza package, and the older CLI rejected the directory format marker. The installed reader-only consumer opened glTF, OBJ and animated/skinned FBX directory packages. Recovery coverage includes process exit before replacement, same-stamp input mutation, damaged or missing parts, concurrent publishers, relocation, flat migration, scene-wide limits, and a deferred lease held across replacement and eviction. Desktop UV, masked vertex-alpha and morph-alpha changes matched independent clean cooks.

## Editor source-open verification

The earlier fresh-cook numbers measure `zenassetc`. The editor also normalizes and validates source data, publishes its cache and reopens the package for runtime image leases. On New Sponza the previous runtime import decoded retained source textures again during import validation and reference cooking. The importer now validates freshly decoded pixels on the texture worker. Later validation checks the immutable source and descriptor. Package-backed images still perform their normal integrity checks.

| Release source-open path | Median CPU-side elapsed time | Peak working set |
| --- | ---: | ---: |
| Previous editor defaults, empty flat cache | 69.10 s | 3.04 GiB |
| New editor defaults, empty named directory | 11.35 s | 4.31 GiB |
| Reopen the source with the published directory | 1.06 s | 1.05 GiB |

The first two rows time three fresh processes calling the editor's CPU source loader, each with an empty application cache and warm OS files. Working set is the median of the sampled peaks. The final row is a single verified cache hit in a new process using the default engine cache root. Directory opens allow up to eight source decodes, eight writer slots and eight reader workers. The reader and writer keep their existing byte limits.

The release ZenApp opened the editor-published manifest in 0.95 s of CPU scene load and reached ready in 8.14 s, windowed at 1280 × 720 with the compute backend and async compute disabled. All 479 non-metadata resource records matched the previous flat package. The measurements JSON includes the individual runs, manifest, executable and setup hashes, recorded extents and launch arguments.

## Streaming glTF textures during import

For a runtime reference glTF directory-cache miss, each worker reads an external image on demand, hashes the exact bytes, decodes and validates the pixels, then writes its native texture part before releasing its buffers. A shared reader lease keeps staged parts available to source consumers and protects them from eviction. The final writer reuses matching records, checks source freshness, and atomically publishes the manifest after geometry and scene-wide validation finish. Embedded textures are processed from the glTF buffer. Geometry accessors, mesh conversion and scene-wide cooking still operate on resident data.

The editor and reference directory CLI cook share this path. Image-processing profiles continue to use resident pixels. When the cache cannot accept an image during import, the runtime source loader can retain its encoded bytes and use the existing publication and fallback paths.

| New Sponza, empty editor cache | Previous build | Streaming build |
| --- | ---: | ---: |
| First texture part visible on disk | 4.79 s | 0.43 s |
| Source open including cache write and reopen | 11.59 s | 8.51 s |
| Peak process working set | 4.31 GiB | 1.81 GiB |

Values are medians of three release CPU-loader runs per build with warm OS files. The source reopened from the live cache in 1.05 s. A release ZenApp capture from the live manifest matched the earlier 1280 × 720 frame byte for byte. All 479 non-metadata records matched the original flat package.

On a separate copy of New Sponza, changing two vertex positions reused all 72 images. Replacing one image with a different image already present in the scene re-encoded one image, reused the other 71 and the geometry, and reused the identical stored part. An unchanged recook wrote zero payload bytes. Cancellation after texture staging left the manifest unpublished. A retained descriptor could still resolve its pixels during cache eviction. Individual timings, memory measurements, capture settings and hashes are in `SceneDirectoryCacheMeasurements.json`.
