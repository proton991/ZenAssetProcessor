# Installed package verification

This is the historical M3 record. See the [current SDK consumer checkpoint](../../docs/CurrentConsumerVerification.md) for the later fingerprint-qualified Debug/Release package and file-progress API checks.

Verified on Windows x64, MSVC 19.51.36256.0, CMake 4.4.2, Ninja, Release,
2026-10-23 (Asia/Seoul). This records the M3 package; later format or codec
changes require a new package build and verification.

## Package and consumer

The initial `build/packaging-release` configuration fetched all eight pinned
archives from their declared network URLs. No `FETCHCONTENT_SOURCE_DIR_*`
override was supplied; all eight cache values are empty. SHA-256 values were
checked both by FetchContent and in the verification manifest. Its compilation
database contains no ZenEngine source paths.

After the final M3 public API update, the package was rebuilt and installed to
`build/packaging-install`. All six standalone tests passed. The independent
consumer linked only `ZenAsset::Processor`, imported its authored triangle,
and decoded the embedded green PNG. The same test passed after copying both
the package and consumer to `build/packaging-relocation-m3`, disabling CMake
package registries, and excluding the original installation prefix.

All 300 installed files match the relocated copy by SHA-256. Installed CMake
files contain no build, `_deps`, or ZenEngine paths. The consumer's compiler
uses the relocated public include directory; its linker uses the relocated
static libraries plus Windows system libraries.

The fresh processor-disabled configuration in `build/packaging-reader` created
no `_deps` directory. Both reader tests passed, and its installed Reader
consumer built and ran. M3 reader rebuild, tests, and installation also passed.

Evidence in the repository's `build/` directory:

- `packaging-configure.log`: initial fresh network configuration.
- `packaging-build.log`, `packaging-m3-build.log`: initial and M3 builds.
- `packaging-m3-tests.xml`, `packaging-reader-m3-tests.xml`: standalone tests.
- `packaging-consumer-m3-tests.xml`, `packaging-relocation-m3-tests.xml`: consumers.
- `packaging-verification-m3.json`: dependency hashes, installed-file hashes,
  production-source hashes, test metadata, and package locations.

## Engine integration

ZenEngine was configured with an empty `ZEN_ASSET_PROCESSOR_SOURCE_DIR` and the
installed package prefix. The M3 CommonTest executable passed 164 of 165 tests;
the directory-symlink test was skipped because symlink creation was unavailable.
The tests explicitly linked the installed ufbx, Draco, and meshoptimizer SDK
targets for reference comparisons. No standalone processor source files were
compiled by the engine build.

A separate engine configuration disabled asset tools, editor, runtime UI, and
tests, and used the Reader-only installed prefix. Its `ScenePackageSmoke`
executable loaded nine preserved v2 packages and four v3 packages. One v3
package was cooked by the installed CLI from this consumer's fixture, then
loaded after the copied source file was deleted. All 13 loads succeeded.
The 361-entry runtime compilation database contains no source parser units;
the executable's link line contains Reader and Format, with no Processor,
asset-tools, ufbx, Draco, meshoptimizer, simdjson, fastgltf, WebP, or Basis library.

Engine evidence is in `E:/Dev/ZenEngine/build/asset-processor/`:
`installed-engine-tests-m3.xml`, `installed-runtime-goldens-m3.json`,
`installed-native-cook.log`, and `installed-package-verification-m3.json`.
The initial installed-engine configure exposed directory-local imported targets;
moving package discovery to the engine root fixed that integration issue.

## Export and redistribution scope

Processor exports retain normal CMake static dependency relationships. Upstream
packages provide `simdjson::simdjson`, `fastgltf::fastgltf`, `draco::draco`,
`meshoptimizer::meshoptimizer`, and `WebP::webpdecoder`. ZenAsset exports the
private wrappers `ZenAsset::PrivateUfbx`, `ZenAsset::PrivateBasis`, and
`ZenAsset::PrivateStb`. ufbx's header is available when its private target is
explicitly linked; Processor clients do not inherit that private include path.
The stb implementation is compiled into Processor, so its exported interface
wrapper needs no installed stb header or archive.

`share/ZenAsset` contains the project license, dependency notice index, and
upstream license, author, and patent notices. Basis's additional license texts
and bundled Zstandard license are included. Native upstream installation also
includes Draco tools and WebP support libraries; their files are retained to
preserve their unmodified package exports.

This verification covers Windows Release packages. Other platforms and Debug
installations were not tested here. Build a processor-disabled installation
for the minimal Reader distribution: the full SDK's package configuration
resolves its Processor dependency packages when importing its shared export
file. No codec or dependency archives are merged manually.
