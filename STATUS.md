# Project Status

Last updated: 2026-09-29

## Current status

Silhouette has a working OTLP/gRPC trace receiver that converts valid protocol
spans into a protobuf-independent C++ domain model and retains them in a
thread-safe finite capture. Each export's accepted spans are published as one
atomic batch. Spans with malformed trace, span, or non-empty parent IDs are
rejected individually through OTLP partial success while valid siblings remain
accepted. After shutdown, captured spans are grouped by trace ID, reconstructed
without relying on arrival order, and printed as deterministic trace trees with
explicit markers for incomplete or inconsistent evidence. Reconstructed traces
are also aggregated into a deterministic service graph that retains known
services, deduplicated cross-service edges, and explicit gap facts without
re-resolving uncertain parent claims.

## Current task

The service-graph aggregation slice is complete. Core aggregation and formatting
project reconstructed evidence into known services, cross-service edges, and
deduplicated graph-level uncertainty.

## Next concrete step

Generate deterministic DOT and a simple static rendering from the aggregated
service graph while keeping observability gaps visible.

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
- Reconstruction ownership: reconstructed traces own their spans and use indices only as internal relationship references; index values and node storage positions are not semantic output.
- Reconstruction ordering: traces use lexicographic trace-ID order, while top-level fragments and siblings use start time, span ID, end time, operation name, optional service identity, and optional parent identity, with absent optional values ordered first.
- Parent resolution: missing and duplicate parent IDs remain unresolved, duplicate spans are retained, and original parent IDs are never rewritten.
- Cycle handling: cycle detection considers only unambiguous resolved edges, detaches only edges between cycle members, and preserves resolved non-cycle children beneath those members.
- Trace text output: missing service identity uses `<unknown-service>`; duplicate, missing-parent, ambiguous-parent, and cycle-parent evidence uses inline diagnostics.
- Service aggregation: retain every known service, create edges only from resolved cross-service relationships with known endpoints, and deduplicate services, edges, and gap facts deterministically.
- Graph uncertainty: missing service identity and unresolved parent states are preserved as gap facts; cycle-parent sources remain unknown rather than being recovered from original parent IDs.
- Service-graph text output: the executable prints an interim deterministic `Services`, `Edges`, and `Gaps` summary after the reconstructed trace trees.

## Open decisions

These should be decided only when authorized work requires them:

- whether a dedicated unit-test framework becomes useful as behavior grows;
- stable CLI flags, defaults, and output locations;
- whether and when to add OTLP/HTTP after the gRPC path;
- precise service-graph styling and graph observability-gap notation;

An open decision is not permission for an agent to choose silently. Present options and tradeoffs when the decision becomes blocking, make the smallest reversible choice when authorized, and record the result here.

## Known issues

No known conversion, receiver, trace-reconstruction, or service-aggregation
issues. Captured and reconstructed spans remain in memory only for the finite
process lifetime. DOT output and static graph rendering are not yet implemented.

## Latest work

- Added deterministic service aggregation over reconstructed parent decisions,
  including isolated services, deduplicated cross-service edges, and no
  same-service self-edges.
- Added graph-level missing-service, missing-parent, ambiguous-parent, and
  cycle-parent facts without speculative parent re-resolution.
- Added a compact deterministic service-graph summary and direct coverage for
  missing endpoints, duplicates, cycles, disconnected components, and input
  permutation stability.
- Added deterministic trace grouping, parent resolution, duplicate-ID handling,
  and cycle-safe reconstruction without changing original span evidence.
- Added readable trace-tree output with canonical ID formatting and inline
  diagnostics for incomplete or inconsistent telemetry.
- Added direct reconstruction and formatting coverage for ordering, fragments,
  ambiguity, cycles, unchanged parent IDs, and input-permutation stability.
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

DOT generation and static graph rendering remain future work within the V1
contract. Possible post-V1 directions are listed in `ROADMAP.md`.
