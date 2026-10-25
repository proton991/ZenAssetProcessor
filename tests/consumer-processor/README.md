# Installed Processor consumer

This independent CMake project locates the installed `ZenAsset` package and
links only `ZenAsset::Processor`. It does not add the library source tree, know
its parser dependencies, or access ZenEngine. The self-contained triangle and
embedded green PNG were authored for this test and use the repository MIT license.

Build and install the package from the repository root first. The default build
includes the upstream targets named by their installation rules.

```sh
cmake -S . -B build/package -G Ninja -DCMAKE_BUILD_TYPE=Release -DZENASSET_BUILD_PROCESSOR=ON -DCMAKE_INSTALL_PREFIX=<installed-prefix>
cmake --build build/package
cmake --install build/package
```

Then configure this consumer from `tests/consumer-processor`:

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH=<installed-prefix>
cmake --build build
ctest --test-dir build --output-on-failure
```

For a relocation check, copy the installed prefix to a different directory and
configure this consumer against that directory in a fresh build. Dependencies
must resolve through the installed package files, without source overrides.
