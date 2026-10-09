# Keep the build-tree runtime usable while making exported copies independent
# of the SDK's absolute library paths. These files come from the verified
# BootstrapMacOS.sh installation, or explicit caller-provided SDK paths.
find_library(ASTER_MOLTENVK_LIBRARY NAMES MoltenVK
  HINTS "$ENV{VULKAN_SDK}/lib" REQUIRED)
find_file(ASTER_MOLTENVK_LICENSE NAMES LICENSE
  HINTS "$ENV{VULKAN_SDK}/share/licenses/MoltenVK" NO_DEFAULT_PATH REQUIRED)
find_file(ASTER_MOLTENVK_ICD NAMES MoltenVK_icd.json
  HINTS "$ENV{VULKAN_SDK}/share/vulkan/icd.d" NO_DEFAULT_PATH REQUIRED)

set_target_properties(AsterRuntime PROPERTIES
  BUILD_WITH_INSTALL_RPATH TRUE
  INSTALL_RPATH "@executable_path/AsterRuntimeDependencies;@executable_path/../Frameworks")
set_property(TARGET AsterRuntime APPEND PROPERTY LINK_DEPENDS
  "${ASTER_MOLTENVK_LIBRARY}" "${ASTER_MOLTENVK_LICENSE}" "${ASTER_MOLTENVK_ICD}")
add_custom_command(TARGET AsterRuntime POST_BUILD
  COMMAND "${CMAKE_COMMAND}" -E make_directory "$<TARGET_FILE_DIR:AsterRuntime>/AsterRuntimeDependencies"
  COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${Vulkan_LIBRARY}"
    "$<TARGET_FILE_DIR:AsterRuntime>/AsterRuntimeDependencies/libvulkan.1.dylib"
  COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${ASTER_MOLTENVK_LIBRARY}"
    "$<TARGET_FILE_DIR:AsterRuntime>/AsterRuntimeDependencies/libMoltenVK.dylib"
  COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${ASTER_MOLTENVK_LICENSE}"
    "$<TARGET_FILE_DIR:AsterRuntime>/AsterRuntimeDependencies/MoltenVK-LICENSE.txt"
  COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${ASTER_MOLTENVK_ICD}"
    "$<TARGET_FILE_DIR:AsterRuntime>/AsterRuntimeDependencies/MoltenVK_icd.json"
  VERBATIM)
