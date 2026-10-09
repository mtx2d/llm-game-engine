# Lua scripting

Attach a `Script` component with a project-relative Lua path to an entity. The file must return a table. Aster loads one table per scripted entity; store instance state in `self`. Scripts run in the shipping runtime, editor play mode, and headless simulation through the same implementation.

```lua
return {
    OnCreate = function(self, entity)
        self.elapsed = 0
    end,

    OnUpdate = function(self, entity, dt)
        self.elapsed = self.elapsed + dt
        if engine.input_focused() and engine.key_pressed("Space") then
            engine.apply_impulse(entity, 0, 4, 0) -- Requires a Dynamic RigidBody.
        end
    end,

    OnCollision = function(self, entity, other, began)
        if began and engine.exists(other) then
            engine.log("Contact with " .. engine.get_name(other))
        end
    end,

    OnDestroy = function(self, entity)
        -- The entity may already have been destroyed.
        engine.log("Behavior stopped")
    end
}
```

For a complete playable example, see [BlockStack](BlockStack.md). [FeatureGallery.lua](../Assets/Scripts/FeatureGallery.lua) exercises all 32 bindings below, and [FeatureTrigger.lua](../Assets/Scripts/FeatureTrigger.lua) exercises contact transitions.

## Lifecycle and fixed stepping

All callbacks are optional, and their return values are ignored. Callback fields must be present directly in the returned table; `__index` does not supply lifecycle callbacks. A present field that cannot be called produces a script error.

| Callback | Timing |
| --- | --- |
| `OnCreate(self, entity)` | Once after loading an enabled script instance, including instances present when simulation starts. |
| `OnUpdate(self, entity, dt)` | Once per fixed simulation step before physics. `dt` is seconds, default `1 / 60`. |
| `OnCollision(self, entity, other, began)` | After physics for each contact pair entering (`true`) or leaving (`false`). Both regular contacts and triggers participate. This is a transition notification, not a continuous-contact callback. |
| `OnDestroy(self, entity)` | When an instance is removed, disabled, changes path, loses its entity, or simulation stops. An instance already disabled by a script failure does not receive further callbacks, including this one. |

Each step consumes input, synchronizes script instances, runs updates, synchronizes bodies, advances Bullet once, writes dynamic transforms back to the scene, dispatches contact transitions, and synchronizes audio. Removing or replacing a script is noticed at the next script synchronization. The C++ `Simulation::Update(deltaTime)` accumulates wall time; it runs at most eight fixed steps by default and discards excess catch-up time after a long pause. `Simulation::Step()` advances exactly one step.

Entity creation, destruction, hierarchy edits, and component edits take effect immediately in the scene. Callbacks iterate snapshots, so a callback may destroy itself, destroy another entity, remove components, or spawn a prefab. Entities created during a callback begin scripting on the following step. Their first `OnUpdate` can run in that same step after `OnCreate`. Disabling or deleting a script prevents its remaining update/contact callbacks. Changing a script's path reloads its instance; disabling and allowing synchronization before re-enabling also reloads it. Editing file bytes alone does not trigger a reload.

Keep state in `self` when it belongs to one instance. Globals are shared by all scripts in a simulation. Script failures and later restarts do not undo scene edits already made by that script. The editor separately restores its saved authoring state on Stop.

## Entity and component API

All functions are in the global `engine` table. `id` means a positive Lua integer containing a persistent scene entity ID; `nil` is the absence of an entity. Except for `exists`, functions accepting an entity reject stale or unknown IDs. Names and paths are case-sensitive. Functions marked `none` return no values.

