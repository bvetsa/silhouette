# Silhouette

Silhouette is a local, vendor-neutral distributed-tracing and architecture-analysis system built around OpenTelemetry. It reconstructs the **observed architecture** and causal request flow of a system from trace data, while making incomplete or inconsistent telemetry visible instead of presenting an uncertain map as fact.

## Capabilities

Silhouette's V1 product boundary is designed to:

- receive real OTLP trace exports from a separate instrumented application;
- convert OpenTelemetry protocol data into a small, transport-independent C++ span model;
- group spans by trace and reconstruct parent-child structure regardless of arrival order;
- derive directed service relationships without treating every same-service child span as a new service edge;
- produce readable trace diagnostics, deterministic DOT output, and a simple static service map;
- preserve useful partial topology while marking missing parents, orphan spans, disconnected fragments, incomplete traces, and missing service identity.

The product reconstructs only what the received telemetry supports. It does not invent missing relationships or claim that an observed map is a complete description of the deployed system.

## How it works

```text
external OpenTelemetry-instrumented application
                         |
                         | OTLP traces
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
                         +--> DOT/static service map
                         +--> explicit observability gaps
```

The usage model is to start Silhouette, send it telemetry from a separate application, generate a finite amount of traffic, stop collection cleanly with Ctrl-C, and inspect the generated diagnostics and graph. Runtime transport, configuration, and output details should be taken from the implemented interface; see `STATUS.md` for what is currently available.

## Build, test, and run

Silhouette requires a C++20 compiler and CMake 3.24 or newer.

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build
ctest --test-dir build --output-on-failure
./build/silhouette
```

The executable target is `silhouette`. Stable runtime flags, telemetry endpoints, and output locations will be documented here when they are established; temporary behavior and open implementation decisions belong in `STATUS.md`.

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
