include(FetchContent)
file(READ "${CMAKE_CURRENT_LIST_DIR}/EditorDependencies.lock.json" editorDependencyLock)
foreach(dependency IN ITEMS imgui imguizmo)
  string(JSON dependencyURL GET "${editorDependencyLock}" "${dependency}" URL)
  string(JSON dependencyHash GET "${editorDependencyLock}" "${dependency}" SHA256)
  set(localArchive "${CMAKE_SOURCE_DIR}/build/_archives/${dependency}.tar.gz")
  if(EXISTS "${localArchive}")
    set(dependencyURL "${localArchive}")
  endif()
  FetchContent_Declare(${dependency} URL "${dependencyURL}" URL_HASH "SHA256=${dependencyHash}"
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE SOURCE_SUBDIR AsterStandaloneSources)
  FetchContent_MakeAvailable(${dependency})
endforeach()
add_library(AsterImGui STATIC
  "${imgui_SOURCE_DIR}/imgui.cpp" "${imgui_SOURCE_DIR}/imgui_draw.cpp"
  "${imgui_SOURCE_DIR}/imgui_tables.cpp" "${imgui_SOURCE_DIR}/imgui_widgets.cpp"
  "${imgui_SOURCE_DIR}/backends/imgui_impl_glfw.cpp"
  "${imguizmo_SOURCE_DIR}/src/ImGuizmo.cpp")
target_include_directories(AsterImGui SYSTEM PUBLIC "${imgui_SOURCE_DIR}" "${imgui_SOURCE_DIR}/backends" "${imguizmo_SOURCE_DIR}/src")
target_link_libraries(AsterImGui PRIVATE glfw)
target_compile_definitions(AsterImGui PRIVATE GLFW_INCLUDE_NONE)
