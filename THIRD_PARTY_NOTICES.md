# Third-party notices

ZenAssetProcessor is distributed under the root MIT license. Installations
also contain the codec libraries listed below; Processor-enabled installations
add the source import dependencies. Full upstream notices
are installed in `share/ZenAsset/licenses/<dependency>`; retain those files when
redistributing the corresponding libraries. Public ZenAsset headers do not
expose parser or codec headers, but static linkage still requires their archives.

The exact pinned archive URLs and SHA-256 hashes are maintained in
`cmake/CodecDependencies.cmake`, `cmake/BlockCodecs.cmake` and
`cmake/ProcessorDependencies.cmake` and `cmake/ObjDependencies.cmake`.
Reader-only builds fetch bcdec and use the reviewed meshoptimizer/Zstandard
decoder subset in `third_party/reader`. They do not fetch, build or link source
importers or encoder libraries. `cmake/ReaderSourceHashes.cmake` records every
bundled file's SHA-256; its provenance and export procedure are documented in
`docs/ReaderDependencies.md`. Full meshoptimizer and Zstandard archives are
fetched only for Processor builds.

| Dependency | Pinned revision | Upstream project | Notices retained |
| --- | --- | --- | --- |
| simdjson | v4.6.11 | https://github.com/simdjson/simdjson | Apache-2.0/MIT alternatives; both supplied license texts |
| fastgltf | v0.9.1 | https://github.com/spnda/fastgltf | MIT license, copyright maintained in upstream notice |
| Draco | 1.5.7 | https://github.com/google/draco | Apache-2.0 license and AUTHORS |
| meshoptimizer | v1.1 | https://github.com/zeux/meshoptimizer | MIT license |
| libwebp | v1.6.0 | https://github.com/webmproject/libwebp | COPYING, PATENTS and AUTHORS |
| ufbx | v0.23.1 | https://github.com/ufbx/ufbx | MIT/public-domain alternatives supplied by upstream |
| Basis Universal | v1_60 | https://github.com/BinomialLLC/basis_universal | Apache-2.0; Binomial LLC; upstream README and additional license texts |
| Zstandard | v1.5.7 | https://github.com/facebook/zstd | BSD/GPLv2 alternatives; both supplied license texts |
| bc7enc_rdo scalar block encoders | b9438627eef73a1157e84201b6fa6eb2ffd6d9f0 | https://github.com/richgel999/bc7enc_rdo | MIT/public-domain alternatives, upstream LICENSE |
| bc6h_enc | 59947031122606cc9926d598454e7e3e9c316e2b | https://github.com/0xc0de/bc6h_enc | MIT; standalone LICENSE plus header retaining Microsoft and Mike Acton notices |
| bcdec | 80859ed3b7afb1c527a2a99d70c61457bea72d0c | https://github.com/iOrange/bcdec | MIT/public-domain alternatives, upstream LICENSE |
| stb image decoder | 2c980bb59875b0d32144a71867fbdebb2f77cd20 | https://github.com/nothings/stb | MIT/public-domain alternatives, copyright Sean Barrett |
| tinyobjloader | 45636bdcef1a4fec140346b90c0b50bf0bc3e23b | https://github.com/tinyobjloader/tinyobjloader | MIT; upstream LICENSE also retains its bundled earcut notice |
| mapbox earcut.hpp | 4811a2b69b91f6127a75e780de6e2113609ddabb (v2.2.4) | https://github.com/mapbox/earcut.hpp | ISC license |

Basis Universal is compiled here as its transcoder and links the shared private
Zstandard decoder archive. Its bundled `zstddeclib.c` is not compiled. Its
upstream third-party license directory is retained alongside the main license.
Encoder sources in the Basis archive are not part of this target.
Block texture encoding compiles only `bc7enc.cpp`, `rgbcx.cpp`, and the single
`bc6h_enc.h` implementation. CLI, RDO and ISPC components are not compiled.
The independently implemented bcdec decoder uses its precise BC4/BC5 mode;
it is the only block-texture dependency in Reader-only builds. Wrapper code
prefixes encoder symbols and uses a single deterministic initialization of
shared BC7/rgbcx tables before worker access. No upstream source is modified.
Upstream Draco/WebP installation rules can also install their own support
libraries or command-line programs; their original notices remain applicable.
OBJ parsing builds only `tiny_obj_loader.cc` with its embedded fast_float
implementation disabled. Polygon triangulation uses the separately pinned
earcut header directly from the importer; tinyobjloader's built-in earcut path
is disabled. Both namespaces have private build prefixes, and neither raw
dependency header is installed in the public SDK. These dependencies are
fetched and linked only by Processor builds.

The fetched dependency sources remain pristine. Since meshoptimizer v1.1 places
encoding and decoding functions in the same source files, the bundled runtime
copies omit encoding functions and their exclusive helpers.
`cmake/MeshoptExtraction.cmake` validates the complete source SHA-256 and exact
extraction boundaries when exporting these copies. Processor configurations
regenerate and compare them against the bundle. Decoder bodies are unchanged; their
public symbols and internal namespace have ZenAsset-specific prefixes. Its MIT
license and copyright notice are retained. The Processor separately builds the
complete upstream meshoptimizer library. A separate private meshoptimizer
sequence encoder retains the upstream algorithm with its version fixed to v1,
so unrelated callers of the upstream global version setter cannot affect
package output. Its only algorithm-adjacent change is replacing the mutable
version read with the constant 1; symbols and internal namespace are prefixed.
Zstandard uses byte-identical copies of its original common/decompression files
and headers in the decoder archive. Processor configurations verify these files
against the full pinned upstream tree and build compression files in a separate
Processor-only archive; no dictionary builder or legacy format code is built.

ZenAsset's own imported scene semantics and throughput kernels derive from ZenEngine,
copyright (c) 2024 proton991, under the root MIT license.
