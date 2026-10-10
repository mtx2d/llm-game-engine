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

Use the executable paths appropriate to the platform/build. Creation requires an absent destination and existing parent. It stages a complete project with `Assets/Scenes/Main.aster` and a primary camera, then publishes the directory with a native exclusive rename. Existing destinations, including empty directories, are rejected. Failed validation publishes nothing and removes staging contents.

Editor and runtime load the startup scene automatically. A relative `--scene Scenes/Other.aster` overrides it within the same asset root. A project can be moved and opened from any working directory. Existing asset-directory and explicit runtime scene workflows remain available. Export uses the project's asset root and produces a standalone package; players do not need a project file.

## Graphical project controls

Choose **Projects** in the toolbar to open a project file, create a project in a new directory, or edit the current project's name, asset directory and startup scene. Enter the complete `.asterproj` filename for Open, or an absent directory and name for Create. Project paths can be outside the currently open project. These controls use the same validated commands as automation and are disabled during play.

**Save configuration** preserves the current scene and its unsaved work when changing the name/startup scene. A different asset root opens its startup scene and offers Save and continue, Discard or Cancel for pending work. Open/Create use the same protection. A rejected open or configuration leaves the current project available with an error in the status bar. Successful root changes reset asset browsing, selection, inspector buffers and recovery discovery; rendering and viewport picking use the new root. Meshes at identical relative filenames in different projects are reloaded from their owning root.

The controls accept typed paths. A file chooser/recent-project launcher and graphical repair of a missing or corrupt startup scene remain planned; the recovery procedure below remains available for damaged projects.

## Documents and automation

`scene.status` reports scene path, unsaved changes, edit group and play state. Save/load establishes the clean baseline; Undo/Redo can return to or leave it. Simulation does not dirty authored content. Changing documents clears the old document's history.

New/load/project-open operations reject unsaved changes unless the request explicitly supplies `"discardChanges": true`. The GUI offers Save and continue, Discard, and Cancel when changing scenes or closing the native window. Save and continue saves the current document's recorded path even when the load-path field contains a different target. An untitled scene must first be saved to a chosen path. Saving/switching requires completed edit groups; project switching requires stopped simulation. Export rejects unfinished or unsaved current documents.

Native close stops play and restores authored content before asking about unsaved changes. Cancelling retains the scene and resumes editing with play stopped. A failed Save leaves the confirmation open and the scene available. Teardown callback errors keep the interactive editor open with an error in its status bar. Automation can use `session.close` after stopping simulation and finishing edits; dirty documents require explicit discard. Successful close ends the protocol after its response and rejects further commands on that session. EOF and a bounded `--frames` run remain explicit noninteractive termination paths; dirty work retains its last recovery checkpoint, which is separate from an explicitly saved scene.

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

Project open/create paths identify explicitly requested filesystem locations; scene/component/prefab paths remain inside the active asset root. `project.get` returns null configuration/path in legacy asset-directory mode. C++ `Project::PreviewConfig` validates a candidate without writing; `UpdateConfig` validates and persists before changing the instance. The Projects window exposes these operations through the shared commands.

`project.configure` accepts the complete versioned `config` object shown above. It requires an open project file, stopped simulation and a finished edit group. Changing the name or startup scene keeps the current authored document, dirty state and Undo/Redo history. A different resolved asset directory replaces the current document with that directory's startup scene, so dirty work requires explicit `discardChanges`. The new document, paths and recovery store are prepared before configuration is saved; rejected validation or persistence preserves the current editor state. Success returns `configured`, `documentChanged` and nullable `warning`. A warning about old recovery cleanup means the configuration and document switch succeeded while the previous checkpoint remains available for explicit cleanup.

```json
{"command":"project.configure","config":{"Version":1,"Name":"My Game","AssetDirectory":"Assets","StartScene":"Scenes/Other.aster"}}
```

Configuration retains the exact bytes read at open or last successful update, with a 64 KiB read/write limit. Updates reject external edits (including whitespace and same-size/same-timestamp changes), deletion, changed physical paths and retargeted asset directories. Rejection preserves configuration, document and history. Reopen the project explicitly to accept the external configuration; save pending scene work separately before discarding it. Export checks both the current scene and project-file baselines before staging.

Scene/project writes create exclusive sibling temporary files and flush contents before publication. Existing-document saves replace the destination atomically after comparing its contents; new editor destinations use exclusive publication so a competing creator cannot be overwritten. Publication failures before replacement preserve the old destination and remove the owned temporary file. Parent-directory entry durability under power loss is not guaranteed on every filesystem. Scene/project/game-manifest reads bound actual bytes read and reject duplicate keys and excessive nesting.

