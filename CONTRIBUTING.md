# Contributing to Silhouette

Contributions should be focused, understandable, and verifiable.

## Setup and baseline checks

Silhouette requires a C++20 compiler and CMake 3.24 or newer.

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build
ctest --test-dir build --output-on-failure
./build/silhouette
```

Keep generated build output under `build/`. If a change introduces a dependency, tool, or runtime requirement, document the reproducible setup in the same change.

## Before making a change

- Read the affected code, tests, and relevant documentation.
- Understand the existing architecture, conventions, public contracts, and repository boundaries.
- Check the working tree and avoid disturbing unrelated work.
- Discuss major, breaking, or hard-to-reverse changes before embedding them in the implementation.

## Coding expectations

- Make the smallest coherent change that solves the stated problem.
- Follow existing architecture and naming conventions unless changing them is part of the approved work.
- Preserve public interfaces, behavior, file formats, and integrations unless the change intentionally revises them.
- Avoid speculative abstractions, dependencies, generalized frameworks, and placeholder layers.
- Keep C++ ownership, lifetimes, error handling, and resource cleanup explicit. Prefer RAII and simple value semantics.
- Add complexity such as concurrency or specialized optimization only when requirements or measurements justify it.
- Do not combine feature work with unrelated refactors, formatting, renames, or cleanup.
- Do not commit generated build output, secrets, local configuration, or external applications that belong outside this repository.

## Tests and verification

- Add or update automated tests for behavior changes, covering normal and applicable failure paths.
- Prefer deterministic inputs and outputs so failures are reproducible.
- Run targeted tests during development, then the broader relevant build and test suite before submission.
- Inspect generated artifacts and integration results when automated assertions do not fully establish correctness.
- Report exactly what was verified. Local tests, synthetic tests, and external integration tests are distinct forms of evidence.

## Major and breaking changes

- Explain the problem, affected contracts, compatibility impact, and considered alternatives.
- Keep migrations or transition behavior explicit when compatibility cannot be preserved.
- Justify new dependencies and document how they are built, configured, updated, and tested.
- Update affected contract or usage documentation in the same change.

## Source control and review

- Work on a focused branch and keep commits cohesive.
- Stage only intended files and review the diff before committing.
- Do not rewrite shared history or discard another contributor's work.
- Use commit and pull-request descriptions that explain what changed, why it changed, how it was verified, and any known limitations.
- Keep changes small enough to review; split unrelated work into separate commits or submissions.

## Completion checklist

- The requested behavior is implemented without unapproved scope expansion.
- Relevant tests were added or updated and pass.
- The repository builds through the documented workflow.
- Public contracts and documentation remain accurate.
- `STATUS.md` is updated only if the current project state materially changed.
- The final diff contains no unrelated changes or generated artifacts.
