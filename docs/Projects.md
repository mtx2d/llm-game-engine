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

New/load/project-open operations reject unsaved changes unless the request explicitly supplies `"discardChanges": true`. The GUI offers Save and continue, Discard, and Cancel when changing scenes or closing the native window. Save and continue saves the current document's recorded path even when the load-path field contains a different target. An untitled scene must first be saved to a chosen path. Saving/switching requires completed edit groups; project switching requires stopped simulation. Export rejects unfinished or unsaved current documents.

Native close stops play and restores authored content before asking about unsaved changes. Cancelling retains the scene and resumes editing with play stopped. A failed Save leaves the confirmation open and the scene available. Teardown callback errors keep the interactive editor open with an error in its status bar. Automation can use `session.close` after stopping simulation and finishing edits; dirty documents require explicit discard. Successful close ends the protocol after its response and rejects further commands on that session. EOF and a bounded `--frames` run remain explicit noninteractive termination paths; automation must save work it wants to retain.

An editor document retains the exact bytes read at load or last successful Save, bounded to 64 MiB. Save compares those bytes immediately before publication. External modifications, including whitespace edits and changes that preserve size/timestamp, cause an error. Deletion is also an error. Rejection preserves in-memory content, dirty state, document identity and Undo/Redo history. Load with explicit discard accepts the external version; Save As to a new path preserves pending work separately. Save As never replaces an existing destination. Tools intentionally reauthoring an existing scene must load it first, then edit/replace it through the shared command interface. Automation export verifies the active document's file baseline before staging, even when the in-memory scene is clean. Core and editor scene saves both reject serialized output larger than the 64 MiB reload limit.

```json
{"command":"scene.status"}
{"command":"project.get"}
{"command":"scene.save","path":"Scenes/Main.aster"}
{"command":"project.open","path":"/path/to/Other/Project.asterproj"}
{"command":"project.create","path":"/path/to/NewGame","name":"My Game"}
{"command":"scene.load","path":"Scenes/Other.aster","discardChanges":true}
{"command":"session.close"}
```

Project open/create paths identify explicitly requested filesystem locations; scene/component/prefab paths remain inside the active asset root. `project.get` returns null configuration/path in legacy asset-directory mode. C++ `Project::UpdateConfig` validates and persists before changing the instance. Interactive configuration editing remains planned.

Scene/project writes create exclusive sibling temporary files and flush contents before publication. Existing-document saves replace the destination atomically after comparing its contents; new editor destinations use exclusive publication so a competing creator cannot be overwritten. Publication failures before replacement preserve the old destination and remove the owned temporary file. Parent-directory entry durability under power loss is not guaranteed on every filesystem. Scene/project/game-manifest reads bound actual bytes read and reject duplicate keys and excessive nesting.

Editor scene saves acquire a nonblocking OS file lock before comparing/publishing bytes. Saves to the same physical directory share `.aster/Writes.lock`, including paths through directory aliases. Contention returns an error asking the caller to retry; it preserves pending content, identity, history and the disk baseline. Destruction or process termination releases ownership. A lock file remains on disk permanently: its existence does not indicate a live owner, and deleting/replacing it can bypass another editor's lock. POSIX uses [flock](https://man7.org/linux/man-pages/man2/flock.2.html); Windows uses [LockFileEx](https://learn.microsoft.com/en-us/windows/win32/api/fileapi/nf-fileapi-lockfileex). These are cooperative local-filesystem checks; arbitrary third-party writers and network filesystems are outside this serialization guarantee. Project configuration writes do not yet use this protocol.

The directory name `.aster` is reserved, case-insensitively, at every asset-directory depth. Editor storage must be a real directory rather than a file, symlink or junction; newly created POSIX storage has owner-only permissions. Asset references, decoded glTF URIs and resolved aliases into this storage are rejected. The asset browser hides it, exports omit it, and Git ignores it. Move or back up the complete development project when preserving editor state. Never delete lock files while editors are running.

Crash recovery, project configuration conflict protection, a graphical project browser and safe live reloading remain production work; see [ProductionPlan.md](ProductionPlan.md).
