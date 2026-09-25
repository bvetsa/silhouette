# Project Status

Last updated: 2026-09-24

## NOW

The pre-V0.1 foundation is in place: a CMake-based C++20 executable and dependency-free CTest smoke test. No OTLP, reconstruction, aggregation, or graph functionality has been implemented.

## NEXT

Define the first vertical V0.1 capability. Before its feature implementation begins, complete the remaining Milestone 1 setup decision by choosing the initial OTLP transport and its required dependencies.

## Accepted decisions

- Project name: **Silhouette**.
- Core thesis: reconstruct observed architecture and request flow, while exposing gaps in the observation itself.
- Core engine language: **C++** for systems-learning depth.
- Input: real OpenTelemetry traces over standard OTLP from the beginning.
- Integration target: a completely separate existing instrumented application, used for manual testing only.
- V1 lifecycle: finite in-memory capture stopped with Ctrl-C, followed by batch processing.
- V1 output: textual trace diagnostics plus a simple DOT/static visual service map.
- V1 robustness: tolerate and explicitly mark incomplete telemetry; do not attempt speculative relationship recovery.
- Test strategy: synthetic deterministic algorithm tests plus manual real-OTLP integration.
- Development strategy: vertical milestones, just-in-time learning, and measurement before optimization.
- Pre-V0.1 build baseline: CMake 3.24 or newer, C++20 with compiler extensions disabled, and standard warnings without warnings-as-errors.
- Initial repository layout: one executable source under `src/` and scaffold checks under `tests/`; add further boundaries only when implemented behavior requires them.
- Scaffold verification: built-in CTest with no third-party test dependency. The production unit-test framework remains undecided until V0.1 needs it.
- Scaffold executable target: `silhouette`. Its startup message is temporary and does not establish a stable CLI contract.

## Open decisions

These should be decided only when Milestone 1 or V0.1 requires them:

- unit-test framework for behavior beyond the dependency-free scaffold smoke test;
- OTLP/gRPC versus OTLP/HTTP as the first supported transport;
- exact OpenTelemetry/protobuf/networking dependencies;
- stable CLI flags, defaults, and output locations;
- precise graph styling and observability-gap notation;
- whether minimal status/error fields are necessary in V1's internal span model.

An open decision is not permission for an agent to choose silently. Present options and tradeoffs when the decision becomes blocking, make the smallest reversible choice when authorized, and record the result here.

## Known issues

No known scaffold issues. Product feature implementation has not started.

## Latest work

- Added the pre-V0.1 configure, build, executable, and smoke-test path.
- Verified the Debug build with Apple Clang and passed the CTest smoke test.
- Kept all product behavior and protocol dependencies out of the scaffold.
- Deferred the first OTLP transport, production test framework, and V0.1 capability definition.

## LATER

The candidate backlog is maintained in `ROADMAP.md`. Items there must not expand V1 unless the user explicitly redefines the version.

## Work-log template

At the end of a substantive work session, update this file with:

- what changed;
- what is broken, uncertain, or blocked;
- the next concrete task;
- decisions made and why;
- ideas deferred to LATER.
