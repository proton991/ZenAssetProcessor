# Reference importer and raw package verification

Local verification on 2026-10-23 used MSVC Release on Windows. The standalone
processor retained glTF/GLB and FBX source behavior before optimization.

The importer comparison covered 339 glTF corpus variants and 22 FBX fixtures.
All 361 outcomes and comparable structure counts matched the frozen engine
baseline: 360 accepted scenes and the expected unsupported dual-quaternion FBX
rejection. Structure comparison uses selected reachable nodes, clips, channels,
skins, morph primitives and variants. Static vertex/index counts are compared
only where runtime skin/morph expansion is absent. Canonical unused definitions,
engine default materials/textures and per-instance runtime mesh copies make
other raw count comparisons inappropriate. This gate alone does not establish
numeric pose or rendered-image parity.

The raw v3 package corpus then processed all 360 accepted scenes: 339 glTF
variants and 21 FBX fixtures. Every imported scene was written, read without
consulting its source dependencies, compared directly against all public
canonical scene fields, and rewritten to deterministic identical bytes.
`tests/SceneValueComparison.h` compares the public object model independently
of the binary schema visitor, so a field omitted symmetrically by writer and
reader fails the direct comparison. The run passed 1,845 checks, including the
synthetic resource and malformed-input regressions run before the corpus.

The full Release CTest configuration passed 11 tests with local fixture coverage:
format records, package framing, importer control/limits, shared processing,
scene I/O and command-line cook/inspect/validate. The scene I/O checks cover
source-independent ownership, all current resource types, extended materials,
authored mips, exact custom integer values, typed animation targets, optional
resource skipping, retained readers, allocation/count limits, metadata-only
lookup and failure/cancellation before publication. Reader sanitizer/fuzz
campaigns and the engine runtime/rendering gates are recorded separately.

To reproduce the optional corpus round-trip run, configure
`ZENASSET_BUILD_PROCESSOR=ON`, `ZENASSET_BUILD_TESTING=ON`, and optionally
`ZENASSET_TEST_CORPUS_DIR` for the engine fixture directory. Build
`ZenAssetSceneIOTests`, then pass `--corpus-list path.txt` containing one UTF-8
source path per line. The driver supports Unicode paths through this list on
Windows and emits `CORPUS_OK` or `CORPUS_FAIL` for every source. Local output is
under a uniquely named `scene-io-test-output-*` directory in its working directory.

The verification run's detailed local evidence is
`build/processor-port/structural-parity.json`, `scene-corpus-results.json` and
`scene-corpus.log`. These build artifacts are not redistributable fixtures.
The optional path configuration keeps the library's default tests independent
of a particular ZenEngine checkout.
