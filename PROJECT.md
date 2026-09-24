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
- retaining the finite capture in memory;
- reconstructing traces from arbitrarily ordered spans;
- identifying incomplete or inconsistent evidence;
- aggregating cross-service relationships;
- generating textual and static graph output.

Silhouette does **not** implement an OpenTelemetry SDK, instrumentation library, context propagator, custom trace protocol, or protobuf implementation. The external application used for manual integration testing must remain outside this repository and outside the product architecture.

## V1 contract

### Input and lifecycle

- Receive real OTLP trace data from an external instrumented application.
- Operate as a finite capture: collect until a graceful Ctrl-C shutdown, then process the captured spans as a batch.
- Support traces only. Metrics and logs are outside V1.
- Choose one standard OTLP transport first; the choice between OTLP/gRPC and OTLP/HTTP remains open until implementation planning.

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
