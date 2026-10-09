# Runtime game packages

The command editor exports the supplied platform's shipping runtime and the project's complete asset directory. Packages contain `AsterGame` (`AsterGame.exe` on Windows), `Game.json`, `Assets/`, and `ThirdParty/`. The runtime does not link the editor command or ImGui/ImGuizmo libraries, and no editor executable is copied.

Send this JSON request to `AsterEditor --automation <asset-root>`, replacing the absolute paths:

```json
{"command":"project.export","scene":"Scenes/FeatureGallery.aster","runtime":"/path/to/AsterRuntime","notices":"/path/to/ThirdParty","output":"/path/to/NewGame"}
```

The entry scene is relative to the asset root. The destination must be absent or empty, with an existing parent directory, and outside the asset tree. Export validates the entry scene and every serialized `.aster` scene or JSON prefab containing `Entities`. It checks referenced script/audio files, actually imports referenced meshes and their decoded material textures, and decodes referenced environment HDR images. It additionally checks external glTF/GLB buffer and image URI containment throughout the asset tree. Dynamic Lua paths cannot be enumerated statically, so all asset files are copied; the integration test executes the feature scene's dynamic prefab and audio references.

Malformed referenced mesh accessors and HDR data fail export before publication. Missing references, unsupported scene versions, escaping/cyclic symbolic links, special files, and occupied destinations also fail. Internal links are materialized as regular files. Files are copied to a sibling staging directory and the staged assets are validated again before the completed directory is renamed into place. Failed exports remove staging contents and preserve user files in an occupied destination. Script and audio file existence is checked without executing every possible script or decoding every possible audio source; the game integration test provides separate execution coverage.

Scene JSON persists `Environment` with `Path`, `Intensity`, and `Rotation`. The path stays relative to `Assets/`; rotation is yaw in radians, and an empty path disables environment lighting. The renderer computes or reuses a versioned irradiance/specular/BRDF cache from the packaged HDR image. The Poly Haven attribution document travels with the original HDR asset. Build-tree caches are not required to move a package.

## Launch and validation

The runtime reads `Game.json` beside its executable, independently of the working directory. The manifest contains only `Version`, the relative `Assets` directory, and the relative entry `Scene`. Manifest paths are validated after resolving symlinks.

```sh
# Deterministic headless simulation; optional state output.
./AsterGame --steps 240 --output /path/to/State.aster

# With rendering enabled in the supplied runtime, run until the window closes.
./AsterGame

# Bounded graphical run for integration testing.
./AsterGame --window --steps 120 --validation
```

Explicit `--steps` chooses headless operation unless `--window` is supplied. Graphical builds without `--steps` use the actual window/render loop and device audio. Headless and bounded runs use offline audio. A CPU-only runtime rejects `--window`. Shipping launches do not require development validation layers; `--validation` explicitly enables Vulkan/NVRHI diagnostics and requires an installed Vulkan validation layer. The relocated Linux package passed a five-frame native-window run with development library paths removed and validation layers unavailable, followed by an explicit validation run. Supported-platform release, device-audio, and full gameplay checks remain separate gates.

Run the registered relocation/failure-path test with CTest or directly:

```sh
bash scripts/LocalBuild.sh ctest --test-dir build/debug -R '^Export$' --output-on-failure
python3 Tests/ExportTests.py --editor build/debug/AsterEditor --runtime build/debug/AsterRuntime --assets Assets --notices ThirdParty
```

The test copies the package, removes the original source assets, starts from a different working directory, compares all asset and notice bytes, and asserts Lua lifecycle/API execution, decoded audio control, and Bullet collision settling. It also rejects manifest traversal/symlink escapes, missing script files, invalid referenced glTF accessors and HDR images, occupied output, and invalid external asset links. The current relocated headless package test, including mesh/HDR/input regressions, passed in debug and AddressSanitizer/UndefinedBehaviorSanitizer builds; commands and evidence are recorded in `ImplementationStatus.md`.

## Distribution limits

Export packages the architecture and operating system of the supplied runtime; it does not cross-compile or bundle system dependencies. Linux retains its C/C++ runtime, window-system, and Vulkan-loader/driver requirements. Graphical execution requires a compatible Vulkan implementation; macOS requires a working MoltenVK integration. Device audio needs a supported operating-system audio backend.

On Windows, an adjacent `vulkan-1.dll` supplied with the selected runtime is copied into the package along with its preserved loader license. Directories, symbolic links, empty files, and oversized loader files are rejected. If no companion loader exists, the package uses the system Vulkan loader. The graphics driver and Microsoft C++ runtime remain platform prerequisites; use a Release build for distribution. SwiftShader is used only for CI and is not bundled into games.

Relocated Linux headless, graphical, and keyboard-driven gameplay tests have passed in debug and release. Native Windows/macOS CPU CI also passed relocated headless launches. Windows/macOS graphical packages and native input, device audio, dependency deployment, platform signing, and self-contained macOS application packaging remain unfinished acceptance gates. See `ImplementationStatus.md` for the complete specification and current evidence.
