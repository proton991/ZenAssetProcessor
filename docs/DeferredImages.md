# Deferred image reads

`ReadSceneDeferredImages` reads and validates the complete native resource graph,
but retains only image descriptors and an open package handle for image data.
It supports existing packages; no new wire schema or recook is required.

Call `ResolveImagePixels` before accessing a deferred image's bytes. The returned
immutable lease contains the complete original image, including authored mips
and array layers. Release that lease after uploading or generating a preview.
There is no resident image cache. Descriptors can outlive the scene and retain
the package handle rather than reopening a possibly replaced pathname.

Every lease checks resource integrity, payload validity, and agreement with its
descriptor. A failed or cancelled read publishes no pixels. Deferred scene reads
budget retained graph data and individual image decode/codec scratch together.
Concurrent leases have independent per-read budgets; callers must bound their
upload/preview concurrency to bound aggregate residency.

Ordinary `ReadScene` and imports retain their resident behavior. `WriteScene`
resolves one image at a time. Cooking deferred images is rejected when processing
is requested: resolve them explicitly first, or use the ordinary resident reader.
Deferred reads reduce retained CPU pixels, not GPU texture residency or mesh data.

Meshoptimizer resources on little-endian hosts decode directly into the final
typed vertex/index vectors. This removes one complete expanded geometry buffer.
The portable canonical-byte decoder remains available for other host byte orders.
Both paths share envelope, block, cancellation and budget checks. Typed decoding
has bitwise regression coverage (including nonfinite bit patterns, which scene
validation subsequently rejects), malformed payloads and cancellation mid-stream.

Verified with all 43 Release CTest suites, including lease lifetime, exact mip
contents, descriptor mismatch, cancellation, allocation limits, and writing a
deferred scene back to a regular package.
