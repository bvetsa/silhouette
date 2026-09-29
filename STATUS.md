# Project Status

Last updated: 2026-09-28

## Current status

Silhouette has a working OTLP/gRPC trace receiver that converts valid protocol
spans into a protobuf-independent C++ domain model and retains them in a
thread-safe finite capture. Each export's accepted spans are published as one
atomic batch. Spans with malformed trace, span, or non-empty parent IDs are
rejected individually through OTLP partial success while valid siblings remain
accepted.

## Current task

The OTLP span-conversion and in-memory-capture slice is complete. Core,
converter, and receiver responsibilities now have separate automated tests.

## Next concrete step

Group captured spans by trace ID and reconstruct their parent-child structure
without relying on arrival order.

## Accepted decisions

- Project name: **Silhouette**.
- Core thesis: reconstruct observed architecture and request flow, while exposing gaps in the observation itself.
- Core engine language: **C++** for systems-learning depth.
- Input: real OpenTelemetry traces over standard OTLP from the beginning.
- Integration target: a completely separate existing instrumented application, used for manual testing only.
- V1 lifecycle: finite in-memory capture stopped with Ctrl-C, followed by batch processing.
- V1 output: textual trace diagnostics plus a simple DOT/static visual service map.
- V1 robustness: tolerate and explicitly mark incomplete telemetry; do not attempt speculative relationship recovery.
- Test strategy: synthetic deterministic algorithm tests plus manual real-OTLP integration.
- Development strategy: vertical progress, just-in-time learning, and measurement before optimization.
- Build baseline: CMake 3.24 or newer, C++20 with compiler extensions disabled, and standard warnings without warnings-as-errors.
- Repository layout: a protobuf-free core library, private OTLP conversion, generated protocol bindings, a focused ingestion library, the `silhouette` executable, and checks under `tests/`; add further boundaries only when implemented behavior requires them.
- Verification framework: built-in CTest with a small test executable and no dedicated third-party test framework.
- Executable target: `silhouette`. Its runtime messages and lack of command-line flags do not establish a stable CLI contract.
- Initial trace transport: synchronous unary OTLP/gRPC on loopback port 4317 using insecure local credentials.
- Receiver lifecycle: `Start()` binds and returns; `Shutdown()` acts only after a successful start, requests gRPC shutdown, and waits for in-flight work.
- Dependency management: vcpkg manifest mode with pinned gRPC and protobuf versions.
- Protocol definitions: official `opentelemetry-proto` v1.11.0, fetched by CMake with a pinned archive checksum; generated sources remain under `build/`.
- Receiver boundary: protocol-specific types may exist inside ingestion code and tests, but must not leak into the future core engine or domain model.
- Domain IDs: `TraceId` and `SpanId` are non-default-constructible fixed-size binary values; incorrect lengths and all-zero values are invalid, while individual zero bytes are valid.
- Initial span model: trace ID, span ID, optional parent ID, optional service name, operation name, and start/end Unix nanoseconds. Status and error fields remain deferred until a concrete diagnostic needs them.
- Resource identity: the first exact-key, non-empty string `service.name` value wins; empty, non-string, and absent values become missing service identity.
- Capture ownership: the application owns one thread-safe `SpanCapture`, and receivers publish complete converted export batches into it atomically.
- Malformed span handling: reject only the malformed span, return OTLP partial success with an exact rejected count, and retain valid siblings.

## Open decisions

These should be decided only when authorized work requires them:

- whether a dedicated unit-test framework becomes useful as behavior grows;
- stable CLI flags, defaults, and output locations;
- whether and when to add OTLP/HTTP after the gRPC path;
- precise graph styling and observability-gap notation;

An open decision is not permission for an agent to choose silently. Present options and tradeoffs when the decision becomes blocking, make the smallest reversible choice when authorized, and record the result here.

## Known issues

No known conversion or receiver issues. Captured spans remain in memory only
for the finite process lifetime and are not yet reconstructed or rendered.

## Latest work

- Added validated binary trace/span value types and the minimal internal `Span`
  model without introducing protobuf dependencies into the core target.
- Added application-owned capture storage that retains complete export batches
  under a mutex and returns flattened owned snapshots.
- Added deterministic resource `service.name` extraction, per-span malformed-ID
  rejection, valid-sibling retention, and OTLP partial-success responses.
- Split automated coverage into core/domain, direct converter, and focused
  receiver-level gRPC tests.
- Added the OTLP/gRPC service, non-blocking startup, explicit graceful shutdown,
  request/span counters, and fixed loopback executable wiring.
- Added reproducible gRPC/protobuf dependency resolution and pinned generation
  from the official OpenTelemetry protocol definitions.
- Added independent CTest receiver cases covering empty and populated success
  responses, nested span counting, one-shot startup, repeated shutdown,
  destructor cleanup, and clean rejection of a second receiver on the same
  occupied port.
- Verified the OpenTelemetry Python SDK/exporter 1.45.0 sent one OTLP/gRPC
  request containing three spans and received a successful acknowledgement;
  Silhouette reported the expected `1` request and `3` spans after Ctrl-C.

## Deferred work

Trace reconstruction, service aggregation, and graph output remain future work
within the V1 contract.
Possible post-V1 directions are listed in `ROADMAP.md`.
