# Silhouette

Silhouette is a local, vendor-neutral distributed-tracing and architecture-analysis system built around OpenTelemetry. It reconstructs the **observed architecture** and causal request flow of a system from trace data, while making incomplete or inconsistent telemetry visible instead of presenting an uncertain map as fact.

## Capabilities

Silhouette currently:

- receives real OTLP trace exports from a separate instrumented application;
- converts OpenTelemetry protocol data into a small, transport-independent C++ span model;
- groups spans by trace and reconstructs parent-child structure regardless of arrival order;
- derives directed service relationships without treating every same-service child span as a new service edge;
- retains inspectable traces while ingestion runs, distinguishing active traces
  from quiescent traces after 30 seconds without accepted evidence;
- reopens quiescent traces when late evidence arrives;
- updates the observed service topology incrementally when accepted telemetry changes;
- exposes an event-driven live SVG map at `http://127.0.0.1:8008`;
- produces readable trace diagnostics, deterministic DOT output, and a static service map at shutdown;
- preserves useful partial topology while marking missing parents, orphan spans, disconnected fragments, incomplete traces, and missing service identity.

The product reconstructs only what the received telemetry supports. It does not invent missing relationships or claim that an observed map is a complete description of the deployed system.

## How it works

```text
external OpenTelemetry-instrumented application
                         |
                         | OTLP traces
                         v
            retained trace state
                         |
                         v
           targeted trace reconstruction
                         |
                         v
       per-trace topology contributions
                         |
                         v
      reference-counted service graph
                         |
                         +--> Graphviz SVG --> SSE --> browser
                         +--> final textual diagnostics
                         +--> final DOT/static service map
                         +--> explicit observability gaps
```

Start Silhouette, open `http://127.0.0.1:8008`, and send telemetry from a
separate application. Each accepted non-empty OTLP export invalidates its
affected traces; a background worker reconstructs their latest authoritative
state and replaces only their topology contributions. A separate renderer may
coalesce obsolete visual frames while preserving every topology update. Stop
collection cleanly with Ctrl-C to drain pending work and write the final V1
diagnostics and artifacts. See `STATUS.md` for current limitations.

Equivalent topology evidence still updates retained traces and contribution
counts. A new topology version is requested only when the graph or the viewer's
telemetry-presence state changes. Equivalent updates suppress rendering once
that version has a successful SVG; otherwise, the next update retries the
current version without a timer or automatic retry loop. A published rendered
version identifies the topology version used for its SVG.

Quiescent means operationally quiet, not known complete. All retained evidence
still contributes to the map and remains available to reconstruction. Becoming
quiescent does not change topology or trigger rendering; late evidence uses the
ordinary update path. Status reports active and quiescent counts even when no
new exports arrive. Quiescent history is bounded to 1,000 traces and expires ten minutes after its
last accepted evidence. The oldest quiescent traces leave first; active traces
are protected. Eviction can remove services, edges, or gaps when their last
retained supporter disappears. That means retained support was lost, not that
Silhouette inferred the deployed relationship disappeared.

Each logical trace accepts at most 10,000 spans. An over-limit incoming trace
contribution is rejected as a whole while fitting sibling traces remain accepted,
with exact OTLP partial-success reporting. Rejected evidence does not keep a
trace active. Evidence arriving after eviction starts fresh; forgotten parents
remain visible as gaps. These limits bound recent history, not total process
memory under an unlimited active workload.

The incremental work is trace reconstruction and topology maintenance, not
Graphviz layout. Whenever rendering runs, Graphviz lays out the whole current
graph; only obsolete pending render requests are coalesced.

## Build, test, and run

