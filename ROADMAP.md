# Roadmap

## Project-management rule

Silhouette has no fixed final version, but every individual version is finite:

```text
define V# -> freeze scope -> plan vertical milestones -> build -> verify -> document
          -> review the completed version -> choose the next bounded version
```

During a version:

- **NOW** is the one current concrete task.
- **NEXT** is the immediately following milestone or task needed for sound design.
- **LATER** contains good ideas that must not expand the current version.

Only a flaw that prevents the current version from being correct or usable justifies an unplanned scope change. Other improvements go to the backlog.

## V1 goal

Turn a finite real OTLP trace capture into correctly reconstructed request traces, an aggregated service map, and explicit observability-gap markers.

## Milestone 1 — Real OTLP to reconstructed traces

Deliver a runnable path from an external exporter to correct textual trace trees.

1. Choose the smallest executable C++ project structure, build system, test framework, and first OTLP transport.
2. Define the minimal internal span model and keep it independent of protobuf-generated types.
3. Receive a real OTLP trace export.
4. Convert `ResourceSpans -> ScopeSpans -> Span` into internal spans, combining resource service identity with span fields.
5. Group spans by trace ID and reconstruct parent-child relationships independent of arrival order.
6. Print a deterministic, readable trace representation.
7. Add synthetic tests for chains, branching, multiple traces, and out-of-order spans.

**Done when:** a separate OTel-instrumented application can send real telemetry to Silhouette and Silhouette prints the correct request trees.

## Milestone 2 — Reconstructed traces to service map

Deliver a useful observed architecture for the finite capture.

1. Define how span relationships become cross-service relationships.
2. Avoid creating service edges for same-service internal parent-child spans.
3. Aggregate nodes, directed edges, counts, and only the minimal metadata V1 needs.
4. Emit a deterministic DOT graph and render a simple static graph such as SVG.
5. Retain textual trace output beside the visual result.
6. Test repeated edges, branching services, multiple trace shapes, and same-service nesting.

**Done when:** finite real telemetry yields a meaningful service map whose edges can be traced back to observed parent-child relationships.

## Milestone 3 — Incomplete telemetry to visible failure points

Deliver a partial-but-honest result from damaged data.

1. Represent missing parents, orphan spans, disconnected fragments, incomplete traces, and missing service identity.
2. Ensure those cases do not crash reconstruction or discard knowable topology.
3. Emit precise warnings in textual output.
4. Mark gaps or uncertainty clearly in graph output without inventing missing edges.
5. Add deliberately damaged synthetic test cases.

**Done when:** incomplete telemetry produces a partial, useful map with visible gaps rather than failure or false certainty.

## Milestone 4 — Integrated V1

Deliver a coherent and reproducible version.

1. Implement the complete lifecycle: start receiver, accept telemetry, stop with Ctrl-C, close ingestion cleanly, process, and generate outputs.
2. Validate against a separate real multi-service OTel application.
3. Verify malformed input and graceful-shutdown behavior.
4. Make CLI and configuration behavior understandable without building a broad configuration system.
5. Document build, run, integration, supported behavior, non-goals, and known limitations.
6. Run the full V1 acceptance suite and review the rendered output.

**Done when:** all acceptance criteria in `PROJECT.md` hold end to end and another developer can reproduce the workflow from the repository documentation.

## V1 review gate

Before any V2 implementation:

1. Confirm V1 is complete rather than “mostly complete.”
2. Record what worked, what was painful, and which architectural assumptions should change.
3. Re-evaluate current AWS X-Ray and CloudWatch capabilities.
4. Choose one primary differentiator and define a bounded V2 around it.
5. Move all unselected ideas back to the candidate backlog.

## Later candidates — not commitments

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
