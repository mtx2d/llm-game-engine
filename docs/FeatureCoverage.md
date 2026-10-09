# Feature-scene coverage audit

This is a source audit dated 2026-10-09. [GameEngineDoc.md](../GameEngineDoc.md) remains the complete specification; this map does not reduce its scope or replace the execution and platform evidence in [ImplementationStatus.md](ImplementationStatus.md). A fixture being present, a function being called, and a behavior being asserted are different levels of evidence.

[FeatureGallery.aster](../Assets/Scenes/FeatureGallery.aster) contains all seven public component types and every rendering capability explicitly named in the specification. [FeatureGallery.lua](../Assets/Scripts/FeatureGallery.lua) invokes all 32 registered `engine` bindings. It is not, by itself, a behavioral assertion for every supported variant: GPU comparisons, input transitions, audio samples, malformed input, and editing gestures are tested by the companion fixtures below.

## Fixtures and execution paths

| Fixture or test | What it actually checks |
| --- | --- |
| `FeatureGallery` CTest | Loads the checked-in Gallery in the shipping runtime for 120 headless simulation steps; requires the all-32-bindings marker and successful runtime exit. It does not create a renderer. |
| `Simulation::TestFeatureGallery` in [SimulationTests.cpp](../Tests/SimulationTests.cpp) | Runs the Gallery Lua script on an isolated entity with a two-node prefab fixture; asserts no script errors, no leaked probe/prefab entities, transform animation, and `OnDestroy`. This is not the complete checked-in Gallery scene. |
| `Export` in [ExportTests.py](../Tests/ExportTests.py) | Exports the checked-in Gallery, relocates it, removes source assets, and runs 240 steps. Asserts all bindings executed, sphere settling, scripted rotation, kinematic motion, capsule-trigger enter/exit/destruction, and all light/body/shape variants. Also tests files, dependencies, notices, and invalid packages. |
| `GraphicalExport` / native graphical export workflow | Runs the relocated Gallery through a native window with shipping environment paths removed and then with explicit validation. This checks working graphics/package integration; it does not compare individual rendering effects. |
| `EditorRender` in [EditorRenderTests.py](../Tests/EditorRenderTests.py) | Uses the Gallery for authored/play screenshots, populated viewport pixels, visible hierarchy text, source-scene preservation, and a failing teardown callback after rendering. Per-effect accuracy is covered separately. |
| `Renderer` in [RendererTests.cpp](../Tests/RendererTests.cpp) | Creates a real scene called `GPU feature checks`, imports checked-in and generated glTF assets, and changes lights, materials, environment, transforms, and options between GPU readbacks. Assertions isolate individual rendering effects; five motion states are compared with fresh renderer instances. |
| `AssetCorpus` in [AssetCorpusTests.cpp](../Tests/AssetCorpusTests.cpp) | Uses five pinned independent Khronos GLBs: Lantern, BoomBox, AlphaBlendModeTest, NormalTangentTest, TextureCoordinateTest. Asserts asset counts, texture dimensions, visible pixels and parent-rotation changes, and saves images. Enabled when the verified corpus is downloaded; this is not full glTF conformance certification. |
| `AudioValidation` scene and `Simulation::TestAudioValidationScene` | [Scene](../Assets/Scenes/AudioValidation.aster) and [Lua](../Assets/Scripts/AudioValidation.lua) cycle left, center, right, far and silent stages. The test measures real offline stereo PCM, timing, attenuation, silence, repeat and restart. Debug, release and ASan/UBSan checks passed; device listening remains separate. |
| `EditorGui`, `BlockStack`, `RuntimeInput` | [GuiTests.py](../Tests/GuiTests.py) uses the Gallery for real editing gestures. [BlockStackTests.py](../Tests/BlockStackTests.py) drives authored game scenes through commands. Native runtime-input tests export the line-clear game and require keyboard-caused victory, changed presented pixels and normal window closure. Platform execution status is recorded separately. |

## Public components and scene data

Fields below follow [Components.h](../Engine/include/Aster/Scene/Components.h). `MakeComponentScene` and `TestSerializationAndInput` in [SceneTests.cpp](../Tests/SceneTests.cpp) fill every public component field with explicit values and assert a complete JSON round trip; invalid fields and failed-load atomicity are separate assertions. This verifies preservation, not every possible runtime parameter combination.

