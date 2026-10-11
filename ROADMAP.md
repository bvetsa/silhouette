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

## V2 — Continuous observed topology

V2.1 replaced finite-only span capture with continuously inspectable active
trace state. V2.2 incrementally maintains per-trace topology contributions and
serves an event-driven live service map on loopback port 8008. V2.3 distinguishes
active traces from operationally quiescent traces using monotonic observation
time; late accepted evidence reopens them. These slices keep the V1
reconstruction and uncertainty semantics and intentionally retain all observed
traces and their topology contributions for the process lifetime.

V2.4 bounds recent quiescent history by count and age, caps spans per logical
trace, and deterministically evicts complete quiescent records. The map converges
to currently retained support; late evidence after eviction starts fresh. Active
traces remain protected, so this is not a hard process-memory budget.

Quiescence is not proof of completeness. Historical playback, architectural
inference, and global resource/backpressure policy remain separate future work.
Other later V2 slices should be defined from implementation evidence.

## Future directions

Candidate directions include:

- confidence-aware observability-gap analysis;
- individual request playback;
- critical-path and concurrency analysis;
- latency, error, volume, and failure-propagation analysis;
- architecture inference and recurring request motifs;
- topology comparison over time;
- bounded active-trace state, timeouts, and backpressure;
- persistence and query capabilities;
- profiling, memory-layout work, and measurement-driven concurrency.

These are possible version themes, not commitments.
