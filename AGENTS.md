# Coding-Agent Instructions

These instructions apply to the entire repository.

## Read first

Before planning or changing code, read:

1. `PROJECT.md` for the product contract and architecture boundaries;
2. `ROADMAP.md` for the current version and milestone definitions;
3. `STATUS.md` for current work, accepted decisions, and unresolved choices;
4. `CONTRIBUTING.md` for testing and completion expectations.

If these documents disagree, stop and surface the conflict. Do not silently choose the interpretation that permits more work.

## Current authorization boundary

The repository is in documentation review. **Do not implement or scaffold product code unless the user explicitly asks to begin implementation.** Documentation edits and read-only investigation do not imply authorization to start Milestone 1.

## Product invariants

- Silhouette reconstructs the **observed** system. Never describe its graph as guaranteed complete.
- Real OTLP input is required; synthetic data supports tests but is not the product boundary.
- The external instrumented application must remain outside this repository.
- OpenTelemetry creates and propagates trace context; Silhouette consumes the exported evidence.
- Protocol-generated objects must be converted at the ingestion boundary into a small internal model.
- Parent-child reconstruction must not rely on arrival order.
- Incomplete telemetry must yield explicit gaps and partial useful output, not crashes or fabricated certainty.
- The C++ engine is the center of the project; graph presentation should remain lightweight in V1.

## Scope discipline

Work only on the current task within the current milestone. Keep future ideas in LATER.

When a new idea appears, classify it:

- **Blocking flaw:** V1 cannot be correct or usable without it. Explain why and address it in the smallest possible way.
- **Improvement:** useful but not required for V1. Record it for later and continue the current task.

Do not implement live views, persistence, dashboards, request playback, latency analysis, advanced inference, production deployment, or unmeasured performance optimizations during V1.

## How to collaborate with the user

This is both a product and a learning project. Agents should help the user retain architectural ownership.

- Explain an unfamiliar concept when it becomes necessary, using the current code path as the example.
- Present meaningful design options with concrete tradeoffs before making a hard-to-reverse choice.
- Do not turn every reversible implementation detail into a planning meeting.
- Research only what the current task needs, then end the research with a decision, experiment, implementation, or test.
- Distinguish facts verified in the repository from assumptions and proposals.
- Never claim an external integration, benchmark, or end-to-end path succeeded unless it was actually exercised.

## Implementation expectations

When implementation is authorized:

- prefer a small, explicit, single-threaded design first;
- make ownership and lifetimes easy to reason about;
- keep ingestion, domain/reconstruction, aggregation, and output responsibilities separate;
- use standard OTLP/protobuf libraries rather than custom wire formats or parsers;
- keep output deterministic where practical;
- include failure context in errors and warnings;
- handle Ctrl-C and receiver shutdown deliberately;
- profile and benchmark before introducing concurrency, sharding, custom allocation, or other performance complexity;
- avoid abstractions that have only hypothetical future consumers.

## Verification expectations

Run the narrow tests for every change, then the broader relevant suite. Validate both positive behavior and damaged telemetry. For rendered graph changes, inspect the actual generated artifact rather than assuming valid DOT means a useful map.

Keep synthetic algorithm verification distinct from real OTLP integration. Report precisely which layer was tested.

## Project-state hygiene

At the end of substantive work:

- update `STATUS.md` with what changed, unresolved problems, and the next concrete task;
- update accepted/open decisions when a choice is made;
- keep `PROJECT.md` stable unless the product contract actually changes;
- update `ROADMAP.md` only when scope, milestone state, or the candidate backlog changes;
- leave the repository buildable and tests passing, or clearly record the exact exception.

Do not rewrite history, delete user work, or make unrelated cleanup changes. Keep changes focused and reviewable.