| Component and all public fields | Gallery use | Behavioral assertions beyond field preservation |
| --- | --- | --- |
| `Transform`: Translation, Rotation, Scale | Mesh placement/scaling, animated Gallery rotation, moving kinematic platform; Lua sets/gets all fields. | Scene hierarchy composition, radians, cycle rejection and prefab transforms; Simulation checks animation; Export checks final positions; Renderer checks visibility/movement/mirroring; EditorGui checks move/rotate/scale and exact single Undo. |
| `Camera`: VerticalFov, NearClip, FarClip, Primary | Primary camera views the Gallery; probe sets a nonprimary camera and checks FarClip. | Renderer requires a primary camera or explicit editor override, rejects invalid matrices and renders through both paths; EditorRender distinguishes authoring and play views. Individual projection bounds are validated in Scene tests. |
| `MeshRenderer`: Mesh, BaseColor, Metallic, Roughness, Visible | Floor, torus, sphere and cube glTF meshes; probe sets all fields. | Renderer compares imported textured output, visibility, transforms, PBR factors and asset reload; corpus checks independent real models. |
| `Light`: Type, Color, Intensity, Range, InnerCone, OuterCone, CastShadows | Directional, point and spot lights; the Sun casts shadows, point and spot do not. Probe sets all fields. | Export asserts all light types exist; Renderer isolates directional enablement, point range, spot cone and shadow behavior for all three types. |
| `RigidBody`: Type, Shape, HalfExtents, LinearVelocity, Radius, Height, Mass, Friction, Restitution, IsTrigger | Static box floor, dynamic sphere/box, kinematic box, static capsule trigger; probe sets all fields. | Export checks settled sphere, moving kinematic body, trigger lifecycle and all type/shape variants. Simulation checks fixed-step accumulation, deterministic motion, mass-dependent force/impulse, kinematic lifting, contacts, rejection and mutation safety. Friction/restitution are preserved and supplied to Bullet; the Gallery does not isolate coefficient-response curves. |
| `Script`: Path, Enabled | Enabled Gallery and trigger scripts; disabled probe script. | Simulation checks create/update/collision/destroy, disabled/replaced scripts, self-removal and callbacks that create/destroy entities; Protocol and EditorRender check teardown-error reporting. |
| `AudioSource`: Path, Volume, Pitch, Loop, PlayOnStart, Spatial | Persistent spatial source has `PlayOnStart=false`. Probe uses a nonspatial source with modified pitch, starts it, checks playing, stops it and checks stopped. | Existing `TestAudio` asserts decoded finite PCM, stop/mute silence, looping and stale-source rejection. New AudioValidation covers PlayOnStart and spatial channel/distance behavior. The Gallery's immediate start/stop does not itself assert mixed samples or physical output; pitch is set/read but has no isolated frequency-ratio assertion. |
| Scene environment: Path, Intensity, Rotation | Persisted `Environment/StudioSmall09.hdr`, intensity 1, yaw 0. This is scene data, not an eighth component. | Scene tests preserve/validate it; Environment tests decode and convolve the actual HDR; Renderer compares sky yaw/intensity and IBL; Export rejects malformed HDR and retains the dependency. |

## All 32 Lua bindings

The list is matched against `RegisterBindings` in [Simulation.cpp](../Engine/src/Simulation/Simulation.cpp). Every row is invoked by the Gallery script. `TestLuaBoundaries` separately rejects stale IDs, invalid types/numbers/components, hierarchy cycles and escaping paths. Input rows use `TestInputEvents` for value and transition assertions because Gallery calls alone mostly check result types.