| Function | Returns | Behavior |
| --- | --- | --- |
| `engine.create(name)` | `id` | Create an entity with its default Transform. Name is required. |
| `engine.destroy(id)` | none | Destroy the entity and all hierarchy descendants. |
| `engine.exists(id)` | boolean | Whether a valid positive integer ID currently exists. Invalid ID types still raise an error. |
| `engine.find(name)` | `id` or `nil` | Find the first matching entity. Names are not unique; retain IDs when identity matters. |
| `engine.get_name(id)` | string | Read the entity name. |
| `engine.set_name(id, name)` | none | Replace the name; names contain 1–4096 bytes and no NUL. |
| `engine.get_parent(id)` | `id` or `nil` | Read the hierarchy parent. |
| `engine.set_parent(id, parent)` | none | Reparent; omit `parent` or pass `nil` to detach. Reject cycles. Local Transform remains unchanged, so world position may change. |
| `engine.spawn_prefab(path, parent)` | root `id` | Load a scene file with exactly one root, clone its subtree with new IDs, and optionally parent the clone. Omit `parent` or pass `nil` for a root entity. |
| `engine.get_component(id, name)` | table or `nil` | Return a copy of the component, or `nil` when an optional component is absent. Reject an unknown component name. |
| `engine.set_component(id, name, fields)` | none | Add or replace the whole component. Validate a copy before committing it. Omitted fields use the defaults below. |
| `engine.remove_component(id, name)` | none | Remove an optional component; removing an absent optional component is harmless. Transform cannot be removed. |
| `engine.get_position(id)` | `x, y, z` | Read local translation as three numbers. |
| `engine.set_position(id, x, y, z)` | none | Set local translation; preserve rotation and scale. |
| `engine.log(message)` | none | Append a string to the simulation log, surfaced by runtime/automation results. |

Component tables are copies, not live references. Read, modify, and write back to preserve other fields:

```lua
local transform = engine.get_component(entity, "Transform")
transform.Rotation[2] = transform.Rotation[2] + dt
engine.set_component(entity, "Transform", transform)
```

`set_component(entity, "Transform", {Translation = {1, 2, 3}})` instead resets Rotation and Scale to their defaults. Lua component replacement differs from the editor's JSON `entity.patch` merge operation.

## Component schemas

Use the exact component and field names below. Vectors are dense Lua arrays: `{x, y, z}` or `{r, g, b, a}`. Booleans must be booleans; numeric strings are rejected. Every numeric value must be finite and representable as a C++ float. Unknown fields, extra vector fields, incorrect vector lengths, and unknown enum strings raise errors. Fields are read directly, without table metamethods.

| Component | Fields and defaults | Constraints |
| --- | --- | --- |
| `Transform` | `Translation = {0,0,0}`, `Rotation = {0,0,0}`, `Scale = {1,1,1}` | Local translation and scale; Euler rotation in radians, composed Z × Y × X. Absolute scale components must exceed `0.000001`. Physics has stricter scale constraints below. |
| `Camera` | `VerticalFov = 60`, `NearClip = 0.1`, `FarClip = 1000`, `Primary = true` | FOV in degrees, `[1,179]`; near greater than zero; far greater than near. Author one primary gameplay camera for an unambiguous view/listener. |
| `MeshRenderer` | required `Mesh`; `BaseColor = {1,1,1,1}`, `Metallic = 1`, `Roughness = 1`, `Visible = true` | Mesh is a project asset path. Color channels and material factors lie in `[0,1]`; factors multiply imported glTF material values. |
| `Light` | `Type = "Directional"`, `Color = {1,1,1}`, `Intensity = 1`, `Range = 10`, `InnerCone = 20`, `OuterCone = 30`, `CastShadows = true` | Type is `Directional`, `Point`, or `Spot`. Color/intensity are nonnegative; range is positive. Cone angles are degrees in `[0,89]`, inner no greater than outer. |
| `RigidBody` | `Type = "Static"`, `Shape = "Box"`, `HalfExtents = {0.5,0.5,0.5}`, `LinearVelocity = {0,0,0}`, `Radius = 0.5`, `Height = 1`, `Mass = 1`, `Friction = 0.5`, `Restitution = 0`, `IsTrigger = false` | Type is `Static`, `Kinematic`, or `Dynamic`; shape is `Box`, `Sphere`, or `Capsule`. All dimensions are positive even when unused by the selected shape. Mass is nonnegative and strictly positive for Dynamic. Friction is nonnegative, restitution in `[0,1]`. Capsule axis is local Y; Height is the cylindrical part, excluding the two hemispheres. |
| `Script` | required `Path`; `Enabled = true` | Project-relative Lua source file returning a lifecycle table. |
| `AudioSource` | required `Path`; `Volume = 1`, `Pitch = 1`, `Loop = false`, `PlayOnStart = false`, `Spatial = true` | Project-relative audio file; volume nonnegative, pitch positive. |

