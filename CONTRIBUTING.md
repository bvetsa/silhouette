# Contributing to Silhouette

Silhouette is developed as a sequence of bounded, end-to-end versions. Contributions should advance the current milestone without quietly expanding its scope.

## Before making a change

1. Read `PROJECT.md`, `ROADMAP.md`, and `STATUS.md`.
2. Confirm the requested work belongs to the current version and milestone.
3. Identify the smallest user-visible or executable result the change should produce.
4. Learn only the unfamiliar concepts that block that result.
5. If a required design choice is still open in `STATUS.md`, discuss it before embedding it deeply in code.

## Change principles

- Keep OTLP/protobuf concerns at the ingestion boundary and the core model transport-independent.
- Prefer clear ownership and lifetimes over clever abstractions.
- Begin with a simple correct design; add concurrency or optimization only after relevant measurements.
- Treat incomplete telemetry as ordinary input, not an exceptional afterthought.
- Preserve uncertainty. Never fabricate a parent, service edge, or complete-system claim.
- Keep the external integration application out of this repository.
- Use established libraries and standards for OTLP, protobuf, networking, and graph rendering instead of rebuilding them.
- Avoid speculative frameworks, generalized plugin systems, and abstractions without a current V1 use case.

## Testing expectations

Every behavior change should have the narrowest meaningful automated test. Reconstruction and graph tests should prefer deterministic synthetic spans with stable IDs and timestamps.

Configure, build, and run the current scaffold checks with:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build
ctest --test-dir build --output-on-failure
```

CTest currently verifies only that the pre-V0.1 executable starts and reports its scaffold state. It is not a substitute for the behavior-focused unit tests and real OTLP validation required as features are added.

At minimum, the V1 suite should cover:

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

Real OTLP validation is separate from algorithm tests. A passing synthetic suite does not prove the receiver works, and a successful manual demo does not replace precise algorithm tests.

## Definition of done

A change is done when:

- its requested behavior works through the relevant end-to-end path;
- tests cover normal and applicable failure behavior;
- failures are explicit and do not create false certainty;
- documentation and `STATUS.md` reflect material decisions or workflow changes;
- no unrelated future feature was pulled into scope;
- the next concrete task is clear.

A milestone is done only when its stated result in `ROADMAP.md` is runnable and demonstrated. A collection of unfinished components is not a completed milestone.

## Review checklist

- Is the change inside the current V1 boundary?
- Does it preserve the ingestion/domain/output separation?
- Does it work with out-of-order and incomplete evidence where relevant?
- Are outputs deterministic enough to test and inspect?
- Are ownership, error handling, and shutdown behavior clear?
- Is added complexity supported by a current requirement or measurement?
- Are claims limited to what the telemetry proves?

## Commits and documentation

Keep changes focused and describe behavior rather than internal activity. Do not mix broad refactors with a milestone deliverable unless the refactor is necessary to make that deliverable correct.

When deferring an idea, add it to the `ROADMAP.md` candidate backlog or the LATER section of `STATUS.md`; do not leave it as an ambiguous TODO inside implementation code.