| Binding | Gallery assertion or action | Additional behavior evidence where needed |
| --- | --- | --- |
| `create` | Creates API Probe. | Probe exists and is later removed; callback mutation tests create children safely. |
| `destroy` | Destroys prefab and probe; asserts absence. | Subtree lifetime tests and callback mutation tests. |
| `exists` | Checks live Gallery and destroyed objects. | Stale-handle rejection and collision/destruction callbacks. |
| `find` | Finds API Probe by name. | Mutation and game tests check name-based lookup after structural changes. |
| `get_name` | Reads renamed probe. | Returned value equals `Renamed Probe`. |
| `set_name` | Renames probe. | Readback equality; BlockStack publishes asserted state through names. |
| `get_parent` | Reads assigned parent and detached nil. | Prefab/hierarchy tests check descendant remapping. |
| `set_parent` | Attaches then detaches probe. | Cycle/cross-scene rejection and world-transform composition. |
| `spawn_prefab` | Spawns Feature prefab under Gallery and checks parent. | Isolated Gallery test uses a child prefab and checks no leaks; Scene tests assert distinct remapped IDs; BlockStack asserts spawned cell geometry. |
| `get_component` | Reads fields from every component and confirms removals. | All-field serialization and invalid-component tests. |
| `set_component` | Sets every field of all seven component types. | Validation failures preserve prior state; mutation tests rebuild/remove simulation objects safely. |
| `remove_component` | Removes all six optional components and checks nil. | Self-removal/destruction lifecycle; removing mandatory Transform is rejected. |
| `get_position` | Reads exactly `(1,2,3)` after setting. | Physics/export assert subsequent world movement. |
| `set_position` | Sets probe position, animates platform and moves trigger. | Export checks motion and trigger exit. |
| `apply_force` | Calls force on probe; interactive Gallery can force the sphere. | Probe is removed before integration, so Gallery proves invocation only. `TestForceAndKinematicBody` checks `F/m * dt` velocity and one-step force clearing. |
| `apply_impulse` | Adds impulse to mass-2 probe. | Subsequent velocity assertion checks the expected increment. |
| `get_velocity` | Reads probe X velocity equal to 2. | Force, impulse and Bullet motion tests. |
| `set_velocity` | Initializes probe X velocity to 1. | Combined impulse assertion; Simulation checks resulting movement. |
| `play_audio` | Starts decoded probe sound. | Playing-state assertion plus `TestAudio` PCM. |
| `stop_audio` | Stops probe sound. | Stopped-state assertion plus `TestAudio` silent samples. |
| `is_audio_playing` | Checks true after start and false after stop. | Loop-at-EOF and stale-source tests. |
| `log` | Emits coverage, collision and destruction messages. | Simulation/Export assert exact expected lifecycle markers. |
| `key_down` | Queries Space type; interactive arrows affect sphere. | InputProbe asserts held W, clearing on focus loss and fixed-step behavior; BlockStack asserts held movement. |
| `key_pressed` | Queries Space type; interactive Space impulses sphere. | InputProbe asserts quick tap and one-shot edges; native gameplay asserts a keyboard-caused hard drop. |
| `key_released` | Queries Space result type. | InputProbe asserts quick-tap release, edge consumption and focus-loss release. |
| `mouse_down` | Queries Left result type. | InputProbe asserts held Right and focus-loss clearing. |
| `mouse_pressed` | Queries Left result type. | InputProbe asserts quick-tap press and one-step consumption. |
| `mouse_released` | Queries Left result type. | InputProbe asserts quick-tap and focus-loss release. |
| `mouse_position` | Checks two numeric results. | InputProbe requires exactly `(108,199)`. |
| `mouse_delta` | Checks two numeric results. | InputProbe requires accumulated `(8,-1)`, then zero in the next step. |
| `mouse_wheel` | Checks two numeric results. | InputProbe requires accumulated `(3,1)`, then zero in the next step. |
| `input_focused` | Checks Boolean result; gates interactive sphere control. | InputProbe asserts focus loss; BlockStack asserts gameplay pause while unfocused. |

`OnCreate`, `OnUpdate`, `OnCollision`, and `OnDestroy` are lifecycle callbacks, not additional bindings. The Gallery, trigger script, and companion mutation/error fixtures exercise all four. Heap limits, instruction budgets, close/finalizer behavior and independent-script survival belong to robustness tests, not new game features.

## Required rendering and authoring capabilities

