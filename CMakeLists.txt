cmake_minimum_required(VERSION 3.24)
project(Aster VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
option(ASTER_BUILD_RENDERER "Build the GLFW/NVRHI Vulkan renderer" ON)
option(ASTER_BUILD_GUI "Build the graphical editor frontend" ON)
option(ASTER_BUILD_TESTS "Build Aster automated tests" ON)
option(ASTER_SANITIZE "Enable address and undefined behavior sanitizers" OFF)
option(ASTER_WARNINGS_AS_ERRORS "Treat Aster warnings as errors" ON)

include(cmake/CompilerSettings.cmake)

if(ASTER_SANITIZE)
  if(MSVC)
    message(FATAL_ERROR "Use Clang or GCC for the sanitizer preset")
  endif()
  add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
  add_link_options(-fsanitize=address,undefined)
endif()

include(cmake/Dependencies.cmake)
include(cmake/AssetDependencies.cmake)

function(aster_target target)
  target_compile_features(${target} PUBLIC cxx_std_20)
  if(WIN32)
    target_compile_definitions(${target} PRIVATE NOMINMAX)
  endif()
  if(MSVC)
    target_compile_options(${target} PRIVATE /W4 /permissive-)
    if(ASTER_WARNINGS_AS_ERRORS)
      target_compile_options(${target} PRIVATE /WX)
    endif()
  else()
    target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
    if(ASTER_WARNINGS_AS_ERRORS)
      target_compile_options(${target} PRIVATE -Werror)
    endif()
  endif()
endfunction()

add_library(Aster STATIC
  Engine/src/Core/DirectoryPublication.cpp
  Engine/src/Core/ExecutablePath.cpp
  Engine/src/Core/FileLock.cpp
  Engine/src/Core/Hash.cpp
  Engine/src/Core/JsonFile.cpp
  Engine/src/Core/PrivateDirectory.cpp
  Engine/src/Project/Project.cpp
  Engine/src/Input/InputState.cpp
  Engine/src/Renderer/Environment.cpp
  Engine/src/Assets/AssetImporter.cpp
  Engine/src/Assets/AssetIO.cpp
  Engine/src/Scene/Scene.cpp
  Engine/src/Scene/SceneSerializer.cpp
  Engine/src/Simulation/Simulation.cpp
)
target_include_directories(Aster PUBLIC Engine/include)
target_include_directories(Aster SYSTEM PRIVATE "${bullet_SOURCE_DIR}/src" "${miniaudio_SOURCE_DIR}")
target_link_libraries(Aster PUBLIC glm::glm nlohmann_json::nlohmann_json PRIVATE AsterAssetThirdParty AsterLua BulletDynamics BulletCollision LinearMath ${CMAKE_DL_LIBS} Threads::Threads)
target_compile_definitions(Aster PUBLIC GLM_FORCE_RADIANS GLM_FORCE_DEPTH_ZERO_TO_ONE)
target_compile_definitions(Aster PRIVATE GLM_ENABLE_EXPERIMENTAL)
aster_target(Aster)

add_library(AsterEditorCommands STATIC Editor/src/CommandProcessor.cpp Editor/src/ProjectExporter.cpp
  Editor/src/SceneDocumentFile.cpp Editor/src/EditorStorage.cpp Editor/src/RecoveryStore.cpp)
target_include_directories(AsterEditorCommands PUBLIC Editor/include)
target_link_libraries(AsterEditorCommands PUBLIC Aster)
aster_target(AsterEditorCommands)
add_executable(AsterEditor Editor/src/Main.cpp)
if(NOT ASTER_BUILD_RENDERER OR NOT ASTER_BUILD_GUI)
  target_link_libraries(AsterEditor PRIVATE AsterEditorCommands)
endif()
aster_target(AsterEditor)
add_executable(AsterRuntime Runtime/src/Main.cpp)
if(NOT ASTER_BUILD_RENDERER)
  target_link_libraries(AsterRuntime PRIVATE Aster)
endif()
aster_target(AsterRuntime)

if(ASTER_BUILD_RENDERER)
  include(cmake/Shaders.cmake)
  add_library(AsterRenderer STATIC Engine/src/Renderer/Renderer.cpp Engine/src/Renderer/SceneRenderPass.cpp)
  target_include_directories(AsterRenderer PUBLIC Engine/include)
  target_link_libraries(AsterRenderer PRIVATE Vulkan::Headers nvrhi_vk nvrhi glfw Vulkan::Vulkan)
  target_link_libraries(AsterRenderer PUBLIC Aster)
  aster_shader(AsterRenderer Shaders/Pbr.vert PbrVert)
  aster_shader(AsterRenderer Shaders/Pbr.frag PbrFrag)
  aster_shader(AsterRenderer Shaders/Fullscreen.vert FullscreenVert)
  aster_shader(AsterRenderer Shaders/Tonemap.frag TonemapFrag)
  aster_shader(AsterRenderer Shaders/Sky.frag SkyFrag)
  aster_shader(AsterRenderer Shaders/Shadow.vert ShadowVert)
  aster_shader(AsterRenderer Shaders/Depth.frag DepthFrag)
  aster_shader(AsterRenderer Shaders/Depth.vert DepthVert)
  aster_shader(AsterRenderer Shaders/Ssao.frag SsaoFrag)
  aster_shader(AsterRenderer Shaders/SsaoBlur.frag SsaoBlurFrag)
  aster_target(AsterRenderer)
  if(ASTER_BUILD_GUI)
    include(cmake/EditorDependencies.cmake)
    add_library(AsterEditorGui STATIC Editor/src/GuiRenderer.cpp Editor/src/GuiLayer.cpp)
    target_include_directories(AsterEditorGui PUBLIC Editor/include)
    target_link_libraries(AsterEditorGui PUBLIC AsterEditorCommands AsterRenderer PRIVATE AsterImGui nvrhi glfw)
    aster_shader(AsterEditorGui Editor/Shaders/Gui.vert GuiVert)
    aster_shader(AsterEditorGui Editor/Shaders/Gui.frag GuiFrag)
    aster_target(AsterEditorGui)
    target_compile_definitions(AsterEditorGui PRIVATE GLM_ENABLE_EXPERIMENTAL)
    target_link_libraries(AsterEditor PRIVATE AsterEditorGui)
    target_compile_definitions(AsterEditor PRIVATE ASTER_HAS_RENDERER)
  endif()
  target_link_libraries(AsterRuntime PRIVATE AsterRenderer)
  target_compile_definitions(AsterRuntime PRIVATE ASTER_HAS_RENDERER)
  if(APPLE)
    include(cmake/MacOSRuntime.cmake)
  endif()
endif()

if(ASTER_BUILD_TESTS)
  enable_testing()
  add_executable(AsterTests Tests/TestMain.cpp Tests/SceneTests.cpp Tests/SimulationTests.cpp Tests/CommandTests.cpp Tests/AssetTests.cpp Tests/EnvironmentTests.cpp Tests/ViewportTests.cpp Tests/ProjectTests.cpp)
  target_link_libraries(AsterTests PRIVATE AsterEditorCommands)
  target_include_directories(AsterTests SYSTEM PRIVATE "${miniaudio_SOURCE_DIR}")
  target_compile_definitions(AsterTests PRIVATE ASTER_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
  aster_target(AsterTests)
  add_test(NAME Scene COMMAND AsterTests scene)
  add_test(NAME Simulation COMMAND AsterTests simulation)
  add_test(NAME Commands COMMAND AsterTests commands)
  add_test(NAME Assets COMMAND AsterTests assets)
  add_test(NAME Environment COMMAND AsterTests environment)
  add_test(NAME Viewport COMMAND AsterTests viewport)
  add_test(NAME Project COMMAND AsterTests project)
  set_tests_properties(Scene Simulation Commands Assets Environment Viewport Project PROPERTIES TIMEOUT 90)
  add_test(NAME FeatureGallery COMMAND AsterRuntime
    --scene "${CMAKE_CURRENT_SOURCE_DIR}/Assets/Scenes/FeatureGallery.aster"
    --project "${CMAKE_CURRENT_SOURCE_DIR}/Assets" --steps 120)
  set_tests_properties(FeatureGallery PROPERTIES TIMEOUT 60 PASS_REGULAR_EXPRESSION "all 32 engine bindings exercised")
  find_package(Python3 COMPONENTS Interpreter REQUIRED)
  add_executable(AsterFileLockTests Tests/FileLockTests.cpp)
  target_link_libraries(AsterFileLockTests PRIVATE AsterEditorCommands)
  aster_target(AsterFileLockTests)
  add_test(NAME FileLock COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/Tests/FileLockTests.py"
    "$<TARGET_FILE:AsterFileLockTests>" "$<TARGET_FILE:AsterEditor>")
  set_tests_properties(FileLock PROPERTIES TIMEOUT 120)
  add_executable(AsterRecoveryTests Tests/RecoveryTests.cpp)
  target_link_libraries(AsterRecoveryTests PRIVATE AsterEditorCommands)
  aster_target(AsterRecoveryTests)
  add_test(NAME Recovery COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/Tests/RecoveryTests.py"
    --probe "$<TARGET_FILE:AsterRecoveryTests>" --editor "$<TARGET_FILE:AsterEditor>"
    --assets "${CMAKE_CURRENT_SOURCE_DIR}/Assets")
  set_tests_properties(Recovery PROPERTIES TIMEOUT 180)
  add_test(NAME Protocol COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/Tests/ProtocolTests.py"
    "$<TARGET_FILE:AsterEditor>" "$<TARGET_FILE:AsterRuntime>")
  set_tests_properties(Protocol PROPERTIES TIMEOUT 90)
  set(asterProjectWorkflowCommand "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/Tests/ProjectWorkflowTests.py"
    --editor "$<TARGET_FILE:AsterEditor>" --runtime "$<TARGET_FILE:AsterRuntime>"
    --assets "${CMAKE_CURRENT_SOURCE_DIR}/Assets" --notices "${CMAKE_CURRENT_SOURCE_DIR}/ThirdParty")
  add_test(NAME ProjectWorkflow COMMAND ${asterProjectWorkflowCommand})
  set_tests_properties(ProjectWorkflow PROPERTIES TIMEOUT 120)
  add_test(NAME Export COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/Tests/ExportTests.py"
    --editor "$<TARGET_FILE:AsterEditor>" --runtime "$<TARGET_FILE:AsterRuntime>"
    --assets "${CMAKE_CURRENT_SOURCE_DIR}/Assets" --notices "${CMAKE_CURRENT_SOURCE_DIR}/ThirdParty")
  set_tests_properties(Export PROPERTIES TIMEOUT 180)
  add_test(NAME BlockStack COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/Tests/BlockStackTests.py"
    --editor "$<TARGET_FILE:AsterEditor>" --runtime "$<TARGET_FILE:AsterRuntime>"
    --assets "${CMAKE_CURRENT_SOURCE_DIR}/Assets" --notices "${CMAKE_CURRENT_SOURCE_DIR}/ThirdParty")
  set_tests_properties(BlockStack PROPERTIES TIMEOUT 180)
  if(ASTER_BUILD_RENDERER)
    add_executable(AsterRendererTests Tests/RendererTestMain.cpp Tests/RendererTests.cpp)
    target_link_libraries(AsterRendererTests PRIVATE AsterRenderer glfw)
    target_compile_definitions(AsterRendererTests PRIVATE ASTER_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
    aster_target(AsterRendererTests)
    add_test(NAME Renderer COMMAND AsterRendererTests --evidence "${CMAKE_CURRENT_BINARY_DIR}/renderer-evidence")
    set_tests_properties(Renderer PROPERTIES TIMEOUT 60 LABELS "gpu")
    if(WIN32 OR APPLE)
      add_test(NAME RendererWindow COMMAND AsterRendererTests)
      set_tests_properties(RendererWindow PROPERTIES
        ENVIRONMENT "ASTER_TEST_WINDOW=1" TIMEOUT 120 LABELS "gpu;window")
    elseif(UNIX)
      find_program(ASTER_XVFB_RUN xvfb-run)
      if(ASTER_XVFB_RUN)
        add_test(NAME RendererWindow COMMAND "${ASTER_XVFB_RUN}" -a
          -s "-screen 0 1280x1024x24 -noreset" "${CMAKE_COMMAND}" -E env
          ASTER_TEST_WINDOW=1 "$<TARGET_FILE:AsterRendererTests>")
        set_tests_properties(RendererWindow PROPERTIES TIMEOUT 120 LABELS "gpu;window")
        find_program(ASTER_OPENBOX openbox)
        find_program(ASTER_XPROP xprop)
        if(ASTER_OPENBOX AND ASTER_XPROP)
          add_test(NAME RendererWindowManaged COMMAND "${ASTER_XVFB_RUN}" -a
            -s "-screen 0 1280x1024x24 -noreset" "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/Tests/ManagedWindowTests.py"
            --renderer "$<TARGET_FILE:AsterRendererTests>" --window-manager "${ASTER_OPENBOX}")
          set_tests_properties(RendererWindowManaged PROPERTIES TIMEOUT 130 LABELS "gpu;window")
        endif()
      endif()
    endif()
    if(WIN32)
      # The pinned Windows CI software Vulkan ICD needs more time for image tests.
      set_tests_properties(Renderer RendererWindow PROPERTIES TIMEOUT 300)
      add_test(NAME RuntimeInput COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/Tests/WindowsRuntimeInputTests.py"
        --editor "$<TARGET_FILE:AsterEditor>" --runtime "$<TARGET_FILE:AsterRuntime>"
        --assets "${CMAKE_CURRENT_SOURCE_DIR}/Assets" --notices "${CMAKE_CURRENT_SOURCE_DIR}/ThirdParty"
        --artifacts "${CMAKE_CURRENT_BINARY_DIR}/runtime-input-evidence")
      set_tests_properties(RuntimeInput PROPERTIES TIMEOUT 180 LABELS "gpu;export;input")
    elseif(APPLE)
      add_test(NAME RuntimeInput COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/Tests/MacOSRuntimeInputTests.py"
        --editor "$<TARGET_FILE:AsterEditor>" --runtime "$<TARGET_FILE:AsterRuntime>"
        --assets "${CMAKE_CURRENT_SOURCE_DIR}/Assets" --notices "${CMAKE_CURRENT_SOURCE_DIR}/ThirdParty"
        --artifacts "${CMAKE_CURRENT_BINARY_DIR}/runtime-input-evidence")
      set_tests_properties(RuntimeInput PROPERTIES TIMEOUT 300 LABELS "gpu;export;input")
    endif()
    add_executable(AsterAssetCorpusTests Tests/AssetCorpusTests.cpp)
    target_link_libraries(AsterAssetCorpusTests PRIVATE AsterRenderer)
    target_compile_definitions(AsterAssetCorpusTests PRIVATE ASTER_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
    aster_target(AsterAssetCorpusTests)
    set(ASTER_ASSET_CORPUS "" CACHE PATH "Optional directory of downloaded glTF conformance assets")
    if(ASTER_ASSET_CORPUS)
      add_test(NAME AssetCorpus COMMAND AsterAssetCorpusTests "${ASTER_ASSET_CORPUS}"
        "${CMAKE_CURRENT_BINARY_DIR}/asset-corpus-evidence")
      set_tests_properties(AssetCorpus PROPERTIES TIMEOUT 180 LABELS "gpu;assets")
    endif()
    set(asterEditorRenderCommand "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/Tests/EditorRenderTests.py"
      --editor "$<TARGET_FILE:AsterEditor>" --assets "${CMAKE_CURRENT_SOURCE_DIR}/Assets"
      --artifacts "${CMAKE_CURRENT_BINARY_DIR}/editor-render-evidence")
    if(ASTER_BUILD_GUI AND (WIN32 OR APPLE))
      add_test(NAME ProjectWindow COMMAND ${asterProjectWorkflowCommand} --graphical)
      set_tests_properties(ProjectWindow PROPERTIES TIMEOUT 180 LABELS "gpu;gui;project")
      add_test(NAME EditorRender COMMAND ${asterEditorRenderCommand})
      set_tests_properties(EditorRender PROPERTIES TIMEOUT 300 LABELS "gpu;gui")
      if(WIN32)
        set(asterNativeEditorTest "${CMAKE_CURRENT_SOURCE_DIR}/Tests/WindowsEditorTests.py")
      else()
        set(asterNativeEditorTest "${CMAKE_CURRENT_SOURCE_DIR}/Tests/MacOSEditorTests.py")
      endif()
      add_test(NAME NativeEditor COMMAND "${Python3_EXECUTABLE}" "${asterNativeEditorTest}"
        --editor "$<TARGET_FILE:AsterEditor>" --assets "${CMAKE_CURRENT_SOURCE_DIR}/Assets"
        --artifacts "${CMAKE_CURRENT_BINARY_DIR}/native-editor-evidence")
      set_tests_properties(NativeEditor PROPERTIES TIMEOUT 300 LABELS "gpu;gui;input")
      add_test(NAME NativeRecovery COMMAND "${Python3_EXECUTABLE}" "${asterNativeEditorTest}"
        --recovery-only --editor "$<TARGET_FILE:AsterEditor>" --assets "${CMAKE_CURRENT_SOURCE_DIR}/Assets"
        --artifacts "${CMAKE_CURRENT_BINARY_DIR}/native-recovery-evidence")
      set_tests_properties(NativeRecovery PROPERTIES TIMEOUT 300 LABELS "gpu;gui;input;recovery")
    endif()
    if(ASTER_BUILD_GUI AND UNIX AND NOT APPLE)
      find_program(ASTER_XVFB_RUN xvfb-run)
      find_program(ASTER_XDOTOOL xdotool)
      if(ASTER_XVFB_RUN AND ASTER_XDOTOOL)
        add_test(NAME ProjectWindow COMMAND "${ASTER_XVFB_RUN}" -a -s "-screen 0 1600x1000x24 -noreset"
          ${asterProjectWorkflowCommand} --graphical)
        set_tests_properties(ProjectWindow PROPERTIES TIMEOUT 180 LABELS "gpu;gui;project")
        # Keep Xvfb available between process/renderer lifetime checks.
        add_test(NAME EditorRender COMMAND "${ASTER_XVFB_RUN}" -a -s "-screen 0 1600x1000x24 -noreset"
          ${asterEditorRenderCommand})
        set_tests_properties(EditorRender PROPERTIES TIMEOUT 300 LABELS "gpu;gui")
        add_test(NAME EditorGui COMMAND "${ASTER_XVFB_RUN}" -a -s "-screen 0 1600x1000x24 -noreset"
          "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/Tests/GuiTests.py"
          --editor "$<TARGET_FILE:AsterEditor>" --assets "${CMAKE_CURRENT_SOURCE_DIR}/Assets"
          --artifacts "${CMAKE_CURRENT_BINARY_DIR}/gui-evidence")
        set_tests_properties(EditorGui PROPERTIES TIMEOUT 300 LABELS "gpu;gui")
        add_test(NAME RecoveryGui COMMAND "${ASTER_XVFB_RUN}" -a -s "-screen 0 1600x1000x24 -noreset"
          "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/Tests/RecoveryGuiTests.py"
          --editor "$<TARGET_FILE:AsterEditor>" --assets "${CMAKE_CURRENT_SOURCE_DIR}/Assets"
          --artifacts "${CMAKE_CURRENT_BINARY_DIR}/recovery-gui-evidence")
        set_tests_properties(RecoveryGui PROPERTIES TIMEOUT 180 LABELS "gpu;gui;recovery")
        add_test(NAME GraphicalExport COMMAND "${ASTER_XVFB_RUN}" -a -s "-screen 0 1280x1024x24 -noreset"
          "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/Tests/ExportTests.py"
          --editor "$<TARGET_FILE:AsterEditor>" --runtime "$<TARGET_FILE:AsterRuntime>"
          --assets "${CMAKE_CURRENT_SOURCE_DIR}/Assets" --notices "${CMAKE_CURRENT_SOURCE_DIR}/ThirdParty"
          --graphical)
        set_tests_properties(GraphicalExport PROPERTIES TIMEOUT 180 LABELS "gpu;export")
        add_test(NAME RuntimeInput COMMAND "${ASTER_XVFB_RUN}" -a -s "-screen 0 1440x900x24 -noreset"
          "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/Tests/RuntimeInputTests.py"
          --editor "$<TARGET_FILE:AsterEditor>" --runtime "$<TARGET_FILE:AsterRuntime>"
          --assets "${CMAKE_CURRENT_SOURCE_DIR}/Assets" --notices "${CMAKE_CURRENT_SOURCE_DIR}/ThirdParty"
          --artifacts "${CMAKE_CURRENT_BINARY_DIR}/runtime-input-evidence")
        set_tests_properties(RuntimeInput PROPERTIES TIMEOUT 120 LABELS "gpu;export;input")
      else()
        message(STATUS "Native GUI interaction test requires xvfb-run and xdotool")
      endif()
    endif()
  endif()
endif()
