# Project Status

Last updated: 2026-10-03

## Current status

Silhouette has a working OTLP/gRPC trace receiver that converts valid protocol
spans into a protobuf-independent C++ domain model and publishes complete
accepted export batches into an application-owned, thread-safe
`ActiveTraceManager`. The receiver publishes each batch before incrementing its
accepted-span counter. Fully owned reconstructed snapshots can be inspected
while ingestion continues, and reconstruction occurs after the manager lock is
released. Spans with malformed trace, span, or non-empty parent IDs are rejected
individually through OTLP partial success while valid siblings remain accepted.
The executable reports concise live active-trace and accepted-span counts. After
shutdown, the latest snapshot is printed as deterministic trace trees and
aggregated into the existing deterministic service graph, DOT, and SVG outputs.

## Current task

V2.1 continuous active-trace state is implemented on
`v2-active-trace-state`, based on merged `main`. The active manager, receiver
integration, concise live reporting, final V1 output path, and focused
concurrency coverage are complete. A fresh build and all 60 CTest cases pass.

## Next concrete step

Review and merge V2.1, then implement trace quiescence and finalization as the
next lifecycle slice. Do not add eviction, incremental topology, persistence,
web UI, or request playback to V2.1.

## Accepted decisions

- Project name: **Silhouette**.
- Core thesis: reconstruct observed architecture and request flow, while exposing gaps in the observation itself.
- Core engine language: **C++** for systems-learning depth.
- Input: real OpenTelemetry traces over standard OTLP from the beginning.
- Integration target: a completely separate existing instrumented application, used for manual testing only.
- V1 lifecycle: finite in-memory capture stopped with Ctrl-C, followed by batch processing.
- V2.1 lifecycle: continuously inspectable active traces with no finalization or eviction yet.
- V1 output: textual trace diagnostics plus a simple DOT/static visual service map.
- V1 robustness: tolerate and explicitly mark incomplete telemetry; do not attempt speculative relationship recovery.
- Test strategy: synthetic deterministic algorithm tests plus manual real-OTLP integration.
- Development strategy: vertical progress, just-in-time learning, and measurement before optimization.
- Build baseline: CMake 3.24 or newer, C++20 with compiler extensions disabled, and standard warnings without warnings-as-errors.
- Repository layout: a protobuf-free core library, private OTLP conversion, generated protocol bindings, a focused ingestion library, a separate rendering library, the `silhouette` executable, and checks under `tests/`; add further boundaries only when implemented behavior requires them.
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
- Active-trace ownership: the application owns one thread-safe `ActiveTraceManager`; receivers hold a non-owning reference and publish complete converted export batches into it atomically.
- Publication ordering: the receiver publishes accepted spans before incrementing its accepted-span counter, so an observed counter value never leads retained manager state.
- Snapshot semantics: snapshots own their reconstructed traces, remain unchanged during later ingestion, and copy evidence under the manager mutex before reconstructing outside it.
- Malformed span handling: reject only the malformed span, return OTLP partial success with an exact rejected count, and retain valid siblings.
- Reconstruction ownership: reconstructed traces own their spans and use indices only as internal relationship references; index values and node storage positions are not semantic output.
- Reconstruction ordering: traces use lexicographic trace-ID order, while top-level fragments and siblings use start time, span ID, end time, operation name, optional service identity, and optional parent identity, with absent optional values ordered first.
- Parent resolution: missing and duplicate parent IDs remain unresolved, duplicate spans are retained, and original parent IDs are never rewritten.
- Cycle handling: cycle detection considers only unambiguous resolved edges, detaches only edges between cycle members, and preserves resolved non-cycle children beneath those members.
- Trace text output: missing service identity uses `<unknown-service>`; duplicate, missing-parent, ambiguous-parent, and cycle-parent evidence uses inline diagnostics.
- Service aggregation: retain every known service, create edges only from resolved cross-service relationships with known endpoints, and deduplicate services, edges, and gap facts deterministically.
- Graph uncertainty: missing service identity and unresolved parent states are preserved as gap facts; cycle-parent sources remain unknown rather than being recovered from original parent IDs.
- Service-graph text output: the executable prints an interim deterministic `Services`, `Edges`, and `Gaps` summary after the reconstructed trace trees.
- Rendering boundaries: `ServiceGraph` is canonical; pure `FormatServiceGraphDot()` is reusable presentation, while private `WriteServiceGraphArtifacts()` is only a V1 local CLI convenience. Core has no rendering or Graphviz dependency.
- DOT identity and ordering: synthetic `service_N`/`gap_N` IDs follow the graph's deterministic ordering; service names are escaped labels, never identifiers or shell arguments.
- Gap visuals: missing service uses an amber diamond, missing parent a red triangle, ambiguous parent a purple hexagon, and cycle parent a teal octagon. An embedded legend distinguishes solid confirmed dependencies from dashed diagnostics.
- Gap connections: use only the source/destination endpoints stored in each gap, never infer or re-resolve endpoints from traces or gap kind. No synthetic unknown-service node is added.
- Local artifacts: overwrite fixed `silhouette.dot`/`silhouette.svg` files in the working directory after capture. Keep DOT, remove stale/partial SVG, identify Graphviz/rendering errors, and return nonzero on SVG failure; report cleanup failures explicitly.
- Graphviz: install locally through the platform package manager and in Ubuntu CI with `apt`; it is not a core or vcpkg dependency. Invocation uses only fixed arguments and filenames.

