# Project Status

Last updated: 2026-10-10

## Current status

Silhouette continuously retains accepted OTLP spans in an application-owned
`ActiveTraceManager` and incrementally maintains the observed service topology.
Traces become quiescent after 30 seconds without accepted evidence and reopen
on late evidence. All active and quiescent traces remain retained and contribute
to topology. Operational finalization does not establish trace completeness.
Each accepted non-empty export publishes its complete batch, advances counters,
and then emits sorted unique dirty trace IDs under a narrow publication mutex.
The graph worker reconstructs only those traces from their latest authoritative
state, atomically replaces their reference-counted topology contributions, and
publishes a new topology version only when the deterministic graph or telemetry
presence changes. Equivalent updates retain all evidence and contribution
counts without rendering an already successful version again; if the current
version has no successful SVG, a new update retries that version. A separate
Graphviz worker coalesces obsolete
frames and publishes only a current rendered version; each render still lays out
the whole current graph. A thin loopback HTTP/SSE
adapter serves the live map at `http://127.0.0.1:8008` while ingestion continues.
Ctrl-C still drains in-flight ingestion and live-map work before producing the
existing deterministic trace, service graph, DOT, and SVG outputs.

## Current task

V2.3 trace quiescence is implemented on `codex/v2-trace-quiescence`, based on
merged V2.2 `origin/main` at `4995d6a` (PR #8). Changes are local and
uncommitted, ready for review and integration.

The manager now retains active/quiescent lifecycle state with a 30-second
production quiet interval, an injected monotonic function-pointer/context clock,
and one explicitly started deadline worker. Lifecycle counts derive from records
under the existing mutex. Quiescence preserves evidence and topology; accepted
late evidence reopens traces. Startup is one-shot, stop is idempotent, and
shutdown stops the lifecycle worker after OTLP has joined without forcing
remaining active traces to become quiescent.

The Debug build passes all 110 CTest cases, including real localhost gRPC/HTTP
and Graphviz integrations. The new real-OTLP test sends a missing-parent child,
advances fake observation time and explicitly wakes the worker, checks retained
quiescence without topology churn, sends an empty export, then sends the late
parent and verifies reopening, gap removal, the confirmed service edge,
counters, and ordered shutdown. Deterministic manager/live-map tests cover exact
deadlines, resets, duplicate/empty input, one timestamp per publication/sweep,
coherent counts, owned snapshots, atomic lifecycle publication, both race orders,
retained telemetry presence, and idle/deadline worker cleanup.

Eleven concurrency-sensitive manager/live-map cases pass 100 repetitions each
(1,100 runs). A separate executable acceptance check used the production
30-second interval and a real external OTLP/gRPC client: the child became
quiescent, its HTTP SVG remained byte-identical, an empty export left it quiet,
and a late parent reopened it and replaced the missing-parent gap with the
confirmed `frontend -> checkout` edge. Ctrl-C returned zero and produced
inspected DOT/SVG artifacts and both retained spans. Both receiver publication-
order regressions also pass 100 repetitions each (200 runs). Final diff review
and whitespace checks pass. No remote V2.3 push or CI result has been verified.
Native Windows execution remains untested on this macOS host.

## Next concrete step

Review and integrate V2.3. Define the bounded retention and deterministic
eviction policy separately for V2.4.

## Accepted decisions

- Project name: **Silhouette**.
- Core thesis: reconstruct observed architecture and request flow, while exposing gaps in the observation itself.
- Core engine language: **C++** for systems-learning depth.
- Input: real OpenTelemetry traces over standard OTLP from the beginning.
- Integration target: a completely separate existing instrumented application, used for manual testing only.
- V1 lifecycle: finite in-memory capture stopped with Ctrl-C, followed by batch processing.
- V2.1 lifecycle: continuously inspectable active traces with no finalization or eviction yet.
- V2.3 lifecycle: active/quiescent state is the source of truth; derive counts coherently under the manager mutex without cached lifecycle counters. Every non-empty accepted batch refreshes affected traces, including duplicates; empty exports do nothing.
- Quiescence timing: use one steady-clock observation time per publication and per sweep. Clock callbacks are monotonic, noexcept, non-blocking, non-reentrant, and their contexts outlive the manager and worker.
- Lifecycle worker: one manager-owned worker waits for the nearest remaining duration, or indefinitely without active traces. Recheck current metadata on every wake. Start is one-shot; stop-before-start does not consume it, repeated stop is harmless, and destruction joins the worker. Fake-clock tests explicitly wake and acknowledge sweeps rather than relying on physical waiting.
- Retained lifecycle: snapshots include active and quiescent traces, and retained count equals active plus quiescent. Quiescence does not notify topology or render; retained count drives telemetry presence.
- V2.2 update semantics: dirty trace IDs are latest-state invalidations, not immutable export snapshots; normal topology work reconstructs only affected traces.
- Incremental topology: store one `ServiceGraph` contribution per trace and maintain deterministic service, edge, and gap reference counts; `ReplaceAll()` is failure recovery only.
- Incremental-update exception safety: preparation failure preserves the observable graph returned by snapshots; invisible empty or zero-count bookkeeping entries may remain.
- Update publication: serialize only manager publication, accepted counters, and dirty notification; never hold that mutex during reconstruction, graph work, Graphviz, HTTP, or SSE.
- Live-map versions: topology versions count changes to the deterministic graph plus telemetry presence; rendered versions identify the topology used for a successfully published SVG. Suppress equivalent successfully rendered states, retry failed current states on a new event without advancing their version, and discard stale completed SVGs.
- Presentation backpressure: every topology update is applied, while the Graphviz worker may coalesce obsolete intermediate visual frames; every render that does run performs a whole-current-graph layout.
- Live rendering: use private temporary DOT/SVG files and publish owned SVG bytes without mutating final `silhouette.dot` or `silhouette.svg` artifacts.
- Local presentation: serve embedded HTML/CSS/JavaScript, SVG, and server-sent events on `127.0.0.1:8008` through cpp-httplib; idle SSE streams use comment heartbeats to detect disconnected clients without browser polling. Limit active SSE streams below the fixed worker count to retain HTTP fetch capacity; excess streams receive a closed HTTP 503 response, and RAII releases admission slots on disconnect, shutdown, or failure.
- Failure isolation: dirty-notification failure requests a conservative full resync; graph, renderer, HTTP, and SSE failures must not reject accepted telemetry or terminate ingestion. HTTP startup waits for fixed-size worker-pool readiness and reports failures to the caller after closing its socket and joining the listener. Request-admission allocation failures reject only the unaccepted connection; queue teardown wakes SSE and joins workers even during listener unwinding.
- Shutdown order: stop and join OTLP, stop and join lifecycle work without forcing quiescence, close and drain invalidations, render the newest topology, stop HTTP/SSE, then run the final V1 artifact path.
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
- HTTP dependency: cpp-httplib is a presentation-only vcpkg dependency and does not enter the core or ingestion domain model.
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
- stable configuration for the OTLP and live-map listen addresses and ports;

An open decision is not permission for an agent to choose silently. Present options and tradeoffs when the decision becomes blocking, make the smallest reversible choice when authorized, and record the result here.

## Known issues

No known conversion, receiver, trace-reconstruction, service-aggregation, or
incremental-topology correctness issues. V2.3 intentionally retains every
observed trace and contribution for the process lifetime, including quiescent
traces. Bounded retention and eviction are not implemented. Each published live
frame still requires a full Graphviz layout, although obsolete pending frames
are coalesced. The live server is fixed to loopback port 8008 and has no stable
configuration interface. SVG generation requires external Graphviz on `PATH`;
fixed artifact paths, live messages, HTTP routes, and styling are not stable
public contracts. A failed live render retains the last valid SVG and waits for
new telemetry to retry; there is no timer or automatic retry loop.

## Latest work

- Added V2.3 active/quiescent lifecycle state, monotonic observation-time injection,
  derived coherent counts, and a one-shot manager-owned deadline worker.
- Preserved atomic publication and owned snapshots; late evidence reopens traces
  through the existing topology pipeline, while lifecycle-only transitions do
  not invalidate topology or render. Telemetry presence now uses retained count.
- Added deterministic deadline, count, snapshot, race, and shutdown coverage,
  plus real OTLP/gRPC and Graphviz reopening integration.
- Updated application status and shutdown ordering and documented the V2.3
  contract while keeping bounded retention and eviction separate.

- Reserved HTTP capacity by limiting persistent SSE admission; covered a full
  stream quota while page and updated SVG requests still complete, rejection of
  excess streams, and admission-slot recovery after disconnect.
- Made the version-event test signal its latch only once and explicitly wait
  for a subsequent heartbeat, exercising the previously unsafe repeated callback.
- Disabled unsafe dynamic HTTP worker expansion, contained queue-allocation
  failures at request admission, and made pool teardown release SSE waiters and
  join workers on both normal shutdown and listener exception unwinding.
- Added deterministic admission-failure coverage for subsequent HTTP requests,
  port release, shutdown with a busy SSE worker, and continued real OTLP export
  acceptance.
- Contained HTTP listener exceptions and made worker-pool startup failure reach
  the caller synchronously, preserving the local diagnostic and OTLP startup
  path. Regression tests verify port release, safe shutdown, and real OTLP
  acceptance after injected HTTP worker failure.
- Quoted Windows live-Graphviz paths with CRT-compatible escaping. Portable
  tests cover input/output paths with spaces, tabs, empty arguments, quotes,
  and trailing backslashes; the existing POSIX process path is unchanged.
- Made equivalent telemetry retry a current topology that lacks a successful
  SVG, without advancing its version. Successful publication cancels redundant
  retry requests queued while that render was in flight.
- Added deterministic coverage for failed-render recovery on equivalent
  evidence, version-zero recovery, waiting without a retry loop, retained
  authoritative evidence, and no duplicate render after an in-flight success.
- Added value equality for `ServiceGraph` and suppressed topology version and
  render requests when both the deterministic graph and telemetry presence are
  unchanged, after normal contribution/refcount maintenance.
- Added deterministic coverage for equivalent trace contributions, same-service
  evidence, telemetry-presence changes with an empty graph, later gap/edge
  changes, retained spans, and authoritative-builder equivalence.
- Corrected README capability grammar and described the pinned vcpkg baseline
  for `grpc`, `protobuf`, and presentation-only `cpp-httplib`; aligned version
  and map-server failure semantics across README, PROJECT, and STATUS.
- Isolated live HTTP startup errors so an occupied map port reports a diagnostic
  and OTLP collection continues; the viewer URL is printed only after successful
  startup. Verified the actual executable with port 8008 occupied and available:
  both accepted a real two-span export and exited cleanly with final DOT/SVG.
- Added sorted unique dirty-trace publication from the active manager and
  targeted owned `SnapshotTraces()` reconstruction outside its mutex.
- Added per-trace topology contribution replacement with service, edge, and gap
  reference counts, deterministic snapshots, and an atomic full-resync path.
- Added separate graph and render workers, monotonic topology/rendered versions,
  stale-frame discard, event coalescing, diagnostic failure isolation, and
  deterministic shutdown draining.
- Added an owned live Graphviz renderer that uses private temporary files and
  leaves final V1 artifact paths untouched.
- Added a thin cpp-httplib loopback server with an embedded viewer, `/graph.svg`,
  and stop-aware `/events` server-sent events.
- Added deterministic coverage for targeted snapshots, contribution replacement
  and shared references, direct-builder equivalence, atomic concurrent reads,
  newest-authoritative-state invalidation, notification ordering, partial
  dirty-set full-resync recovery, stale and failed renders, pending coalesced
  shutdown drain behavior, live rendering, HTTP routes, SSE events, and SSE
  heartbeat/disconnect handling and shutdown.
- Fixed recovery notification publication under the dirty mutex, covered failed
  first insertion as well as partial insertion, and synchronized the render
  failure diagnostic assertion by joining the workers before reading the stream.
- Verified a fresh warnings-enabled build and all 87 CTest cases, plus 100
  repeated runs each of twelve concurrency, retry, stale-render, resync, and
  drain cases. Real Graphviz failure injection recovered on equivalent OTLP
  evidence and published the unchanged topology version through HTTP/SSE.
- Verified the actual executable with a two-span `frontend -> checkout` OTLP
  export: the page and empty SVG were immediately available, SSE published
  rendered version 1, the live SVG showed both services, and Ctrl-C produced the
  expected final trace, edge, DOT, and SVG.

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

Bounded retention and eviction, persistence,
request playback, incremental Graphviz layout, configurable networking, and a
larger browser interface remain deferred beyond V2.3. Candidate later
directions remain listed in `ROADMAP.md`.
