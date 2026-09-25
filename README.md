# Silhouette

Silhouette is a local, vendor-neutral distributed-tracing and architecture-analysis system built around OpenTelemetry. It reconstructs the **observed architecture** and causal request flow of a system from trace data, while making incomplete or inconsistent telemetry visible instead of presenting an uncertain map as fact.

## Status

Silhouette has a **pre-V0.1 C++ scaffold**. The executable and smoke-test path are buildable, but no product features have been implemented. V0.1 will be the first vertical product capability; the broader V1 scope remains frozen as described below.

## Build the scaffold

The scaffold requires a C++20 compiler and CMake 3.24 or newer.

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build
ctest --test-dir build --output-on-failure
./build/silhouette
```

The executable currently prints an explicit scaffold message and exits successfully. That message is temporary verification behavior, not a stable CLI contract.

## V1 in one flow

```text
external OTel-instrumented application
                |
                | real OTLP traces
                v
         finite capture
                |
                v
       internal C++ spans
                |
                v
  reconstructed request traces
                |
                v
   aggregated service graph
                |
                +--> textual trace diagnostics
                +--> DOT/SVG service map
                +--> explicit observability gaps
```

The user starts Silhouette, generates traffic in a completely separate instrumented application, and stops collection with Ctrl-C. Silhouette then processes the complete capture and emits its results.

V1 must:

- accept real OTLP trace exports;
- convert OpenTelemetry data into a transport-independent internal span model;
- group spans by trace and reconstruct parent-child structure regardless of arrival order;
- derive directed service relationships without treating every same-service internal span as a new service edge;
- produce readable trace diagnostics and a simple static service map;
- preserve useful partial topology and mark missing parents, orphan spans, disconnected fragments, incomplete traces, and missing service identity;
- validate algorithms with synthetic tests and the integration boundary with a separate real application.

## Why this project exists

The C++ engine is the technical center of the project. It creates a practical setting for learning ownership, lifetimes, memory layout, data structures, networking, concurrency, profiling, and performance. Those topics will be introduced only when the current implementation requires them.

A static service map is a foundation, not the final product thesis. After V1, Silhouette should be evaluated against current tools such as AWS X-Ray before choosing a differentiator. Candidate directions include observability-gap analysis, confidence-aware topology, critical-path reasoning, architecture inference, weakness detection, and architecture change over time.

## Documentation

- [PROJECT.md](PROJECT.md) — product thesis, system boundaries, V1 contract, and design principles
- [ROADMAP.md](ROADMAP.md) — bounded V1 milestones, completion criteria, and future candidates
- [STATUS.md](STATUS.md) — current state, accepted decisions, open decisions, and next task
- [CONTRIBUTING.md](CONTRIBUTING.md) — development and review workflow
- [AGENTS.md](AGENTS.md) — operating rules for coding agents
