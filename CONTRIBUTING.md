# Contributing to Silhouette

Contributions should stay within explicitly requested work, preserve the project contract, and produce a reviewable result.

## Before making a change

1. Read `PROJECT.md` for the durable product and architecture boundaries.
2. Read `ROADMAP.md` for version-level goals and `STATUS.md` for the current task and decisions.
3. Confirm that the requested work is within scope.
4. Identify the smallest user-visible or executable result the change should produce.
5. If a required design choice is open in `STATUS.md`, discuss its tradeoffs before embedding it deeply in code.

## Development principles

- Keep OTLP/protobuf concerns at the ingestion boundary and the core model transport-independent.
- Prefer clear ownership and lifetimes over clever abstractions.
- Begin with a simple correct design; add concurrency or optimization only after relevant measurements.
- Treat incomplete telemetry as ordinary input, not an exceptional afterthought.
- Preserve uncertainty. Never fabricate a parent, service edge, or complete-system claim.
- Keep the external integration application out of this repository.
- Use established libraries and standards for OTLP, protobuf, networking, and graph rendering instead of rebuilding them.
- Avoid speculative frameworks, generalized plugin systems, and abstractions without a defined use case.
- Keep changes narrow; do not mix a requested result with unrelated cleanup.

## Build and test workflow

Silhouette requires a C++20 compiler and CMake 3.24 or newer.

Configure, build, test, and run with:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build
ctest --test-dir build --output-on-failure
./build/silhouette
```

When new dependencies, test targets, or runtime requirements are introduced, document the reproducible workflow with the same change.

## Testing expectations

Every behavior change should have the narrowest meaningful automated test. Reconstruction and graph tests should prefer deterministic synthetic spans with stable IDs and timestamps.

When the affected behavior exists, cover the applicable cases:

- a single root and simple chain;
- branching child spans;
- multiple traces in one capture;
- arbitrary span arrival order;
- repeated service relationships;
- same-service nested spans;
- missing parents and orphan spans;
- disconnected fragments or multiple apparent roots;
- missing service identity;
- malformed input at the receiver boundary;
- graceful finite-capture shutdown.

Real OTLP validation is separate from algorithm tests. A passing synthetic suite does not prove the receiver works, and a successful manual demo does not replace precise algorithm tests. Report which layer was exercised.

## Definition of done

A change is done when:

- its requested behavior works through the relevant path;
- tests cover normal and applicable failure behavior;
- failures are explicit and do not create false certainty;
- documentation and `STATUS.md` reflect material behavior, workflow, or decision changes;
- no unrelated future feature was pulled into scope;
- the repository builds and relevant tests pass, or the exact exception is recorded;
- the next concrete task is clear.

A version is complete only when its acceptance outcome in `ROADMAP.md` and product criteria in `PROJECT.md` are demonstrated end to end.

## Review checklist

- Is the change inside the project contract and explicitly requested scope?
- Does it preserve the ingestion/domain/output separation?
- Does it work with out-of-order and incomplete evidence where relevant?
- Are outputs deterministic enough to test and inspect?
- Are ownership, error handling, and shutdown behavior clear?
- Is added complexity supported by a current requirement or measurement?
- Are claims limited to what the telemetry proves?
- Were relevant tests and generated artifacts actually inspected?

## Commits and documentation

Keep commits focused and describe behavior rather than internal activity. Do not combine broad refactors with a product result unless the refactor is necessary to make that result correct.

Record current work, task-level follow-ups, accepted decisions, and open decisions in `STATUS.md`. Reserve `ROADMAP.md` for version-scale goals and future directions.
