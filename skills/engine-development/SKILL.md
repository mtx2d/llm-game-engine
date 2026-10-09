---
name: engine-development
description: Implement or extend Aster engine systems, editor commands, components, and scripting APIs.
---

Read `AGENTS.md`, `GameEngineDoc.md`, and `docs/ImplementationStatus.md`. Identify the relevant acceptance gate before editing. Keep subsystem ownership explicit and coordinate shared interface changes.

1. Define observable behavior, validation rules, lifetime, and failure semantics.
2. Implement the actual specified backend; preserve headless testability of core behavior.
3. Route editor and automation changes through a shared validated command interface.
4. Add feature-scene coverage for each new component or script binding, including destruction and reload where relevant.
5. Run the validation workflow, review the diff, correct findings, and update implementation evidence.
6. Commit reviewed changes and push without rewriting remote history, as authorized by the product specification.

Do not claim platform or release readiness from compilation alone. Track unfinished requirements explicitly.