Editor scene saves acquire a nonblocking OS file lock before comparing/publishing bytes. Saves to the same physical directory share `.aster/Writes.lock`, including paths through directory aliases. Contention returns an error asking the caller to retry; it preserves pending content, identity, history and the disk baseline. Destruction or process termination releases ownership. A lock file remains on disk permanently: its existence does not indicate a live owner, and deleting/replacing it can bypass another editor's lock. POSIX uses [flock](https://man7.org/linux/man-pages/man2/flock.2.html); Windows uses [LockFileEx](https://learn.microsoft.com/en-us/windows/win32/api/fileapi/nf-fileapi-lockfileex). These are cooperative local-filesystem checks; arbitrary third-party writers and network filesystems are outside this serialization guarantee. Project configuration updates use a separate stable `.aster/ProjectWrites.lock` in the physical project directory, held across exact-byte comparison, publication and baseline update. Configuration contention preserves state and asks the caller to retry.

The directory name `.aster` is reserved, case-insensitively, at every asset-directory depth. Editor storage must be a real directory rather than a file, symlink or junction; newly created POSIX storage has owner-only permissions. Asset references, decoded glTF URIs and resolved aliases into this storage are rejected. The asset browser hides it, exports omit it, and Git ignores it. Move or back up the complete development project when preserving editor state. Never delete lock files while editors are running.

## Crash recovery

The editor automatically checkpoints completed authored changes under the active asset directory's `.aster/Recovery`. GUI updates are throttled to five seconds, so a crash can lose changes since the last successful checkpoint. An unfinished inspector/gizmo edit is deferred until completed; play checkpoints use the pre-play authoring scene. Automation persists pending completed edits before each command response. Errors remain visible in the GUI status bar or the response's `warnings`; edits remain available and persistence can be retried. Checkpoints never silently save source scenes.

After a crash, the GUI offers **Recover scenes**. Select a session and choose **Recover copy**, **Discard checkpoint**, or **Later**. The toolbar's **Recover** button reopens the list. Live sessions are disabled. Recovering a scene uses the normal Save/Discard/Cancel protection for current unsaved work and clears its history. The adopted work gets its own checkpoint before replacing the in-memory scene. The source checkpoint stays until explicitly discarded, allowing recovery to be retried. Later preserves all work; successful save/clean Undo, scene/project replacement or explicit close clears only the current editor's owned checkpoint.

If the original scene's exact bytes still match, recovery retains its filename and the ordinary conditional Save protection. If the original changed, disappeared or cannot be read, recovery produces an untitled copy and reports why. Choose a new filename; Save As never overwrites an existing destination. A damaged/deleted project startup scene can prevent opening its `.asterproj`; recovery remains accessible by starting automation with its existing **asset directory** instead:

```sh
build/debug/AsterEditor --automation /path/to/Game/Assets
```

```json
{"command":"recovery.list"}
{"command":"recovery.restore","session":"<id returned by recovery.list>"}
{"command":"scene.save","path":"Scenes/Recovered.aster"}
{"command":"recovery.discard","session":"<original session id>"}
{"command":"session.close"}
```

This preserves the damaged original and project configuration for explicit repair. A graphical project repair/start screen remains planned.

Each lazy session owns a native OS lock for its lifetime; a short catalog lock serializes publication, discovery and cleanup. Process death releases ownership without relying on PID files. Checkpoints use alternating scene slots and publish their manifest last through flushed atomic file writes. The previous published slot survives an interrupted next publication. SHA-256 checks reject corrupt scene bytes before restore, and strict versioned manifests reject duplicate keys, escaping paths, unknown fields and invalid source identities. This protects process-crash recovery; it does not promise directory-entry durability after power loss on every filesystem.

Default limits are 32 sessions, 512 MiB of total session files, 64 MiB per scene, 64 KiB per manifest and eight known files per session. A publication that would exceed a limit is rejected while retaining the previous checkpoint. Recovery never evicts published work automatically: explicitly discard unneeded inactive sessions. Malformed/oversized regular checkpoint data can be explicitly discarded; linked files, unknown files, and linked directories are preserved and reported for manual inspection. Storage is synchronous; large-scene checkpoint latency and asynchronous persistence remain part of production performance work. Checkpoints are editor metadata and are excluded from asset imports, exports and Git.

A project launcher/file chooser, graphical startup repair and safe live reloading remain production work; see [ProductionPlan.md](ProductionPlan.md).
