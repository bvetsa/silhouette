# Coding-Agent Instructions

These instructions apply to the entire repository.

## Sources of truth

Before planning or changing the repository, read:

1. `PROJECT.md` for the product thesis, contract, and architecture boundaries;
2. `ROADMAP.md` for version-level goals and acceptance outcomes;
3. `STATUS.md` for current work, accepted and open decisions, known issues, and the next concrete step;
4. `CONTRIBUTING.md` for the development, testing, and review workflow;
5. `README.md` for the user-facing build and usage path.

If these documents disagree, stop and surface the conflict. Do not silently choose the interpretation that permits more work. A roadmap goal does not authorize implementation; follow the user's request and the active work recorded in `STATUS.md`.


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

Work only on the explicitly requested task and keep each change focused on the smallest coherent result.

When a new idea appears, classify it:

- **Blocking flaw:** the requested result cannot be correct or usable without it. Explain why and address it in the smallest practical way.
- **Improvement:** useful but not required for the requested result. Record it in `STATUS.md` if it needs follow-up, then continue the current task.

Do not introduce V1 non-goals or future-version capabilities unless the user explicitly changes the project scope. Avoid unrelated cleanup, speculative interfaces, placeholder layers, and abstractions with only hypothetical consumers.

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

Run the narrow tests for every change, then the broader relevant suite. Validate both expected behavior and applicable failure cases. For rendered graph changes, inspect the generated artifact rather than assuming valid DOT means a useful map.

Keep synthetic algorithm verification distinct from real OTLP integration. Report precisely which layer was tested and distinguish local validation from external end-to-end confirmation.

## Project-state hygiene

At the end of substantive work:

- update `STATUS.md` with what changed, unresolved problems, material decisions, and the next concrete task;
- change `PROJECT.md` only when the product thesis, contract, or architecture boundaries change;
- change `ROADMAP.md` only when version-scale goals, acceptance outcomes, or future directions change;
- keep user-facing build and usage guidance in `README.md` accurate;
- leave the repository buildable and tests passing, or record the exact exception in `STATUS.md`.

Do not rewrite history, delete user work, or make unrelated cleanup changes. Keep changes focused and reviewable.
