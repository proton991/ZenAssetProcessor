# Asset builds from CMake

`zenasset_add_manifest_build` connects a versioned asset manifest to a CMake
target. It invokes the same `zenassetc build` command as a manual batch build.
The compiler compares dependency contents and processing identities before
reusing complete packages. The target runs whenever requested; CMake timestamps
do not replace those checks.

An installed package with the CLI provides the `Compiler` component and the
`ZenAsset::Compiler` executable target:

```cmake
find_package(ZenAsset 0.1 CONFIG REQUIRED COMPONENTS Compiler)

zenasset_add_manifest_build(
    TARGET cook_assets
    MANIFEST assets.manifest.json
    OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/Cooked"
    JOBS 2
    MEMORY_BUDGET 4294967296
    JOB_MEMORY 1073741824)

add_dependencies(game cook_assets)
```

Build `cook_assets` explicitly, or let `game` request it. Add `ALL` to the helper
when assets should also be checked by the default build. Use `add_dependencies`
for consumers: the helper does not infer package filenames or create a stamp
that could conceal a missing or changed dependency. The build command fails if
the manifest or any job fails. Each package has its own atomic publication
boundary; a batch is not an all-or-nothing directory transaction.

`MANIFEST` is relative to the caller's source directory. `OUTPUT_DIRECTORY` is
relative to its binary directory and overrides the manifest's destination.
When omitted, the manifest controls the destination. Source paths inside the
manifest remain relative to the manifest's directory. Profiles and detailed
processing settings belong in the manifest.

`JOBS`, `MEMORY_BUDGET` and `JOB_MEMORY` forward positive integers to the batch
builder. Job reservations and per-stage byte limits govern admission and
requested storage; they are not a process RSS limit. Omitted values use the CLI
defaults. Reuse and rebuild decisions are reported as JSON by the compiler.

A source build with `ZENASSET_BUILD_CLI=ON` provides the same helper and compiler
target. The helper adds a build dependency on that compiler. When cross-compiling,
pass `COMPILER` with the path to an existing **host** `zenassetc` executable;
the helper deliberately does not try to execute the target-platform tool:

```cmake
zenasset_add_manifest_build(
    TARGET cook_assets
    MANIFEST assets.manifest.json
    OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/Cooked"
    COMPILER "${HOST_ZENASSETC}")
```

An explicit compiler also works with a Reader-only installed package, which
contains the helper but has no compiler executable target. Relative explicit
compiler paths use the caller's source directory. Keep independent build
processes from writing the same output directory concurrently; the batch API's
serialization covers callers in one process.