Asset paths contain 1–4096 bytes, use project-relative locations, and cannot contain parent traversal, absolute roots, colons, or NUL. Asset opening additionally checks canonical containment and rejects escaping symlinks and missing files. Component validation checks path syntax; a file is opened when its subsystem needs it. The persisted scene environment is edited through the editor/automation `scene.environment` command; it is not an entity component or a Lua binding.

## Physics

| Function | Returns | Behavior |
| --- | --- | --- |
| `engine.apply_force(id, x, y, z)` | none | Apply a central world-space force for the next physics step. Reapply every step for sustained acceleration. |
| `engine.apply_impulse(id, x, y, z)` | none | Apply an immediate central world-space impulse; velocity change depends on mass. |
| `engine.get_velocity(id)` | `x, y, z` | Read world-space linear velocity. |
| `engine.set_velocity(id, x, y, z)` | none | Replace world-space linear velocity and wake the body. |

All four functions require a Dynamic RigidBody. Bullet synchronizes pending component edits before these operations. Units are conventionally meters, seconds, and kilograms; default gravity is `{0,-9.81,0}`. Transforms and `LinearVelocity` reflect physics results after each step. Moving a static or kinematic body uses its Transform; setting a dynamic Transform teleports it at synchronization. Changes to collider dimensions, material parameters, body type, mass, or world scale rebuild the body.

Physics rejects sheared transforms and nonpositive world scale. Sphere and capsule colliders require uniform world scale. Dynamic bodies may have ordinary transform parents, but cannot descend from another RigidBody; there is no joint/constraint hierarchy. Triggers generate overlap transitions without contact response. Contact callbacks contain IDs, not contact points or normals. The other ID may already be stale because an earlier callback destroyed it; check `engine.exists(other)` before reading it. Fixed stepping makes scheduling reproducible but is not a claim of bit-identical physics across platforms.

## Audio

| Function | Returns | Behavior |
| --- | --- | --- |
| `engine.play_audio(id)` | none | Rewind the source to frame zero and start it. |
| `engine.stop_audio(id)` | none | Stop the source. |
| `engine.is_audio_playing(id)` | boolean | Read miniaudio's playing state. |

These functions require an AudioSource and enabled audio. Sources decode through miniaudio. `PlayOnStart` starts a source when it is first synchronized, including sources created during play; changing the file path replaces the source. Volume, pitch, looping, spatialization, and world position synchronize during stepping and audio operations. Removing the component or destroying its entity releases the source at synchronization. Spatial audio uses the primary camera's world position and orientation as listener zero; nonspatial sources ignore scene distance.

C++ settings select `Device` for real output, `Offline` for the same graph without an output device, or `Disabled`. Offline audio is stereo float PCM at 48 kHz and advances when C++ `RenderAudio(frameCount)` consumes frames, not simply because physics stepped. Device audio advances on the audio device clock. Automation defaults to offline; normal interactive runtime uses a device. Device availability, audible output, and spatial listening still require platform checks in [ImplementationStatus.md](ImplementationStatus.md).

## Input

| Function | Returns | Meaning |
| --- | --- | --- |
| `engine.key_down(key)` | boolean | Key is held in this fixed step's snapshot. |
| `engine.key_pressed(key)` | boolean | Key received a press since the previous consumed snapshot. |
| `engine.key_released(key)` | boolean | Key received a release since the previous consumed snapshot. |
| `engine.mouse_down(button)` | boolean | Mouse button is held. |
| `engine.mouse_pressed(button)` | boolean | Mouse button received a press. |
| `engine.mouse_released(button)` | boolean | Mouse button received a release. |
| `engine.mouse_position()` | `x, y` | Latest window-content cursor coordinates, origin at the top left. |
| `engine.mouse_delta()` | `x, y` | Accumulated cursor movement since the previous consumed snapshot. |
| `engine.mouse_wheel()` | `x, y` | Accumulated horizontal/vertical scroll units. |
| `engine.input_focused()` | boolean | Whether the game window has input focus. |

