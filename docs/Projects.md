# Aster projects

A `.asterproj` file records the project name, asset directory and startup scene. The repository's `Aster.asterproj` opens its sample assets and feature scene. Configuration belongs to a project instance, without a global active-project singleton.

```json
{
  "Version": 1,
  "Name": "My Game",
  "AssetDirectory": "Assets",
  "StartScene": "Scenes/Main.aster"
}
```

`AssetDirectory` is relative to the project file's directory; `StartScene` is relative to the asset directory. Serialized paths use forward slashes and cannot escape their roots through traversal or symbolic links. Opening validates the startup scene. Unknown fields, duplicate keys, unsupported versions, invalid UTF-8, control characters in names, missing directories/scenes and excessive size/nesting fail with an error. Project names contain 1–128 UTF-8 bytes; project files are limited to 64 KiB.

## Create and launch

```sh
build/debug/AsterEditor --create-project /path/to/NewGame "My Game"
build/debug/AsterEditor --project /path/to/NewGame/Project.asterproj
build/debug/AsterEditor --automation /path/to/NewGame/Project.asterproj
build/debug/AsterRuntime --project /path/to/NewGame/Project.asterproj
build/debug/AsterRuntime --project /path/to/NewGame/Project.asterproj --steps 120
```

Use the executable paths appropriate to the platform/build. Creation requires an absent destination and existing parent. It stages a complete project with `Assets/Scenes/Main.aster` and a primary camera, then publishes the directory. Existing destinations, including empty directories, are rejected. Failed validation publishes nothing and removes staging contents.

Editor and runtime load the startup scene automatically. A relative `--scene Scenes/Other.aster` overrides it within the same asset root. A project can be moved and opened from any working directory. Existing asset-directory and explicit runtime scene workflows remain available. Export uses the project's asset root and produces a standalone package; players do not need a project file.

## Documents and automation

`scene.status` reports scene path, unsaved changes, edit group and play state. Save/load establishes the clean baseline; Undo/Redo can return to or leave it. Simulation does not dirty authored content. Changing documents clears the old document's history.

New/load/project-open operations reject unsaved changes unless the request explicitly supplies `"discardChanges": true`. The GUI offers Save and continue, Discard, and Cancel when changing scenes. Save and continue saves the current document's recorded path even when the load-path field contains a different target. An untitled scene must first be saved to a chosen path. Saving/switching requires completed edit groups; project switching requires stopped simulation. Export rejects unfinished or unsaved current documents.

```json
{"command":"scene.status"}
{"command":"project.get"}
{"command":"scene.save","path":"Scenes/Main.aster"}
{"command":"project.open","path":"/path/to/Other/Project.asterproj"}
{"command":"project.create","path":"/path/to/NewGame","name":"My Game"}
{"command":"scene.load","path":"Scenes/Other.aster","discardChanges":true}
```

Project open/create paths identify explicitly requested filesystem locations; scene/component/prefab paths remain inside the active asset root. `project.get` returns null configuration/path in legacy asset-directory mode. C++ `Project::UpdateConfig` validates and persists before changing the instance. Interactive configuration editing remains planned.

Scene/project writes create exclusive sibling temporary files, flush contents, then atomically replace the destination. Publication failures preserve the old destination and remove the owned temporary file. Parent-directory entry durability under power loss is not guaranteed on every filesystem. Scene/project/game-manifest reads bound actual bytes read and reject duplicate keys and excessive nesting.

Window-close protection, crash recovery, external modification conflicts, a graphical project browser and safe live reloading remain production work; see [ProductionPlan.md](ProductionPlan.md).
