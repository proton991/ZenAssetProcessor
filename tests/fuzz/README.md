# Reader fuzzing

`ZenAssetReaderFuzz` drives the actual file-based `OpenPackage`, selective
resource reads, metadata reads, and full scene decoding under libFuzzer and
AddressSanitizer. It links Reader, Format, and private decoder dependencies;
source importers and encoders are absent. Every input is tested as
received and, when the byte ranges are bounded, with repaired header/directory/
payload checksums so mutations reach the semantic decoder. Successful results
must satisfy ownership, decoded-size, graph-validity, and cancellation invariants.
Valid scene images and texture views also exercise mip previews with an eight-pixel
dimension limit and 256-byte output budget, including precancellation. Texture
views carry the format and interpretation needed for successful BC5 normal previews.
Unsupported formats remain explicit rejections with empty output. The block decoder
is instrumented along with container, mesh, and transport decoding.

Use an isolated build with Clang's `clang++` driver and its libFuzzer runtime:

```sh
cmake -S . -B build/reader-fuzz -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ -DZENASSET_BUILD_PROCESSOR=OFF -DZENASSET_BUILD_CLI=OFF -DZENASSET_BUILD_FUZZER=ON
cmake --build build/reader-fuzz
ctest --test-dir build/reader-fuzz --output-on-failure
```

On Windows, run in an MSVC developer shell, add
`-DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded`, and put the compiler's
`lib/clang/<version>/lib/windows` directory first on `PATH` so the matching ASan
DLL loads before any MSVC-bundled ASan DLL. A mismatched DLL can fail at startup
with Windows status `0xc0000139`, before the fuzzer emits its header.
The bundled libFuzzer uses the static release CRT and disables MSVC STL
container annotations; the harness matches those settings. ASan allocation
checks remain active. These instrumented libraries are for validation, not
the release install prefix.

Create `build/reader-fuzz/corpus` and `build/reader-fuzz/artifacts`. Seed the
corpus with small valid packages cooked by the current CLI or the authored
`rich.zenasset` produced by `ZenAssetSceneIOTests`. Include resource-rich seeds
because starting from empty input mostly exercises header rejection.
Include tiny and odd-sized BC4/BC5/BC7 images with authored mip tails so the preview
path and block mode checks are reached. Environment seeds should include legacy
version-one float cubes and version-two float/BC6H cubes with partial authored mips.
Environment payloads exercise scene validation; HDR previews are unsupported.

```sh
build/reader-fuzz/ZenAssetReaderFuzz build/reader-fuzz/corpus -max_len=262144 -max_total_time=120 -timeout=5 -rss_limit_mb=1024 -seed=12345 -artifact_prefix=build/reader-fuzz/artifacts/
```

The harness limits each input to 256 KiB, directory entries to 512, total
declared decoded payload to 2 MiB, requested scene storage to 4 MiB, and array
elements to 65,536. It does not read source dependencies. A process-local
temporary file is reused and removed on normal exit; a crash may leave it.
Preserve the generated crashing input and log before reducing a failure.
Reproduce with the executable followed by that single input path. Add a focused
regression test for a confirmed defect. Finite runs provide evidence, not a
guarantee that every malformed file is safe.