| Specification capability | Present in checked-in Gallery | Automated effect-specific assertion |
| --- | --- | --- |
| Import glTF meshes with materials and textures | Four model files are referenced. `Floor.gltf` has a base-color image; Cube/Sphere/Torus have PBR factors. | Assets checks glTF and GLB, external/embedded/data images, decoded geometry/material pixels and transforms. Renderer checks textured geometry changes pixels; corpus adds five independent assets. |
| PBR workflow | Imported metallic/roughness materials plus all three light types. | Renderer isolates base/emissive sRGB, linear metallic/roughness/occlusion channels, factors and normals. Analytic or matched-factor comparisons assert their effects. |
| IBL with Poly Haven HDRIs | Actual attributed CC0 Studio Small 09 HDR. | Environment asserts `E = pi * radiance`, specular roughness convolution, BRDF values and cache behavior. Renderer checks IBL-only lighting, sky radiance, intensity/yaw and nonconstant-environment roughness response. |
| Soft shadow maps | Shadow-casting directional Sun and receiving scene geometry; default PCF softness is nonzero. | Generated receiver/occluder scenes compare enabled/disabled shadows, `CastShadows`, softness, receiver acne, moving lights/casters and fresh-renderer references for directional/point/spot lights. |
| Good SSAO | Default RenderSettings enable SSAO for Gallery rendering. | Generated receiver/occluder scenes assert ambient-only attenuation, unchanged isolated plane/direct-only image, motion, viewport edge and removal of offscreen occlusion. These establish behavior; visual quality acceptance on representative scenes remains separately recorded. |
| HDR pipeline and tonemapping | Gallery uses the same HDR scene pass, HDRI and tonemapper. | Renderer compares known emissive radiance at two exposures against analytic ACES-style tonemap/sRGB values and checks HDR resize. |
| Editor positioning gizmos | Gallery is the native editor's default scene. | EditorGui performs real move/rotate/scale gestures, exact single Undo, Escape cancellation, modal isolation, transformed mesh picking and hidden-mesh exclusion. Platform coverage is explicit in ImplementationStatus. |
| Scriptable component-authored 3D physics | All supported body types/shapes plus scripted forces/kinematic movement and triggers. | Gallery export and Simulation numerical/callback assertions described above. |
| Entity/component update, creation/destruction and prefab spawn | Gallery scripts and probe do all of these. | Gallery script assertions, structural mutation tests and complete command-authored BlockStack gameplay. |
| Full agent control and distributable game export | Gallery can be loaded, patched, saved, played and exported through the shared command interface. | Commands/Protocol validate transactions, history and failures; BlockStack is authored using those commands and exported/relocated; native gameplay tests check the shipped runtime independently of the editor. |

## Exact scope of remaining coverage

No rendering capability named in the specification is absent from the checked-in Gallery. Its principal limitation is assertion granularity: the headless Gallery test and broad editor screenshot checks do not individually prove shadows, SSAO, material channels or HDR accuracy. The generated GPU feature scene provides those comparisons. Both fixtures must remain in the acceptance suite; a passing headless Gallery alone is insufficient evidence for the specification's every-feature testing requirement.

The Gallery does not display every currently supported material/light variant. Normal, metallic/roughness, emissive and occlusion textures; alpha mask/blend; double-sided surfaces; UV1; texture transforms; mip selection; and point/spot shadows are asserted in Renderer tests and independent corpus assets. `Triangle.gltf`, which contains all five core material texture slots, is an importer/renderer fixture and is not referenced by Gallery. These are detailed variants of the requested material/import/shadow capabilities. Adding visible Gallery exhibits for them would consolidate the demonstration; their absence does not mean the engine lacks the named capabilities or that the variants have no tests.

The audio and input distinctions are substantive: Gallery audio control calls do not measure spatial sound, and most Gallery input queries only assert types. The companion PCM and InputProbe fixtures supply behavioral assertions. Actual device output/listening, native editor/input workflows and unverified platform results must retain their own status; source inspection cannot close them. Pitch-frequency response and isolated friction/restitution coefficient-response tests are useful specific coverage improvements; field round trips are not numerical evidence for those effects.

There is no untested registered Lua binding or absent public component in Gallery. The coverage claim is nevertheless bounded to the documented supported behavior. Animation, skinning, morph targets, compressed mesh extensions, transmission/refraction, cascaded shadows, order-independent transparency and full glTF certification are not separately requested features in GameEngineDoc. Keep unsupported modes and quality limitations explicit rather than treating those extensions as necessary to make this audit pass.

This audit does not certify production readiness. Continue to use the complete specification, platform execution evidence, validation diagnostics, sanitizer results, representative visual/audio assessment and reviewed failure behavior as the acceptance criteria.
