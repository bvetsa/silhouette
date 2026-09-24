# Project Status

Last updated: 2026-09-24

## NOW

Review and approve the repository context documents. No product code has been implemented.

## NEXT

Begin V1 Milestone 1, Step 1: choose the smallest executable C++ project structure, build/test tooling, and initial OTLP transport. Do not begin this step until the documentation review is complete or the user explicitly asks to proceed.

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

## Open decisions

These should be decided only when Milestone 1 requires them:

- repository layout;
- C++ language standard;
- build system and test framework;
- OTLP/gRPC versus OTLP/HTTP as the first supported transport;
- exact OpenTelemetry/protobuf/networking dependencies;
- CLI name, flags, defaults, and output locations;
- precise graph styling and observability-gap notation;
- whether minimal status/error fields are necessary in V1's internal span model.

An open decision is not permission for an agent to choose silently. Present options and tradeoffs when the decision becomes blocking, make the smallest reversible choice when authorized, and record the result here.

## Known issues

None yet; implementation has not started.

## LATER

The candidate backlog is maintained in `ROADMAP.md`. Items there must not expand V1 unless the user explicitly redefines the version.

## Work-log template

At the end of a substantive work session, update this file with:

- what changed;
- what is broken, uncertain, or blocked;
- the next concrete task;
- decisions made and why;
- ideas deferred to LATER.
