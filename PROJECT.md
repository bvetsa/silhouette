# Project Specification

## Thesis

Silhouette reconstructs and analyzes a system's **observed architecture** and causal request flow from OpenTelemetry traces, with special attention to uncertainty, broken observability, and—after V1—architectural weakness and latency.

“Observed” is deliberate. Silhouette may only claim what the received telemetry supports. Missing instrumentation or broken context propagation can make a functioning system appear disconnected; Silhouette should surface that gap rather than silently invent a relationship.

## Product model

Silhouette performs two related graph transformations:

1. **Trace reconstruction:** group spans by `trace_id`, index them by `span_id`, and connect them through `parent_span_id` to recover the causal structure of each request.
2. **Architecture projection:** project cross-service parent-child relationships from many reconstructed traces into a directed service graph for the capture.

A trace is normally modeled as a rooted span tree for V1, although malformed or incomplete input may produce multiple roots or disconnected fragments. The service graph is an aggregate view; it is not the same structure as an individual trace.

## System boundary

OpenTelemetry is responsible for:

- instrumenting applications and creating spans;
- generating trace and span identifiers;
- propagating trace context between services;
- serializing and exporting telemetry through OTLP.

Silhouette is responsible for:

- receiving OTLP trace exports;
- extracting span data and resource-level service identity;
- converting protocol objects into its own internal representation;
- retaining active trace evidence in memory while ingestion continues;
- reconstructing traces from arbitrarily ordered spans;
- identifying incomplete or inconsistent evidence;
- aggregating cross-service relationships;
- incrementally maintaining the latest observed service topology;
- generating a local event-driven live map plus final textual and static graph output.

Silhouette does **not** implement an OpenTelemetry SDK, instrumentation library, context propagator, custom trace protocol, or protobuf implementation. The external application used for manual integration testing must remain outside this repository and outside the product architecture.

## V1 contract

### Input and lifecycle

- Receive real OTLP trace data from an external instrumented application.
- Operate as a finite capture: collect until a graceful Ctrl-C shutdown, then process the captured spans as a batch.
- Support traces only. Metrics and logs are outside V1.
- Use OTLP/gRPC as the first transport. A future OTLP/HTTP receiver may be added at the ingestion boundary without changing the core engine or its domain model.

### Minimal internal span information

The internal model should retain only information needed by the engine, initially:

- trace ID;
- span ID;
- parent span ID, if present;
- service name, normally extracted from the OTel resource;
- operation/span name;
- start and end timestamps;
- minimal status or error information only if needed by V1 diagnostics.

The core engine must not depend on OpenTelemetry protobuf objects as its domain model. Protocol conversion belongs at the ingestion boundary.

### Output

After capture, V1 produces:

- a readable textual/debug view of reconstructed traces and warnings;
- a directed service graph aggregated across the capture;
- a DOT representation and a rendered static graph such as SVG;
- explicit indications of observability gaps wherever the evidence is incomplete.

### Required failure handling

V1 must tolerate, retain, and report at least:

- a span whose parent is missing;
- orphan spans and disconnected trace fragments;
- incomplete traces;
- missing service identity;
- malformed input at the ingestion boundary without crashing the full process.

V1 preserves all topology that can be established. It does not infer missing relationships from timing, attributes, logs, or network evidence, and it must not turn a guess into a certain edge.

## Acceptance criteria

V1 is complete when a user can point a separate OpenTelemetry-instrumented application at Silhouette, generate a finite amount of traffic, stop capture cleanly, and receive:

```text
real OTLP
  -> internal spans
  -> reconstructed request traces
  -> aggregated service graph
  -> textual diagnostics + static visual map + visible observability gaps
```

Correctness must be demonstrated for simple chains, branching, multiple traces, repeated service edges, same-service child spans, out-of-order arrival, and deliberately incomplete telemetry.

## Engineering principles

### Correctness before sophistication

Start with the smallest clear implementation, preferably single-threaded. Establish correctness and useful measurements before adding concurrency, specialized memory layouts, or other optimizations.

### Evidence before certainty

Telemetry is evidence about execution, not an infallible description of the complete deployed system. Preserve uncertainty in internal structures and outputs.

### Vertical progress

Each milestone must end in a runnable end-to-end capability. Avoid long periods of building isolated layers that have never been integrated.

### Just-in-time learning

Research should terminate in a design decision, implementation step, or test. Learn unfamiliar C++, networking, OTLP, and distributed-systems concepts immediately before they are needed; do not front-load broad onboarding.

### Measurement before optimization

The long-term project should include memory management, buffering, backpressure, profiling, and performance work. These are not reasons to complicate V1 prematurely. Benchmark and profile first, then change the design when evidence justifies it.

