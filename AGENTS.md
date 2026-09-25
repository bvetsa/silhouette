# Coding-Agent Guide

These instructions apply to any coding agent working in this repository.

## Before editing

- Inspect the affected code, tests, and relevant repository documentation before proposing or making changes.
- Check the working tree and preserve existing user work. Do not overwrite, revert, or reformat unrelated changes.
- Identify the existing architecture, conventions, contracts, and repository boundaries that apply to the task.
- If sources of truth conflict or a blocking requirement is ambiguous, surface the conflict instead of guessing.

## Making changes

- Implement the smallest coherent change that satisfies the request.
- Follow existing architecture and conventions unless the task explicitly requires changing them.
- Preserve public behavior, interfaces, file formats, and integration contracts unless an intentional change is requested.
- Do not add speculative features, dependencies, interfaces, placeholder layers, or abstractions with no current consumer.
- Do not mix the requested work with unrelated refactors, formatting, renames, or cleanup.
- Keep changes localized, readable, and easy to review.
- Respect repository boundaries. Do not move external applications, generated dependencies, secrets, or unrelated artifacts into the repository.

## C++ expectations

- Make ownership, lifetimes, and resource cleanup explicit and easy to reason about.
- Prefer RAII and clear value semantics; introduce shared ownership, raw owning pointers, or concurrency only when the design requires them.
- Keep error handling deliberate and include enough context to diagnose failures.
- Preserve deterministic behavior where practical, especially in output and tests.
- Avoid optimization or concurrency complexity without a demonstrated requirement or measurement.

## Verification

- Add or update tests for every behavior change, including applicable failure cases.
- Run the narrowest relevant checks first, then the broader build and test suite affected by the change.
- Inspect generated artifacts or integration output when correctness cannot be established by exit status alone.
- Distinguish verified behavior from assumptions. State exactly what was tested and do not claim external or end-to-end success unless it was exercised.

## Decisions and handoff

- For a blocking, hard-to-reverse design choice, present concrete options and tradeoffs rather than silently choosing.
- Do not silently expand scope. Record or report useful follow-up work without implementing it unless requested.
- Update `STATUS.md` only when the current project state, material decisions, known issues, or next concrete work actually change.
- Leave the repository coherent and buildable. Report any unresolved failure or limitation precisely.
