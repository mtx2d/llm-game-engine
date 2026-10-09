# Aster development instructions

`GameEngineDoc.md` is the product specification. Preserve its complete scope. Work autonomously and ask questions only when a decision requires the user. The engine name is Aster.

## Workspace and dependencies

- Inspect project files only inside this working directory. Do not use other local projects as references.
- Fetch third-party sources from their official upstreams into the build tree. Pin exact revisions or verified release archive hashes. Never commit build trees, credentials, downloaded binaries, or generated caches.
- Build the engine as a C++20 static library, with separate editor and shipping runtime targets. Keep editor dependencies out of the runtime.
- Use GLFW, Vulkan/NVRHI, GLM, miniaudio, and Lua. Track macOS/MoltenVK validation explicitly; build configuration is not proof of platform support.

## Code conventions

- Follow https://docs.hazelengine.com/HazelForEngineers/DeveloperGuide#naming: PascalCase types, namespaces, functions, and source filenames; camelCase locals and parameters; `m_Member` private fields; `s_Static` static variables. Public component fields use PascalCase.
- Use tabs for C++ indentation and Allman braces for type/function/control bodies. Prefer explicit, readable code, RAII, bounded ownership, and descriptive errors.
- Validate serialized and scripting input at boundaries. Reject nonfinite values, stale entities, cycles, duplicate IDs, unsupported versions, and escaping asset paths.
- Keep simulation independent of graphics/window creation. Structural changes during callbacks must not invalidate iteration.
- Do not silently catch errors, substitute dummy implementations for required backends, or label an unverified feature production-ready.

## Quality gates

- Add meaningful unit tests for state transitions and failure paths; add integration tests for subsystem boundaries. A test must assert behavior, not merely successful construction.
- Every public component and scripting API must be exercised by the automated feature scene. Rendering features need GPU output and validation evidence.
- Run relevant CTest checks, warning-clean builds, and sanitizers before claiming completion. Record platform limitations accurately.
- Review all changes before each commit, correct findings, then rerun affected checks. Commit and push to https://github.com/mtx2d/llm-game-engine as requested; never force-push.
- Maintain `docs/ImplementationStatus.md` with evidence and unfinished requirements. Never shrink the specification to fit completed work.

Repository workflows live in `skills/engine-development/SKILL.md` and `skills/engine-validation/SKILL.md`.
