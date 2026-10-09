# Third-party notices

Dependencies come from official upstream sources and are pinned by revision and verified SHA-256 archive digest. General dependency locks are in `cmake/Dependencies.lock.json`; editor-only locks are in `cmake/EditorDependencies.lock.json`; cgltf/stb pins and hashes are in `cmake/AssetDependencies.cmake`. Sources and generated third-party implementation files are downloaded into the build tree. License text in `Licenses/` must accompany distributed binaries.

| Library | Version/revision | License |
| --- | --- | --- |
| GLM | 1.0.1 | MIT or modified MIT |
| GLFW | 3.4 | zlib/libpng |
| nlohmann JSON | 3.11.3 | MIT |
| Lua | 5.4.7 | MIT |
| Bullet | 3.25 | zlib |
| miniaudio | 0.11.23 | Public domain or MIT No Attribution |
| NVRHI | `6b96fb03e07539f08327aea76c56d55f1de9d906` | MIT and bundled third-party notices |
| Vulkan-Loader, optional Windows runtime companion | SDK 1.4.328.1, `0a278cc725089cb67bf6027076e5d72f97c04d86` | Apache-2.0 and permissive notices in upstream LICENSE.txt |
| Vulkan-Headers | 1.4.352 | Per-file Apache-2.0, MIT, and applicable notices in LICENSES |
| cgltf | 1.15, `bbeb5b0b070ddacddac6852fb72143eb68454937` | MIT |
| stb_image | `2c980bb59875b0d32144a71867fbdebb2f77cd20` | Public domain or MIT |
| Dear ImGui, editor only | `52fe0a05a7b1aa180a202bb24f0f2a049a9c1b7d` | MIT |
| ImGuizmo, editor only | `18cef5e031d8c6973d80284c67f60549fafd78c1` | MIT |

The runtime does not link the ImGui/ImGuizmo editor libraries. Export currently copies the complete notice directory, including editor notices, to preserve all upstream license text.

When a selected Windows runtime has an adjacent `vulkan-1.dll`, export copies it beside `AsterGame.exe`. Otherwise it uses the Vulkan loader installed with the machine's graphics driver. The CI setup obtains the optional loader from the official LunarG 1.4.328.1 runtime components archive; its verified archive hash is pinned in `scripts/SetupWindowsVulkan.ps1`. [The upstream loader license](https://github.com/KhronosGroup/Vulkan-Loader/blob/0a278cc725089cb67bf6027076e5d72f97c04d86/LICENSE.txt) is preserved in `Licenses/vulkan_loader.txt`; permissive per-file copyright notices are preserved in `Licenses/vulkan_loader_notices.txt`. SwiftShader is a CI-only software driver and is not included in exported games.

## Asset provenance

`Assets/Audio/Feature.wav` is an original generated 440 Hz sine tone used as a test fixture. The small triangle glTF and generated test fixtures are authored for Aster's automated tests.

`Assets/Environment/StudioSmall09.hdr` is the 1K [Studio Small 09](https://polyhaven.com/a/studio_small_09) HDRI by Sergej Majboroda from Poly Haven, released under [CC0](https://polyhaven.com/license). Its exact official download URL, metadata endpoints, byte length, upstream MD5, and verified SHA-256 are recorded in `Assets/Environment/Attribution.json`. The file is 1,615,248 bytes and its SHA-256 is `e7cfda5f4e98e623db12b8bfd0184e048488e4855d9c83e2751fb44a32e80c45`. The attribution document is copied with the assets during game export. Generated environment caches are rebuildable and remain outside version control.
