# SDK consumption after joint normal qualification

Verified on Windows x64 on 25 October 2026 against fingerprint
`aa21c824877d080bc4c7746498a84ffa73c18456005ab2311f247b6edb720ebd`.
This supersedes the [earlier consumer checkpoint](CurrentConsumerVerification.md)
for that production checkpoint. The later
[processor completion checkpoint](ProcessorCompletionVerification.md) records
current consumption; both earlier records remain preserved.

All eight installed-package consumer checks pass:

| SDK configuration | Original install | Relocated SDK and consumer |
| --- | ---: | ---: |
| Debug | 2/2 | 2/2 |
| Release | 2/2 | 2/2 |

Every test reports the fingerprint above. The unchanged independent consumer
imports embedded and external-file glTF fixtures, verifies geometry and decoded
pixels, observes coherent physical-file progress through callback and polling,
and writes/reads an actually Zstandard-compressed package. This checks the public
Processor/Reader interface and static-library dependency closure. The normal
regressions and full-suite results are recorded separately in the
[joint qualification report](ContinuousJointNormalVerification.md).

Both standalone production builds were rebuilt before these checks. Debug was
installed from `build/block-codec-debug` into
`build/joint-consumer-verification/debug/install`; Release uses the frozen
`build/normal-joint-audit/install`. Each original SDK has 320 files. Every file
matches its relocated copy by size and SHA-256, and the relocated consumer source
and fixtures match the originals.

All four consumers use fresh Ninja builds, explicit Debug/Release settings,
`find_package`, and both CMake package registries disabled. Relocated builds
explicitly ignore their original SDK prefix. Exported CMake files contain no
absolute processor checkout, ZenEngine or `_deps` paths. Actual relocated
compile/link logs use the relocated SDK and contain no original SDK path.
No engine build or source change was involved.

Evidence is under `build/joint-consumer-verification/`:

- `run.ps1` and `commands.json`: exact installation/configuration/build/test commands.
- `*-tests.log` and `*-tests.xml`: verbose results and all eight fingerprint checks.
- `*-configure.log` and `*-build.log`: actual compiler/linker inputs.
- `audit.py` and `package-audit.json`: file identities, relocation equality,
  exported-path checks and test totals.
- `debug/` and `release/`: original-consumer builds, relocated SDKs, copied
  consumer sources and relocated-consumer builds.

This is a Windows Debug/Release SDK consumption checkpoint. It does not claim
another host's support, current engine rendering acceptance or release publication.
Development integration remains unpinned.