Silhouette requires a C++20 compiler, CMake 3.24 or newer, and
[vcpkg](https://github.com/microsoft/vcpkg). The manifest's pinned vcpkg baseline
selects the versions of `grpc`, `protobuf`, and `cpp-httplib` (`grpc` includes
its `codegen` feature). CMake downloads the pinned OpenTelemetry protocol
definitions and generates their C++ bindings under `build/`.

Install [Graphviz](https://graphviz.org/) so `dot` is available on `PATH` for
SVG output and the rendering integration tests:

```bash
brew install graphviz                 # macOS
sudo apt-get install graphviz         # Ubuntu
```

After cloning and bootstrapping vcpkg, set `VCPKG_ROOT` to that checkout and run:

```bash
cmake -S . -B build \
  -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_TOOLCHAIN_FILE="$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake"
cmake --build build
ctest --test-dir build --output-on-failure
./build/silhouette
```

The `silhouette` executable listens for OTLP/gRPC trace exports on
`127.0.0.1:4317` and serves its live map on `http://127.0.0.1:8008`. Map-server
startup failures, including an occupied port or HTTP worker-pool failure, are
diagnosed while OTLP ingestion continues. The viewer URL is printed only after
successful startup. HTTP uses a fixed-size worker pool; a request-admission
allocation failure is diagnosed and closes only that connection, without
stopping the listener or ingestion.
SSE streams leave one worker available for page and SVG requests; excess event
streams receive HTTP 503 until a slot becomes available.
The local page is plain embedded HTML/CSS/JavaScript. It receives rendered-version events
over server-sent events and fetches completed in-memory SVG; there is no
frontend build or browser polling loop. Stop Silhouette with Ctrl-C; it waits
for in-flight requests, stops the trace lifecycle worker without forcing active
traces to become quiescent, drains topology and rendering work, and then
prints the accepted export-request and span totals followed by deterministic
reconstructed trace trees and a deterministic service-graph summary. Valid spans
are converted to protobuf-independent values and retained in memory as complete
export batches. Reconstruction groups spans by trace and resolves parent-child
relationships without relying on arrival order. Service aggregation retains all
known services, collapses repeated resolved cross-service relationships, and
does not turn same-service relationships into self-edges. Missing or ambiguous
parents, parent cycles, and missing service identity remain visible as
deduplicated graph gaps rather than being inferred or silently repaired. Spans
with malformed trace, span, or non-empty parent IDs are rejected individually
through OTLP partial success and do not contribute to the accepted span total.
The receiver uses local insecure transport and does not yet define command-line
configuration as a stable interface.

After printing the traces and service summary, Silhouette writes deterministic
`silhouette.dot` and renders `silhouette.svg` in the current working directory.
These fixed files are overwritten on each run. Open the SVG in a browser to
inspect the static map. Known services are blue rounded boxes, and confirmed
observed dependencies are solid directed edges. Gap facts use colored shapes
(amber diamond for missing service identity, red triangle for missing parent,
purple hexagon for ambiguous parent, teal octagon for cycle parent), with an
embedded legend. Dashed diagnostic connections use only the endpoints retained
in each gap fact; unknown endpoints are never recovered or drawn as synthetic
services. Endpoint-less gaps remain standalone diagnostic nodes.

If Graphviz is unavailable or rendering fails, the executable reports a
Graphviz/rendering error and returns a nonzero status. The DOT file is retained,
and stale or partial SVG output is removed; a cleanup failure is reported if the
filesystem prevents removal. DOT write failures report the file context.

`ServiceGraph` remains the architecture model. The separate
`silhouette_rendering` target provides the pure `FormatServiceGraphDot()`
presentation boundary without adding rendering dependencies to
`silhouette_core`. Live rendering uses private temporary files and returns owned
SVG bytes without touching the public `silhouette.dot` or `silhouette.svg`
paths. The private `WriteServiceGraphArtifacts()` helper remains the final V1
CLI artifact path. The HTTP/SSE adapter depends on the live-map and rendering
boundaries; ingestion and core code do not depend on browser or HTTP concepts.

## Design priorities

- correctness before sophistication;
- explicit uncertainty rather than fabricated certainty;
- clear C++ ownership and lifetimes;
- standard OTLP, protobuf, networking, and graph tooling;
- deterministic output where practical;
- measurement before concurrency or performance complexity;
- a lightweight presentation layer around the C++ engine.

## Documentation

- [PROJECT.md](PROJECT.md) — project thesis, system boundaries, V1 contract, and engineering principles
- [ROADMAP.md](ROADMAP.md) — version-level goals, acceptance outcomes, and possible future directions
- [STATUS.md](STATUS.md) — current work, accepted and open decisions, known issues, and the next concrete step
- [CONTRIBUTING.md](CONTRIBUTING.md) — development, testing, and review workflow
- [AGENTS.md](AGENTS.md) — durable operating rules for coding agents
