# Current SDK consumer verification

The [joint-normal SDK checkpoint](JointConsumerVerification.md) verifies the later
`aa21c824...` production build. The results below remain scoped to `14d66cc2...`.

Verified on Windows x64 on 25 October 2026, using MSVC 19.51.36256.0,
CMake 4.4.2 and Ninja 1.13.2. This checkpoint applies to the installed SDK with
library fingerprint:

```text
14d66cc26703f8a724c19f02a92033665a42c5f51b3407cb5f27fc7139d558f5
```

Later production changes, including the subsequent BC5 continuous-LOD work,
require their own verification. This record establishes the public SDK/package
boundary at the fingerprint above; it does not complete M8 or publish a release.

## Consumer coverage

The independent [consumer](../tests/consumer-processor/main.cpp) links only the
public `ZenAsset::Processor` and `ZenAsset::Reader` targets through `find_package`.
It imports and verifies the triangle geometry and green texture, writes a package
with actual Zstandard-compressed chunks, and reads back its geometry and metadata.
Supported-format lookup covers glTF, GLB, FBX and OBJ without assuming an exact
number of registered importer descriptors.

The file-progress cases exercise the installed `SceneImportControl` constructor,
out-of-line `GetFileProgress()` implementation and `fileProgressChanged` callback:

- An embedded buffer and PNG remain one physical root file; three coherent
  callbacks are observed, including a known total before completion.
- An equivalent glTF with an external buffer and PNG counts three files; seven
  coherent callbacks include a partial completion before the final `3 / 3`.
- Every callback can poll the public control and sees the same coherent snapshot;
  the final callback and final poll agree, with `processedFiles == totalFiles`.

Invalid count arguments are rejected before source import. The consumer source
passed clang-format 22.1.3 `--style=file --dry-run --Werror`; its SHA-256 is
`0d256db9065e90183865956482b57b77c7a44f9ff80d1b693df18eca05d5797d`.

## Configurations and results

| Configuration | Standalone build | Installed consumer | Relocated consumer |
|---|---|---:|---:|
| Debug, `/MDd` | `build/block-codec-debug` | 2 / 2 passed | 2 / 2 passed |
| Release, `/MD` | `build/packaging-release` | 2 / 2 passed | 2 / 2 passed |

Both standalone builds enable Processor, tests, CLI and processor installation.
`ZENASSET_TEST_CORPUS_DIR` points to the processor-owned
`tests/integration/zenengine/CommonTest/Assets`. Existing dependency caches were
reused with `FETCHCONTENT_FULLY_DISCONNECTED=ON`; Debug also selects the cached
TinyObjLoader and earcut sources from the Release dependency directory.

All outputs are beneath `build/current-consumer-verification`. For each of
`debug` and `release`, `install` is the original local SDK prefix, `consumer` is
its independent consumer build, `relocated/sdk` is the copied SDK,
`relocated/consumer-source` is the copied consumer and fixtures, and
`relocated/consumer-build` is a fresh independent build at the new location.
The relocated builds disable both CMake package registries and set
`CMAKE_IGNORE_PREFIX_PATH` to the corresponding original install prefix.
The original SDK remains available for other explicitly scoped verification.

Each SDK contains 320 installed files. SHA-256 comparisons found no mismatches
between the installed and relocated files. The copied consumer code and fixtures
also match the verified originals. Exported CMake files contain no absolute
processor build, `_deps` or ZenEngine paths. Relocated compiler/linker commands
use only the relocated SDK for SDK headers and libraries. Each standalone compile
database contains 442 translation units and no ZenEngine references.

The full baseline Debug run completed cases 1–18, including `ZenAssetCookTests`
and both file-progress suites. `ZenAssetCookFixtures` (case 19) was intentionally
interrupted before a subsequent production fix to avoid duplicate full runs;
this was not a timeout or a reported test failure. The queued baseline Release
invocation never started. This checkpoint therefore makes no full-suite pass
claim. The eight installed/relocated consumer checks above all completed before
that interruption.

## Evidence

All paths below are relative to `build/current-consumer-verification/`:

- `summary.json`: exact consumer results and completed/interrupted baseline cases.
- `package-audit.json`: every installed-file hash, relocation comparisons, exported
  path audit, compile-database counts and SDK fingerprint.
- `debug-configure.log`, `debug-build.log`, `debug-install.log` and corresponding
  `release-*` logs: standalone configure/build/install records.
- `debug-consumer-tests.log` / `.xml` and `release-consumer-tests.log` / `.xml`:
  original installed SDK consumer runs.
- `debug-relocated-consumer-tests.log` / `.xml` and
  `release-relocated-consumer-tests.log` / `.xml`: copied SDK/source consumer runs.
- Matching `*-consumer-configure.log` and `*-consumer-build.log` files retain the
  CMake configurations and verbose compiler/link commands.
- `debug-tests.log` and `baseline-interruption.json`: the 18 completed Debug cases,
  interrupted case and exact verified process tree stopped at the scope change.

The [historical M3 record](../tests/consumer-processor/VERIFICATION.md) remains
unchanged apart from its link to this checkpoint. Host coverage here is Windows
x64 Debug/Release; other supported-host, engine-deployment and final-release gates
remain separately qualified. Development integration remains unpinned.
