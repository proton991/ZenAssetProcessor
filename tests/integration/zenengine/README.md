# ZenEngine consumer tests

ZenAssetProcessor owns the importer, package, compression, texture/atlas and
geometry acceptance tests, their fixtures, and the asset benchmarks. This optional
suite checks those results through ZenEngine's runtime adapter, Vulkan uploader
and editor. It is not part of the engine's normal test targets or the standalone
processor's default build.

The engine integration stays opt-in through `ZEN_EXTERNAL_TESTS_DIR`; no test
directory or private decoder is required by an ordinary ZenEngine build.

From this repository, in an MSVC developer shell on Windows:

```sh
cmake -S ../ZenEngine -B build/zenengine-release -G Ninja -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON -DZEN_BUILD_ASSET_TOOLS=ON -DZEN_BUILD_EDITOR=ON -DZEN_ASSET_PROCESSOR_SOURCE_DIR=E:/Dev/ZenAssetProcessor -DZEN_EXTERNAL_TESTS_DIR=E:/Dev/ZenAssetProcessor/tests/integration/zenengine
cmake --build build/zenengine-release --target ZenAssetEngineTests ZenAssetTextureIntegrationTest EditorAssetRenderingTest GLTFCorpusImport SceneLoadBenchmark ScenePackageSmoke ScenePackageBenchmark SceneUploadBenchmark
ctest --test-dir build/zenengine-release -R "^ZenAssetEngineTests$" --output-on-failure
ctest --test-dir build/zenengine-release -L asset -LE cpu --output-on-failure
```

Replace the absolute paths when using a different checkout location. Native tests
require a Vulkan device and validation layer. Disable `ZEN_BUILD_EDITOR` to omit
editor comparisons. The processor library itself has no ZenEngine dependency.

`CommonTest/Assets` is the owned glTF/GLB/FBX corpus, including its FBX license and
provenance. Standalone semantic tests use it by default; set
`ZENASSET_TEST_CORPUS_DIR` to select another corpus or to an empty string to skip
corpus tests. `Editor/Fixtures` contains generated scenes for cooked-versus-source
comparisons. ZenEngine retains a small independent fixture subset for its ordinary
renderer/editor tests.

For an existing standalone build that cached the old engine corpus location,
configure it once with `-U ZENASSET_TEST_CORPUS_DIR` to adopt the new default.

The batch/editor parity case accepts `ZEN_EDITOR_BATCH_PACKAGE`. Generate its
input from this repository before running the native suite:

```sh
build/processor/zenassetc cook tests/integration/zenengine/Editor/Fixtures/recook_textured.gltf --output build/batch.zenasset --profile desktop --vertex-fetch on --texture-compression bc7 --texture-mipmaps on
```

Set `ZEN_EDITOR_BATCH_PACKAGE` to the absolute output path. Without it, the two
parameterized batch comparisons report a skip.

The integration targets include shared engine test utilities from the explicitly
selected checkout. Private importer decoder dependencies are linked only by this
optional suite, never by ZenEngine's ordinary `CommonTest` target.

Python audits and benchmarks are in [tools/validation](../../../tools/validation/README.md).
Historical benchmark logs still identify their original executable and script
locations; they are records of those runs, not current build instructions.
