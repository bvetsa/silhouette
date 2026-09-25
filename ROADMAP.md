# Roadmap

## Version terminology

- **Pre-V0.1** is the buildable project scaffold only; it contains no product capability.
- **V0.1** is V1 Milestone 1: real OTLP input to correctly reconstructed textual traces.
- **V1** is complete only after Milestones 1–4 and the acceptance criteria in `PROJECT.md` are satisfied.

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

## V1 milestone sequence

1. **Milestone 1 / V0.1 — Real OTLP to reconstructed traces:** receive real OTLP from a separate application, convert it into the internal span model, reconstruct out-of-order parent-child relationships, and print deterministic textual traces.
2. **Milestone 2 — Reconstructed traces to service map:** aggregate cross-service relationships and produce deterministic DOT/static graph output.
3. **Milestone 3 — Incomplete telemetry to visible failure points:** retain useful partial topology while reporting and displaying observability gaps without invented relationships.
4. **Milestone 4 — Integrated V1:** complete graceful finite capture, real multi-service validation, malformed-input handling, reproducible documentation, and the full V1 acceptance suite.

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