## Open decisions

These should be decided only when authorized work requires them:

- whether a dedicated unit-test framework becomes useful as behavior grows;
- stable CLI flags, defaults, and output locations;
- whether and when to add OTLP/HTTP after the gRPC path;
- future presentation design beyond the initial static map;

An open decision is not permission for an agent to choose silently. Present options and tradeoffs when the decision becomes blocking, make the smallest reversible choice when authorized, and record the result here.

## Known issues

No known conversion, receiver, trace-reconstruction, or service-aggregation
issues. V2.1 intentionally retains every observed trace for the process lifetime;
quiescence, finalization, and bounded eviction are not implemented yet. SVG
generation requires external Graphviz on `PATH`; fixed artifact paths, live
messages, and current styling do not establish stable CLI/design contracts.

## Latest work

- Replaced `SpanCapture` with an application-owned `ActiveTraceManager` that
  retains active evidence in append-only per-trace batches without recopying
  historical spans and returns owned reconstructed snapshots without
  reconstructing under its mutex.
- Preserved whole-export atomic publication and strong logical commit behavior:
  an exception leaves shared trace state unchanged, and concurrent snapshots
  observe either none or all of a batch.
- Preserved receiver publication ordering by updating active state before the
  accepted-span counter, with direct receiver coverage of the resulting
  no-leading-counter invariant.
- Added concise live active-trace/accepted-span updates while OTLP ingestion is
  running and preserved final trace, service graph, DOT, and SVG output.
- Added deterministic manager coverage for interleaved and out-of-order traces,
  duplicate and missing-parent evidence, stable input permutations, owned
  snapshots, distinct active counts, concurrent writers, and concurrent readers.
- Verified a fresh build and all 60 CTest cases. A live external OpenTelemetry
  Python exporter produced three pre-shutdown state updates, then final output
  for three spans across two traces and valid DOT/SVG artifacts.
- Added a separate rendering target with pure deterministic DOT serialization,
  escaped service labels, synthetic IDs, distinct gap visuals, and a legend.
- Added fixed local DOT/SVG output with explicit renderer failure reporting and
  stale/partial SVG cleanup; the DOT artifact survives renderer failure.
- Added direct DOT coverage and real, unavailable, and controlled failing
  Graphviz integration tests, plus Graphviz installation in existing Ubuntu CI.
- Verified 11 focused rendering tests and all 50 CTest cases locally, plus the
  full build and whitespace checks.
- Verified external SDK OTLP capture through SVG: 7 export requests, 9 spans,
  1 trace, 7 services, 6 confirmed edges, and no gaps or self-edges.
- Verified the design chat's adversarial raw-OTLP fixture through SVG: 1 export
  request, 16 spans, 4 traces, 11 services, 5 confirmed edges, and 7 gap facts.
  Visually checked all four gap types and the legend; valid cycle-member
  children survive, with no speculative edges or synthetic unknown services.
- Verified the actual CLI with a controlled failing `dot`: nonzero exit,
  Graphviz/rendering error, retained DOT, and removed stale/partial SVG.
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

Trace quiescence/finalization, bounded retention, incremental topology, web
rendering, and request playback remain deferred beyond V2.1. Candidate later
directions remain listed in `ROADMAP.md`.
