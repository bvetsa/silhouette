# Project Status

Last updated: 2026-09-25

## Current status

The pre-V0.1 foundation is in place: a CMake-based C++20 executable and dependency-free CTest smoke test. No OTLP ingestion, trace reconstruction, service aggregation, or graph generation functionality has been implemented.

## Current task

The final pre-product documentation cleanup is complete. No product feature implementation task is active.

## Next concrete step

Select and explicitly authorize the next product task before implementation. The initial OTLP transport and smallest required dependency set remain blocking design choices.

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
- Development strategy: vertical progress, just-in-time learning, and measurement before optimization.
- Build baseline: CMake 3.24 or newer, C++20 with compiler extensions disabled, and standard warnings without warnings-as-errors.
- Initial repository layout: one executable source under `src/` and checks under `tests/`; add further boundaries only when implemented behavior requires them.
- Baseline verification: built-in CTest with no third-party test dependency. The production unit-test framework remains undecided until product behavior needs it.
- Executable target: `silhouette`. Its current startup message is temporary and does not establish a stable CLI contract.

## Open decisions

These should be decided only when authorized work requires them:

- unit-test framework for behavior beyond the dependency-free smoke test;
- OTLP/gRPC versus OTLP/HTTP as the first supported transport;
- exact OpenTelemetry/protobuf/networking dependencies;
- stable CLI flags, defaults, and output locations;
- precise graph styling and observability-gap notation;
- whether minimal status/error fields are necessary in V1's internal span model.

An open decision is not permission for an agent to choose silently. Present options and tradeoffs when the decision becomes blocking, make the smallest reversible choice when authorized, and record the result here.

## Known issues

No known scaffold issues. Product feature implementation has not started.

## Latest work

- Added and verified the configure, build, executable, and smoke-test path.
- Kept all product behavior and protocol dependencies out of the foundation.
- Completed the final pre-product documentation cleanup without changing repository behavior.

## Deferred work

No task-level follow-ups are currently recorded. Possible post-V1 version directions are listed in `ROADMAP.md`.
