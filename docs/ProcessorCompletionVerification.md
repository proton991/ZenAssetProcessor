# Processor completion and installed SDK verification

Verified on Windows x64 on 25 October 2026 at production fingerprint
`8976c628910f91106f0055657a85e9ace6afa3efaf6748639b6cd7804ca04a4f`.
This closes the processor-scoped implementation and verification work. ZenEngine
integration, rendered acceptance and release publication remain separate gates.
No main-engine sources or builds were changed for this checkpoint.

## Final ordinary builds

| Configuration | Full standalone suite | Elapsed test time |
| --- | ---: | ---: |
| Release | 40/40, no skips | 27.85 s |
| Debug | 40/40, no skips | 118.70 s |

These are test-run durations, not performance benchmarks; independent HDR
correctness work overlapped part of the Debug run. The source snapshot,
additional test/CLI identities, exact commands, logs and XML results are under
[`build/processor-completion-verification`](../build/processor-completion-verification).
[Parsed suite results](../build/processor-completion-verification/full-suite-results.json)
confirm both configurations and unchanged final source identities.

The [geometry admission report](ImportGeometryBudgetVerification.md) records
actual glTF/FBX quota boundaries, cancellation, full-scene preservation and
360 distinct source scenes with raw package equality and deterministic rewriting.
That corpus ran at the preceding import checkpoint. The final fingerprint differs
only in `cmake/BlockCodecs.cmake` and the added `cmake/PatchBc6h.cmake`; importer,
reader, raw writer and geometry-processing source hashes match the corpus snapshot.
The final full suites include all new admission tests and the BC6H regression.

## Installed and relocated consumers

| SDK | Original installation | Relocated SDK and consumer |
| --- | ---: | ---: |
| Debug | 2/2 | 2/2 |
| Release | 2/2 | 2/2 |

All eight checks print the final fingerprint. They import embedded and external
file glTF fixtures, verify geometry and decoded textures, write/read compressed
packages, and check physical-file progress. Each 320-file SDK has identical sizes
and SHA-256 hashes after relocation, as do the copied consumer sources/fixtures.

Fresh Ninja builds explicitly selected Debug or Release, disabled both CMake
package registries, and excluded the original SDK prefix for relocated builds.
Exported CMake files contain no checkout or `_deps` paths. Actual relocated
compile/link commands use the relocated installation without original-SDK,
engine or dependency-build paths. This verifies link closure; it does not claim
that debug binary symbols contain no historical source paths.

Evidence: [consumer commands](../build/processor-completion-verification/consumer-commands.json)
and [complete package audit](../build/processor-completion-verification/package-audit.json).
This supersedes [the joint-normal SDK checkpoint](JointConsumerVerification.md)
for the final processor source; earlier results remain historical records.

## HDR correction and separate stage measurements

Ordinary/instrumented parity exposed an uninitialized BC6H palette lane. The
tracked build-local correction uses RGB-only scoring, initializes endpoint alpha
and all default integer-color lanes, preserves the fetched dependency, and
installs the corrected header with its license. Patch/source hashes are checked
before compilation. The [stage report](StageMeasurementVerification.md) retains
the original failure and corrected package/scene/block equality.

A regression varies caller stack contents/layout across signed and unsigned HDR
inputs. The old encoder differs on 104 of 224 varied/repeated calls; the corrected
encoder has zero differences. The 14 reference encodes are additional calls.
Both variants satisfy the selected fixture's decoded quality check, so the
negative control specifically exposes nondeterministic encoded bytes.

The final [independent HDR audit](../build/bc6h-determinism-audit/independent-hdr-initialized/README.md)
passes 12 cases, 90 authored face/mip quality checks and 24 source-free resource-exact
round trips. Maximum relative RGB RMSE is 0.0148244493 against the unchanged 0.02
limit. Six cases compress and six retain their exact original cubes. All 48
package variants preserve the same nonmetadata resources and decoded metrics as
the preceding corrected checkpoint. Sources are restored and binary identities
remain unchanged.

[Actual-pipeline measurements](StageMeasurementVerification.md) separately record
source acquisition/parsing, conversion, cooking, resource encoding, file operations,
package decoding and scene construction. Seven alternating ordinary/instrumented
rounds pass full postflight comparisons with unchanged input/build identities.
Default builds erase instrumentation; the results do not measure engine upload,
GPU execution or runtime atlas profitability.

Only the verified Windows x64 scope is claimed. The development checkout remains
unpinned and the v3 schema remains unreleased. Integration awaits the user's next
instruction.
