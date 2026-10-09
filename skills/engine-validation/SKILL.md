---
name: engine-validation
description: Validate Aster changes, review commits, and audit engine release requirements against executable evidence.
---

Read `AGENTS.md` and the affected requirement in `GameEngineDoc.md`.

1. Configure and build the relevant CMake preset, with warnings enabled.
2. Run CTest with failure output. Inspect what each test actually asserts.
3. For lifetime/input changes run AddressSanitizer and UndefinedBehaviorSanitizer checks. Include invalid data, stale handles, and callback-driven mutation.
4. For rendering run Vulkan/NVRHI validation, offscreen image checks, and window resize/shutdown checks. For export launch a copied package away from the build tree.
5. Review all changed code for ownership, bounds, portability, errors, style, and test gaps. Record concrete findings and fix them before committing.
6. Update `docs/ImplementationStatus.md` with commands, actual results, and unverified gates. Windows, macOS, and Linux each require their own evidence.

A passing subset does not establish completion of the full engine. Leave the goal active while any specified feature or release gate remains incomplete.
