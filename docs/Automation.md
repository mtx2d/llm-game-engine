# Editor automation protocol

Start `AsterEditor --automation <project-directory>`. Send one JSON object per line on standard input. Each response is one JSON line with `ok`, optional `id` copied from the request, and either `result` or `error`. Diagnostics never share the protocol stream. Commands execute in input order on the editor thread.

The graphical editor uses the same validated command backend for scene edits, hierarchy changes, inspector controls, gizmos, history, and export. The standalone automation process supports authoring and deterministic simulation without creating a window or graphics device.

```json
{"id":1,"command":"entity.create","name":"Player"}
{"id":2,"command":"entity.patch","entity":1,"patch":{"Transform":{"Translation":[0,3,0]}}}
{"id":3,"command":"scene.save","path":"Level.aster"}
```

| Command | Parameters | Behavior |
| --- | --- | --- |
| `help` | none | List commands |
| `scene.get` | none | Return current serialized scene, including simulation state during play |
| `scene.new` | optional `name` | Replace authoring scene with an empty scene |
| `scene.replace` | `scene` | Validate and replace a complete versioned scene document |
| `scene.load` / `scene.save` | `path` | Load/save relative to project root |
| `scene.environment` | `environment` | Merge HDR environment `Path`, nonnegative `Intensity`, and yaw `Rotation` in radians; empty path disables IBL |
| `entity.create` | optional `name` | Create entity and return persistent `entity` ID |
| `entity.destroy` | `entity` | Destroy entity and descendant subtree |
| `entity.parent` | `entity`, optional `parent` | Reparent; omitted/zero parent detaches |
| `entity.patch` | `entity`, `patch` | JSON merge patch of entity fields/components; ID and Parent are protected |
| `prefab.spawn` | `path`, `root`, optional `parent` | Clone prefab subtree with remapped identities |
| `history.undo` / `history.redo` | none | Restore authoring snapshots; retains up to 100 edits |
| `history.begin` / `history.commit` / `history.cancel` | none | Group edits into one undo entry or restore the state before the group; nested groups are rejected |
| `simulation.start` | optional `audio`: `offline`, `device`, `disabled` | Start play using Lua, Bullet, and miniaudio; automation defaults to offline audio |
| `simulation.step` | optional `steps` in 1–10000 | Advance fixed steps, return scene, script errors and log |
| `simulation.stop` | none | Stop play and restore authored scene |
| `input.set` | `input` | Supply a validated gameplay input snapshot during play |
| `project.export` | `scene`, `runtime`, `notices`, `output` | Export saved relative scene with runtime, notices, and project assets; see [Export.md](Export.md) |

All editing commands require stopped simulation. Play and undo/redo require a completed edit group. Failed edits preserve scene contents. A successful new edit clears redo history. File paths cannot escape the project root, including through symbolic links. Asset paths inside scenes use portable relative paths.

Scenes use version 1 with `Name` and `Entities`. Each entity has `ID`, `Name`, `Parent` (ID or null), and `Transform`. Optional component keys are `Camera`, `MeshRenderer`, `Light`, `RigidBody`, `Script`, and `AudioSource`. `scene.get` supplies complete records. `entity.patch` preserves unspecified fields; setting an optional component to null removes it. Adding a component requires all its schema fields, as defined by `Components.h` and the scene serializer. Transforms use XYZ Euler radians and local translation/scale.

`input.set` accepts optional `KeysDown`, `KeysPressed`, and `KeysReleased` arrays of canonical names such as `A`, `Space`, and `Left`; `MouseDown`, `MousePressed`, and `MouseReleased` arrays of eight booleans; `MousePosition`, `MouseDelta`, and `Wheel` pairs; and boolean `Focused`. Omitted fields use empty/default snapshot values. Nonfinite coordinates and unknown fields or names are rejected. Held keys carry into subsequent simulation steps; accumulated edges, motion, and wheel are consumed once by the next fixed step. Lua exposes `key_down`, `key_pressed`, `key_released`, matching mouse queries, `mouse_position`, `mouse_delta`, `mouse_wheel`, and `input_focused`. See [Simulation.h](../Engine/include/Aster/Simulation/Simulation.h) and [InputState.h](../Engine/include/Aster/Input/InputState.h) for the complete API and button names.

```json
{"command":"simulation.start","audio":"offline"}
{"command":"input.set","input":{"KeysDown":["Space"]}}
{"command":"simulation.step","steps":1}
{"command":"input.set","input":{}}
{"command":"simulation.step","steps":1}
{"command":"simulation.stop"}
```
