# Aster

A native C++20 3D game engine under development from [GameEngineDoc.md](GameEngineDoc.md). The complete release requirements and evidence are tracked in [ImplementationStatus.md](docs/ImplementationStatus.md). This repository is not yet a finished engine release.

The implementation includes a scene library, Lua/Bullet/miniaudio simulation, a native graphical editor with transform gizmos, a JSON command interface, and a separate shipping runtime. The GLFW/NVRHI Vulkan renderer imports glTF materials and textures, renders PBR lighting into HDR, processes Poly Haven HDRIs for image-based lighting, and adds soft shadow maps and SSAO. These rendering features have automated GPU image checks. Game export produces a relocatable asset package and runtime. Remaining quality, platform, and release gates are tracked in the implementation status.

## Build

Use CMake 3.24+, Ninja, a C++20 compiler, and Vulkan development tools. On Ubuntu 24+, install CMake, Ninja, Vulkan headers/loader/validation layers, glslang, and X11 development headers. `scripts/BootstrapUbuntu.sh` alternatively downloads an isolated amd64 toolchain under `.tools/` without sudo.

```sh
cmake --preset debug
cmake --build --preset debug --parallel 4
ctest --preset debug
```

For the isolated toolchain, prefix each command with `bash scripts/LocalBuild.sh`. The first configuration downloads pinned, SHA-256 checked upstream dependency archives. Build directories are ignored by git. `release` and `sanitize` presets are also available; the sanitizer preset tests CPU systems without loading GPU driver code.

```sh
build/debug/AsterEditor --project Assets
build/debug/AsterEditor --automation Assets
build/debug/AsterRuntime --scene Assets/Scenes/FeatureGallery.aster --project Assets
build/debug/AsterRuntime --scene Assets/Scenes/FeatureGallery.aster --project Assets --steps 120
```

The graphical editor opens the feature scene. Use the hierarchy and inspector to author entities, click visible meshes in the viewport to select them, double-click assets to attach them, and use the gizmo to move, rotate, or scale. Each drag takes one Undo; Escape cancels it. Right mouse plus WASD flies the editor camera; Q/E changes height. Play starts simulation; Stop restores the authored scene. Interactive play uses a native audio device and reports initialization failures; `--audio offline` selects deterministic audio for editor testing.

[BlockStack](docs/BlockStack.md) is a playable falling-block game authored through the same JSON commands available to AI agents. Open `Games/BlockStack/BlockStack.aster` and press Play, or pass that scene to the runtime. The example includes gameplay, prefab spawning, input, audio, and relocated export tests.

`Scenes/AudioValidation.aster` cycles a tone through left, center, right, far, and silent stages, two seconds each. Its offline PCM test checks channel balance, attenuation, silence and repeat. To exercise an actual output device, run `build/debug/AsterRuntime --scene Assets/Scenes/AudioValidation.aster --project Assets` without `--steps`, using stereo speakers or headphones. Physical listening results remain unverified. [FeatureCoverage.md](docs/FeatureCoverage.md) maps every component and Lua binding to its scene and behavioral assertions.

The editor also accepts JSON lines as documented in [Automation.md](docs/Automation.md). The runtime opens a game window by default; `--steps` selects deterministic headless execution. [Export.md](docs/Export.md) describes packaging and runtime arguments. Renderer tests use a real Vulkan device and validate pixel readback. Native GUI interaction checks require a display, X11, and `xdotool`:

```sh
xvfb-run -a -s '-screen 0 1600x1000x24' python3 Tests/GuiTests.py \
  --editor build/debug/AsterEditor --assets Assets --artifacts build/gui-evidence
```

The optional upstream glTF corpus downloads five pinned Khronos models and their notices into the ignored build tree. Its test checks imported data and rendered visibility/rotation, and saves images under `build/debug/asset-corpus-evidence`:

```sh
python3 scripts/FetchAssetCorpus.py
cmake --preset debug -DASTER_ASSET_CORPUS="$PWD/build/asset-corpus"
cmake --build --preset debug --parallel 4
ctest --preset debug -R AssetCorpus
```

Native Windows and macOS CPU suites pass CI. Linux Vulkan CI passes its integrated tests and separate GLFW presentation check. Graphics validation uses `scripts/SetupWindowsVulkan.ps1` on Windows and `scripts/BootstrapMacOS.sh` on macOS; both fetch verified toolchains into `build/`. Run Windows setup in a non-elevated PowerShell 7 shell; its automatic driver registration is restricted to ephemeral GitHub-hosted runners. The Windows CI driver is SwiftShader software Vulkan. The macOS toolchain uses MoltenVK and requires a Metal-capable device. Native macOS scene rendering, GLFW presentation, and relocated `.app` execution passed on Apple's paravirtual device, including launch without development SDK paths. Broader editor interaction, audio, and release gates remain in the implementation status.

## Development

Follow [AGENTS.md](AGENTS.md) and repository workflows in `skills/`. Review and test changes before committing. Third-party revisions and archive hashes are in `cmake/Dependencies.lock.json`. Engine code follows Hazel naming conventions.

Windows and macOS are required targets with unverified native release gates. In particular, NVRHI/MoltenVK integration on macOS requires additional validation. See the implementation status for current evidence instead of inferring platform readiness from CMake configuration.
