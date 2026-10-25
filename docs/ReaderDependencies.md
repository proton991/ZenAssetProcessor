# Reader source dependencies

Reader-only configurations fetch **bcdec only**. They use the reviewed
meshoptimizer v1.1 and Zstandard v1.5.7 decoder subset distributed under
`third_party/reader`: 39 files, 1,018,766 bytes including licenses. Processor
configurations fetch the full pinned upstream archives for encoding, while
compiling the same bundled runtime decoder files.

The archive URLs and SHA-256 pins remain in `cmake/CodecDependencies.cmake`.
Each bundled file has a separate SHA-256 in `cmake/ReaderSourceHashes.cmake`.
Configuration checks those files and fails on missing or changed bytes. They
are also configure dependencies, so editing one causes the next build to rerun
the check. Git attributes preserve their exact bytes across checkout platforms.
The hash manifest is part of the existing processor build fingerprint.

## Contents and provenance

- Zstandard retains its twelve existing C translation units, their common and
  decompression headers, public headers and both upstream license texts.
  These files are byte-identical to the pinned archive. Compression API
  declarations remain in the shared upstream headers; compressor, dictionary
  builder, legacy and assembly implementation files are absent from the subset.
- Meshoptimizer retains its two existing generated decoder files and MIT license.
  `cmake/MeshoptExtraction.cmake` contains the reviewed extraction procedure and
  checks the full upstream input hashes and unique section boundaries. Decoder
  bodies are unchanged; generated files now use explicit LF line endings on all
  hosts. Encoding functions/helpers are removed, and the private
  declarations, namespace and symbols retain their existing ZenAsset prefixes.
- bcdec remains the pinned, separately fetched block decoder. Its source is
  independent of the Processor's BC7/BC6H encoder dependencies.

Processor configuration regenerates the meshoptimizer subset from its pinned
sources and requires byte equality with the bundled copies. It also compares
the bundled Zstandard files against the full pinned tree. This keeps source
and installed Reader builds on the same decoder implementation and catches an
upstream pin change without a corresponding reviewed subset update.

No public C++ API or package schema changes are involved. The decoder libraries,
private exported CMake target names and installed license locations are the same.

## Exporting the subset

Use source trees extracted from the hash-verified archives declared by CMake.
The exporter requires a new output directory and never changes the input trees:

```powershell
cmake -DMESHOPT_SOURCE_DIR=<verified-meshoptimizer-source> `
  -DZSTD_SOURCE_DIR=<verified-zstd-source> `
  -DOUTPUT_DIRECTORY=<new-export-directory> `
  -P tools/ExportReaderSources.cmake
```

It writes `meshoptimizer/`, `zstd/` and `ReaderSourceHashes.cmake` into the output.
For an unchanged pin, compare every output byte and the manifest with the
repository copies. For an upgrade, review the upstream changes, update archive
pins and extraction input hashes/boundaries, then review the generated subset
and manifest before replacing their repository copies. The two component
directories belong under `third_party/reader`; the manifest belongs under
`cmake`. Preserve the upstream licenses and do not format vendored code.

The first export was checked directly against the cached archives whose hashes
match the declared pins. All 36 Zstandard files and the meshoptimizer license
match their archive members. The two meshoptimizer inputs match their archive
members and the extraction's required hashes. A second export reproduces all
39 bundled files and the manifest exactly.

## Verification checkpoint

On Windows x64, fresh Ninja Debug and Release configurations each compile 31
translation units including four test sources, fetch only bcdec, and pass all
four Reader tests. Fourteen production units come from the bundled subset
(twelve Zstandard C files and two meshoptimizer decoders).

The full Processor Debug and Release suites each pass 33/33 tests with the same
decoder files (452.60 and 13.41 seconds respectively on this host).
Separately installed Debug and Release consumers pass 16 combined checks:
default version/preview behavior, six v3 packages covering geometry, Zstandard,
mesh coding, raw and BC arrays, and BC6H, plus truncated-package rejection.
An isolated copy of the bundle also verifies that changing a decoder source
causes the next build to fail the integrity check, and restoring it permits the
build again. Evidence is retained under `build/reader-source-evidence`.

This verifies the source-download/build boundary on this host. It does not
claim another host's support or complete the remaining profile, rendering,
performance and release gates.
