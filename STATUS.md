# Project Status

Last updated: 2026-09-27

## Current status

Silhouette has a working OTLP/gRPC trace receiver. The executable listens on
`127.0.0.1:4317`, acknowledges empty and populated trace exports, counts export
requests and spans across resource/scope nesting, shuts down cleanly on Ctrl-C,
and reports the final totals. It does not yet retain or convert received spans.

## Current task

The OTLP/gRPC receiver slice is complete and verified with both a generated
OTLP client and a separate OpenTelemetry SDK exporter.

## Next concrete step

Define Silhouette's internal `Span` representation and convert received OTLP
trace data into it without allowing protobuf types to cross into the core
engine.

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
- Repository layout: generated OTLP bindings, a focused ingestion library, the `silhouette` executable, and checks under `tests/`; add further boundaries only when implemented behavior requires them.
- Verification framework: built-in CTest with a small test executable and no dedicated third-party test framework.
- Executable target: `silhouette`. Its runtime messages and lack of command-line flags do not establish a stable CLI contract.
- Initial trace transport: synchronous unary OTLP/gRPC on loopback port 4317 using insecure local credentials.
- Receiver lifecycle: `Start()` binds and returns; `Shutdown()` acts only after a successful start, requests gRPC shutdown, and waits for in-flight work.
- Dependency management: vcpkg manifest mode with pinned gRPC and protobuf versions.
- Protocol definitions: official `opentelemetry-proto` v1.11.0, fetched by CMake with a pinned archive checksum; generated sources remain under `build/`.
- Receiver boundary: protocol-specific types may exist inside ingestion code and tests, but must not leak into the future core engine or domain model.

## Open decisions

These should be decided only when authorized work requires them:

- whether a dedicated unit-test framework becomes useful as behavior grows;
- stable CLI flags, defaults, and output locations;
- whether and when to add OTLP/HTTP after the gRPC path;
- precise graph styling and observability-gap notation;
- whether minimal status/error fields are necessary in V1's internal span model.

An open decision is not permission for an agent to choose silently. Present options and tradeoffs when the decision becomes blocking, make the smallest reversible choice when authorized, and record the result here.

## Known issues

No known receiver issues. The receiver intentionally does not validate, retain,
or convert span contents yet; successful requests are acknowledged and counted
only.

## Latest work

- Added the OTLP/gRPC service, non-blocking startup, explicit graceful shutdown,
  request/span counters, and fixed loopback executable wiring.
- Added reproducible gRPC/protobuf dependency resolution and pinned generation
  from the official OpenTelemetry protocol definitions.
- Added independent CTest receiver cases covering empty and populated success
  responses, nested span counting, one-shot startup, repeated shutdown,
  destructor cleanup, and clean rejection of a second receiver on the same
  fixed port.
- Verified the OpenTelemetry Python SDK/exporter 1.45.0 sent one OTLP/gRPC
  request containing three spans and received a successful acknowledgement;
  Silhouette reported the expected `1` request and `3` spans after Ctrl-C.

## Deferred work

Capture storage, protocol-to-domain conversion, trace reconstruction, service
aggregation, and graph output remain future work within the V1 contract.
Possible post-V1 directions are listed in `ROADMAP.md`.