### Lightweight presentation

The engine is the project. Use established tools for serialization, transport, and graph rendering. Do not turn V1 into a frontend project.

## V1 non-goals

- live or continuously updating maps;
- persistent or distributed trace storage;
- a polished dashboard or request-playback UI;
- latency, bottleneck, or critical-path analysis;
- speculative reconstruction of missing relationships;
- production deployment, multi-tenancy, authentication, or authorization;
- metrics or log ingestion;
- sampling algorithms;
- performance or concurrency optimization without measurements;
- bundling an example application into this repository.

## Long-term differentiation

Silhouette must not settle into being an open-source AWS X-Ray clone. V1 intentionally builds the substrate that established tracing products also have. At the V1 review, compare Silhouette with the then-current X-Ray/CloudWatch feature set and choose a bounded V2 thesis from evidence.

Promising directions include:

- locating and classifying observability gaps;
- assigning confidence or coverage to inferred topology;
- finding critical paths and separating blocking work from concurrent work;
- inferring architectural motifs, central dependencies, fan-out/fan-in, and hidden coupling;
- detecting fragile dependencies, bottleneck edges, or failure-propagation paths;
- comparing architecture snapshots over time;
- remaining local, portable, and vendor-neutral.

These are candidates, not V1 commitments.

## V2.2 contract — event-driven live service map

V2.2 keeps the V1 evidence and graph semantics while making the service map
continuously visible during ingestion:

```text
accepted OTLP export
  -> atomic active-trace publication
  -> sorted unique dirty trace IDs
  -> targeted latest-state reconstruction
  -> per-trace contribution replacement
  -> reference-counted aggregate topology
  -> observable state comparison (graph plus telemetry presence)
  -> changed topology version only
  -> coalescing Graphviz render
  -> rendered-version SSE event
  -> browser map update
```

Dirty IDs are invalidations, not immutable export snapshots. If several exports
modify one trace before the graph worker runs, it reconstructs that trace from
the newest retained evidence. Normal updates never rebuild unrelated traces.
A full `ReplaceAll()` exists only as recovery if dirty-notification publication
fails. Once dirty-set insertion fails, the accumulated dirty IDs are advisory
until a successful full snapshot and `ReplaceAll()` resynchronize the graph.
Incremental-update preparation failures preserve the observable graph snapshot;
invisible empty or zero-count bookkeeping entries are not part of that contract.

`topology_version` advances only when the deterministic `ServiceGraph` or the
viewer-relevant `has_telemetry` state changes. Equivalent evidence still updates
contributions and reference counts, but causes no version advance. It suppresses
rendering only when the current topology already has a successfully published
SVG; otherwise a new event requests a retry of that same version. A failed
attempt retains the last valid SVG and waits for another event, without a timer
or automatic retry loop. A successful current render cancels any equivalent
retry requests queued during that attempt.
`rendered_version` identifies the topology version of a successfully published
SVG. Topology and rendering have separate monotonic versions. A render is published
only if its topology version is still current; obsolete intermediate SVG frames
may be discarded, but topology updates are not. Live Graphviz work uses private
temporary files and owned SVG bytes. The loopback HTTP/SSE presentation adapter
is intentionally thin and remains outside ingestion and the core graph model.
Incremental reconstruction and topology maintenance do not imply incremental
layout: each render still asks Graphviz to lay out the whole current graph, while
obsolete pending render requests may be coalesced.

HTTP/SSE presentation failures must not disable OTLP ingestion. Map-server
startup failures, including an occupied loopback port 8008 or HTTP worker-pool
failure, are diagnosed locally while the receiver continues starting. HTTP
startup waits for worker-pool readiness; a failed start joins the listener and
releases its socket before returning the error. The viewer URL is printed only
when the map server starts successfully.
The HTTP worker pool is fixed-size, using cpp-httplib's initial worker count,
rather than growing threads during request admission. Failed queue allocation
rejects only the unaccepted request, whose socket is closed by cpp-httplib; the
listener and OTLP receiver remain available. Queue teardown wakes SSE waiters
and joins its workers even if the listener exits through an exception.
Persistent event streams are limited to one fewer than the HTTP worker count,
leaving capacity for page and SVG requests. Excess `/events` requests receive
HTTP 503 and a closed connection. An owned admission slot is released when a
stream disconnects, shuts down, or fails, including during provider setup.

V2.2 does not add trace finalization, quiescence, eviction, bounded retention,
persistence, authentication, a frontend framework, or incremental Graphviz
layout. Active traces and their contributions remain process-lifetime state.
