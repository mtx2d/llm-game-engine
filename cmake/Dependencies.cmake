include(FetchContent)
find_package(Threads REQUIRED)

# CMake 4 removed pre-3.5 policy compatibility. Raise the policy floor for
# pinned third-party subdirectories (not Aster's own declared policy version).
# https://cmake.org/cmake/help/latest/variable/CMAKE_POLICY_VERSION_MINIMUM.html
if(CMAKE_VERSION VERSION_GREATER_EQUAL 4.0 AND
   (NOT DEFINED CMAKE_POLICY_VERSION_MINIMUM OR CMAKE_POLICY_VERSION_MINIMUM VERSION_LESS 3.5))
  set(CMAKE_POLICY_VERSION_MINIMUM 3.5)
endif()

# Each archive is pinned by both its source revision and content digest. The
# optional local archive cache avoids downloads on subsequent configurations.
file(READ "${CMAKE_CURRENT_LIST_DIR}/Dependencies.lock.json" asterDependencyLock)
function(aster_dependency name)
  string(JSON dependencyURL GET "${asterDependencyLock}" "${name}" URL)
  string(JSON dependencyHash GET "${asterDependencyLock}" "${name}" SHA256)
  set(localArchive "${CMAKE_SOURCE_DIR}/build/_archives/${name}.tar.gz")
  if(EXISTS "${localArchive}")
    set(dependencyURL "${localArchive}")
  endif()
  set(sourceSubdirectory "")
  if(name STREQUAL "lua" OR name STREQUAL "miniaudio")
    set(sourceSubdirectory SOURCE_SUBDIR AsterStandaloneSources)
  endif()
  FetchContent_Declare(${name} URL "${dependencyURL}" URL_HASH "SHA256=${dependencyHash}"
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE ${sourceSubdirectory})
endfunction()

set(JSON_BuildTests OFF CACHE BOOL "" FORCE)
set(GLM_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(GLM_BUILD_LIBRARY OFF CACHE BOOL "" FORCE)
aster_dependency(glm)
aster_dependency(json)
FetchContent_MakeAvailable(glm json)

aster_dependency(lua)
FetchContent_MakeAvailable(lua)
file(GLOB luaSources CONFIGURE_DEPENDS "${lua_SOURCE_DIR}/src/*.c")
list(REMOVE_ITEM luaSources "${lua_SOURCE_DIR}/src/lua.c" "${lua_SOURCE_DIR}/src/luac.c")
# Lua supports C++ exceptions for protected errors. This preserves C++ ownership
# when an allocation failure unwinds an engine binding. Include the public C-linkage
# declarations first, without modifying the pinned upstream sources.
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/LuaCpp")
set(luaCppSources "")
foreach(luaSource IN LISTS luaSources)
  get_filename_component(luaName "${luaSource}" NAME_WE)
  set(luaWrapper "${CMAKE_CURRENT_BINARY_DIR}/LuaCpp/${luaName}.cpp")
  file(STRINGS "${luaSource}" luaMode REGEX "^#define LUA_(CORE|LIB)$")
  list(LENGTH luaMode luaModeCount)
  if(NOT luaModeCount EQUAL 1)
    message(FATAL_ERROR "Cannot determine upstream Lua compilation mode for ${luaSource}")
  endif()
  file(GENERATE OUTPUT "${luaWrapper}" CONTENT "${luaMode}\n#include <lprefix.h>\n#include <lua.hpp>\n#include \"${luaSource}\"\n")
  set_source_files_properties("${luaWrapper}" PROPERTIES GENERATED TRUE)
  list(APPEND luaCppSources "${luaWrapper}")
endforeach()
add_library(AsterLua STATIC ${luaCppSources})
target_compile_features(AsterLua PRIVATE cxx_std_20)
if(MSVC)
  # Also protect consumers that call Lua's C-linkage API (including Simulation).
  # Clear only the C no-throw assumption; preserve a user's /EHs or /EHa model.
  target_compile_options(AsterLua PUBLIC /EHc-)
endif()
target_include_directories(AsterLua SYSTEM PUBLIC "${lua_SOURCE_DIR}/src")
if(UNIX)
  target_compile_definitions(AsterLua PRIVATE LUA_USE_POSIX)
  target_link_libraries(AsterLua PUBLIC m ${CMAKE_DL_LIBS})
endif()

if(MSVC)
  # Bullet and GLFW share this legacy option. Bullet defaults it OFF, causing
  # GLFW to force /MT while Aster uses CMake's /MD default. Disable their legacy
  # overrides and let every target inherit CMAKE_MSVC_RUNTIME_LIBRARY instead;
  # an explicitly selected parent /MT remains supported through that property.
  set(USE_MSVC_RUNTIME_LIBRARY_DLL ON CACHE BOOL "Follow the parent MSVC runtime selection" FORCE)
endif()
set(BUILD_BULLET2_DEMOS OFF CACHE BOOL "" FORCE)
set(BUILD_CPU_DEMOS OFF CACHE BOOL "" FORCE)
set(BUILD_OPENGL3_DEMOS OFF CACHE BOOL "" FORCE)
set(BUILD_UNIT_TESTS OFF CACHE BOOL "" FORCE)
set(BUILD_EXTRAS OFF CACHE BOOL "" FORCE)
set(BUILD_BULLET3 OFF CACHE BOOL "" FORCE)
set(INSTALL_LIBS OFF CACHE BOOL "" FORCE)
set(USE_GRAPHICAL_BENCHMARK OFF CACHE BOOL "" FORCE)
aster_dependency(bullet)
FetchContent_MakeAvailable(bullet)
# Only the linked dynamics/collision/math targets belong to Aster's default build.
set_property(DIRECTORY "${bullet_SOURCE_DIR}" PROPERTY EXCLUDE_FROM_ALL TRUE)
aster_dependency(miniaudio)
FetchContent_MakeAvailable(miniaudio)

if(ASTER_BUILD_RENDERER)
  set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
  set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
  set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
  set(GLFW_INSTALL OFF CACHE BOOL "" FORCE)
  option(GLFW_BUILD_WAYLAND "Build GLFW Wayland support" OFF)
  aster_dependency(glfw)
  FetchContent_MakeAvailable(glfw)
  aster_dependency(vulkan_headers)
  FetchContent_MakeAvailable(vulkan_headers)
  # Import loader only after the matching pinned Vulkan::Headers target exists.
  find_package(Vulkan REQUIRED)
  set(NVRHI_BUILD_SHARED OFF CACHE BOOL "" FORCE)
  set(NVRHI_WITH_DX11 OFF CACHE BOOL "" FORCE)
  set(NVRHI_WITH_DX12 OFF CACHE BOOL "" FORCE)
  set(NVRHI_WITH_VULKAN ON CACHE BOOL "" FORCE)
  set(NVRHI_WITH_VALIDATION ON CACHE BOOL "" FORCE)
  set(NVRHI_INSTALL OFF CACHE BOOL "" FORCE)
  set(NVRHI_FETCH_VULKAN_HEADERS OFF CACHE BOOL "" FORCE)
  aster_dependency(nvrhi)
  FetchContent_MakeAvailable(nvrhi)
endif()
