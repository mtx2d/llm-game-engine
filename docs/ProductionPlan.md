# Aster production development

The production-engine goal requested on 2026-10-10 is active. `GameEngineDoc.md` remains required in full. Previous passing feature tests establish a working baseline; they do not establish production readiness for larger games, sustained development or shipping support. A completed milestone does not close the overall goal.

## Hazel reference

The public [TheCherno/Hazel](https://github.com/TheCherno/Hazel) repository was inspected at revision **`1feb70572fa87fa1c4ba784a2cfeada5b4a500db`**. Its reference checkout is under the ignored `build/reference/Hazel/` directory. No Hazel implementation code was copied. Hazel's README describes an early-stage engine and distinguishes its more advanced private development version. Only publicly inspectable behavior is used here.

| Inspected reference | Finding and Aster work |
| --- | --- |
| [Project and serializer](https://github.com/TheCherno/Hazel/tree/1feb70572fa87fa1c4ba784a2cfeada5b4a500db/Hazel/src/Hazel/Project) | Project configuration separates project directory, asset directory and startup scene. Add an owned, validated project model and connect editor, automation, runtime and export. |
| [Application](https://github.com/TheCherno/Hazel/blob/1feb70572fa87fa1c4ba784a2cfeada5b4a500db/Hazel/src/Hazel/Core/Application.h) and [editor panels](https://github.com/TheCherno/Hazel/tree/1feb70572fa87fa1c4ba784a2cfeada5b4a500db/Hazelnut/src/Panels) | Explicit application, layer and panel lifetimes provide useful separation. Separate Aster document/session ownership, platform lifecycle and editor panels while preserving graphics-independent simulation. |
| [Logging](https://github.com/TheCherno/Hazel/blob/1feb70572fa87fa1c4ba784a2cfeada5b4a500db/Hazel/src/Hazel/Core/Log.h) and [instrumentation](https://github.com/TheCherno/Hazel/blob/1feb70572fa87fa1c4ba784a2cfeada5b4a500db/Hazel/src/Hazel/Debug/Instrumentor.h) | Logging and timing are engine facilities. Add persistent structured diagnostics, CPU/GPU timing, resource counters and an editor console/profiler. |
| [Script engine](https://github.com/TheCherno/Hazel/blob/1feb70572fa87fa1c4ba784a2cfeada5b4a500db/Hazel/src/Hazel/Scripting/ScriptEngine.cpp) | File changes schedule assembly reload through a main-thread queue. Add bounded jobs, main-thread dispatch, dependency tracking and safe reload using Aster's required Lua backend. |
| [Scene](https://github.com/TheCherno/Hazel/tree/1feb70572fa87fa1c4ba784a2cfeada5b4a500db/Hazel/src/Hazel/Scene), [editor](https://github.com/TheCherno/Hazel/blob/1feb70572fa87fa1c4ba784a2cfeada5b4a500db/Hazelnut/src/EditorLayer.cpp) and [renderer](https://github.com/TheCherno/Hazel/tree/1feb70572fa87fa1c4ba784a2cfeada5b4a500db/Hazel/src/Hazel/Renderer) | Distinct edit/play states, scene copying and batching inform scalable scenes/history, resource invalidation and GPU scheduling. Aster retains Vulkan/NVRHI. |

Hazel informs design; it is not a release certification or a substitute for Aster's three-platform requirements.

## Production requirements and acceptance evidence

The target remains a straightforward desktop 3D engine for Windows, macOS/MoltenVK and Ubuntu 24+, with an editor and separate distributable runtime. The table covers the complete production upgrade. No row is complete merely because an API exists or a test constructs it.

| Milestone | Required behavior | Evidence needed | Current state |
| --- | --- | --- | --- |
| Projects and documents | Portable project creation/open/configuration and startup scenes; dirty-document protection during scene/project replacement and close; crash recovery; external-edit conflict protection; correct save/Undo semantics and isolated document histories. | Real GUI/automation workflows, failed save/switch assertions, crash/recovery/conflict tests, relocated projects and exports on all three platforms. | Project files, creation/opening, startup resolution, dirty tracking, scene replacement/native-close confirmation and exact-byte external-change detection implemented. Recovery, cooperative ownership/locking for simultaneous writers, graphical project management and new native evidence remain open. |
| Asset pipeline | Stable identities surviving moves/renames; typed metadata and dependency graph; asynchronous import; bounded CPU/GPU caches; safe reload/cancellation; reproducible versioned cooking and packages with integrity checks. | Reference repair, dependency reimport, cancellation/shutdown, corrupt/stale cache rejection, reproducible packages and runtime loading without source assets. | Static glTF/HDR importer and whole-tree export are a baseline. Registry, asynchronous pipeline, cooking and reload remain open. |
| Core and observability | Explicit startup/shutdown ownership; bounded workers and main-thread dispatch; persistent categorized logs, console, traces and resource statistics; errors that preserve authored work. | Startup/reload/shutdown failure injection, concurrent queue tests, valid trace/log outputs and bounded memory stress. | Existing RAII/error propagation remain. Shared application/session facilities, logging, jobs and profiling remain open. |
| Scalable scenes and authoring | Efficient component queries and hierarchy/world-transform caching; mutation-safe iteration; memory-bounded incremental history; modular panels/layouts, search/multiselect, duplication, prefab editing/overrides. | Large-scene CPU/memory measurements, hierarchy/callback mutation tests, large-scene GUI/history use and prefab round trips through GUI/automation. | Generational handles and strict serialization exist. Whole-scene snapshots and repeated traversal need replacement and measurement. |
| Runtime and scripting | Scene transitions, pause/resume/step, timing configuration and input actions; authored script properties/reload; reusable game UI/text, animation and particle workflows for ordinary 3D games. | Feature-scene coverage for every new component/binding, reload/transition failure tests and a multi-scene sample authored with engine tools and played from a relocated package. | Existing Lua lifecycle/input and single-scene runtime work. Production gameplay/content workflows remain open. |
| Rendering and resources | Multiple frames in flight and correct retirement; culling, batching/instancing, bounded streaming/invalidation; quality presets, GPU timings and scalable shadows; retained PBR/IBL/SSAO/HDR correctness. | GPU validation/reviewed images, resource stress, sustained frame-time/memory measurements, authored/runtime output on native platforms and representative physical GPUs. | Current renderer waits synchronously and caches meshes by path. Existing image tests remain required; production scheduling/resource/performance work remains open. |
| Physics and audio | Collision layers, queries, swept motion/CCD, interpolation and dependable contact lifecycle; bounded audio voices/resources, listener/output lifecycle and recovery. | Query/filter/contact assertions, high-speed/variable-frame tests, device disappearance/recreation and physical output checks. | Bullet/miniaudio integration exists. Production controls/budgets/device lifecycle remain open. Original Windows/Ubuntu physical audio checks remain open; the user's macOS pass is retained. |
| Releases and support | Reproducible optimized builds/cooks, format migrations, documented prerequisites, installable developer workflow, release artifacts/notices, sustained load/unload/play/resize tests and performance/memory regressions. | Native Release CI, sanitizer/fuzz/soak results, clean-machine relocated packages, build provenance and supported hardware/backend matrix. | Native CI and relocated exports are a baseline. New features need native evidence; release engineering, soak/performance and wider physical hardware validation remain open. |

Each milestone must work across subsystem boundaries and under rejected input, reload, cancellation, shutdown and sustained use. Performance budgets will be recorded against measured representative scenes and machines; a fixed entity/light cap or green small fixture is not a scalability result.

## Implementation sequence

1. Establish project/document ownership and persistence, then complete recovery/conflict/close workflows.
2. Add diagnostics, profiling and bounded jobs; measure scene, import and frame costs before changing data layout or scheduling.
3. Build the asset registry and dependency-aware import/cook/reload and resource lifecycle.
4. Replace costly scene/history paths, split panels and implement runtime/content workflows and the multi-scene sample.
5. Upgrade rendering, physics and audio while retaining original image/input/script/export gates.
6. Complete the native release, robustness, performance and sustained-use matrix before declaring readiness.

Executable evidence belongs in `ImplementationStatus.md`. This ordering does not replace or narrow the requirement table.
