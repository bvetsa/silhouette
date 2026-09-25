# Roadmap

## V1 — Observed architecture from OTLP traces

Silhouette V1 turns a finite capture of real OpenTelemetry trace data into correctly reconstructed request traces, an aggregated service map, and explicit observability-gap markers.

V1 is complete when a user can point a separate OpenTelemetry-instrumented application at Silhouette, generate a finite amount of traffic, stop capture cleanly, and receive:

```text
real OTLP
  -> internal spans
  -> reconstructed request traces
  -> aggregated service graph
  -> textual diagnostics + static visual map + visible observability gaps
```

The result must be reproducible by another developer and must demonstrate correct handling of simple chains, branching, multiple traces, repeated service edges, same-service child spans, out-of-order arrival, malformed input at the receiver boundary, graceful shutdown, and deliberately incomplete telemetry.

The complete V1 contract, system boundary, engineering principles, and non-goals are defined in `PROJECT.md`.

## Future versions

No post-V1 version scope is committed. A future version should be defined only after V1 is complete and its implementation evidence, limitations, and comparison with then-current tracing products have been reviewed.

Candidate directions include:

- confidence-aware observability-gap analysis;
- live ingestion and incremental map updates;
- individual request playback;
- critical-path and concurrency analysis;
- latency, error, volume, and failure-propagation analysis;
- architecture inference and recurring request motifs;
- topology comparison over time;
- bounded active-trace state, timeouts, and backpressure;
- persistence and query capabilities;
- profiling, memory-layout work, and measurement-driven concurrency.

These are possible version themes, not commitments.
