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

Silhouette requires a C++20 compiler, CMake 3.24 or newer, and
[vcpkg](https://github.com/microsoft/vcpkg). The manifest pins the gRPC and
protobuf dependency versions. CMake downloads the pinned OpenTelemetry protocol
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
`127.0.0.1:4317`. Stop it with Ctrl-C; it waits for in-flight requests and then
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
`silhouette_core`. Its private `WriteServiceGraphArtifacts()` helper is only a
V1 local CLI convenience, not a long-term renderer interface. Future web
renderers can consume `ServiceGraph` or DOT, and request playback can consume
`ReconstructedTrace` without changing the aggregate graph.

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