Names are case-sensitive. Keys include `A`–`Z`, `0`–`9`, `F1`–`F25`, `Space`, `Escape`, `Enter`, `Tab`, `Backspace`, `Insert`, `Delete`, `Left`, `Right`, `Up`, `Down`, `PageUp`, `PageDown`, `Home`, `End`, `CapsLock`, `ScrollLock`, `NumLock`, `PrintScreen`, `Pause`, `Menu`, and `Left`/`Right` prefixes on `Shift`, `Control`, `Alt`, and `Super`. Punctuation names are `Apostrophe`, `Comma`, `Minus`, `Period`, `Slash`, `Semicolon`, `Equal`, `LeftBracket`, `Backslash`, `RightBracket`, and `GraveAccent`, plus `World1` and `World2`. Keypad names are `Keypad0`–`Keypad9`, `KeypadDecimal`, `KeypadDivide`, `KeypadMultiply`, `KeypadSubtract`, `KeypadAdd`, `KeypadEnter`, and `KeypadEqual`.

Mouse names are `Left`, `Right`, `Middle`, `Back`, `Forward`, `Button6`, `Button7`, and `Button8`, in that order. Unsupported names raise errors.

Input is independent of ImGui and graphics creation. Window events or automation snapshots feed `Simulation::SetInput`. The latest held state wins; press/release edges, cursor deltas, and wheel movement accumulate until a fixed step consumes them. If rendering runs faster than simulation, an intervening press is retained. If one update performs several physics steps, edges appear only in the first step and held states persist. A press followed by release can produce both edge queries as `true` while `down` is `false`; repeated OS key-down events do not create extra presses. Queries outside stepping reflect the last consumed snapshot.

Focus loss releases every held key/button and clears movement/wheel. The first cursor sample and the first sample after a focus change have zero delta. Editor UI capture suppresses gameplay controls while interacting with panels. The [automation protocol](Automation.md) accepts `input.set` snapshots with canonical key sets, eight mouse-button booleans, finite coordinate pairs, and focus. Held-state changes synthesize missing edges. Contradictory edge/held combinations and held controls on an unfocused snapshot are rejected.

## Errors and resource limits

Invalid API arguments become ordinary Lua errors and may be inspected with `pcall`. Component replacement validates before committing, so a rejected replacement preserves the previous component. Missing assets, stale entities, invalid lifecycle returns, and syntax/runtime errors include their source or callback in simulation errors. An uncaught lifecycle error disables that instance; other instances continue. C++ `GetErrors()` exposes accumulated errors, and console `Execute()` throws on failure. Editor status and automation/runtime results surface errors rather than silently ignoring them.

Each protected script load, callback, or console execution receives a shared budget of 1,000,000 Lua instructions, including resumed coroutines. Exhaustion remains sticky through nested `pcall`, `xpcall`, and coroutine error handling. This is a Lua instruction limit, not a wall-clock bound on C functions, physics, or asset loading. Script files and console source are limited to 1 MiB. The aggregate Lua heap, including coroutines, defaults to 64 MiB; C++ `SimulationSettings::LuaMemoryLimitBytes` accepts 1 MiB–1 GiB. Engine-side assets and scene allocations are outside that heap limit. Lua is built with C++ exception unwinding so allocation failure unwinds engine objects safely.

The `io`, `os`, `package`, and `debug` libraries and `dofile`, `loadfile`, and `require` globals are unavailable. `setmetatable` rejects a non-nil `__gc` finalizer because Lua suppresses instruction hooks in garbage-collection finalizers; use `OnDestroy` for engine cleanup. Ordinary metatables remain available. These restrictions and budgets do not isolate mutually untrusted code: project scripts share a Lua state and can modify scene state and shared globals. Use trusted project scripts.

Executable coverage lives in [SimulationTests.cpp](../Tests/SimulationTests.cpp), [ExportTests.py](../Tests/ExportTests.py), and [BlockStackTests.py](../Tests/BlockStackTests.py). Those tests cover callback mutation, stale IDs, invalid component values, real Bullet motion/contacts, decoded miniaudio PCM, input timing, script-budget failures, allocation recovery, and relocated packages. Rendering and native-platform acceptance remain tracked separately in [ImplementationStatus.md](ImplementationStatus.md).
