include_guard(GLOBAL)
include(FetchContent)

# Official upstream revisions and verified archive digests; downloaded sources
# and generated implementation translation units remain exclusively in build/.
set(asterCgltfURL "https://codeload.github.com/jkuhlmann/cgltf/tar.gz/bbeb5b0b070ddacddac6852fb72143eb68454937")
set(asterStbURL "https://codeload.github.com/nothings/stb/tar.gz/2c980bb59875b0d32144a71867fbdebb2f77cd20")
if(EXISTS "${CMAKE_SOURCE_DIR}/build/_archives/cgltf.tar.gz")
  set(asterCgltfURL "${CMAKE_SOURCE_DIR}/build/_archives/cgltf.tar.gz")
endif()
if(EXISTS "${CMAKE_SOURCE_DIR}/build/_archives/stb.tar.gz")
  set(asterStbURL "${CMAKE_SOURCE_DIR}/build/_archives/stb.tar.gz")
endif()
FetchContent_Declare(cgltf URL "${asterCgltfURL}"
  URL_HASH SHA256=98b987d9a6b0443a830af5bcc019eb2e75d8f2d79988e0aec02db5c849c43ee4
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE SOURCE_SUBDIR AsterStandaloneSources)
FetchContent_Declare(stb URL "${asterStbURL}"
  URL_HASH SHA256=9a955b1b49a4410088a2e0ee2a9c057c3c907d0c1d75454144cb980aca0ba515
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE SOURCE_SUBDIR AsterStandaloneSources)
FetchContent_MakeAvailable(cgltf stb)

file(GENERATE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/AsterAssetThirdParty.c" CONTENT
  "#define CGLTF_IMPLEMENTATION\n#include <cgltf.h>\n#define STB_IMAGE_IMPLEMENTATION\n#define STBI_FAILURE_USERMSG\n#define STBI_MAX_DIMENSIONS 16384\n#include <stb_image.h>\n")
set_source_files_properties("${CMAKE_CURRENT_BINARY_DIR}/AsterAssetThirdParty.c" PROPERTIES GENERATED TRUE)
add_library(AsterAssetThirdParty STATIC "${CMAKE_CURRENT_BINARY_DIR}/AsterAssetThirdParty.c")
target_include_directories(AsterAssetThirdParty SYSTEM PUBLIC "${cgltf_SOURCE_DIR}" "${stb_SOURCE_DIR}")
if(UNIX)
  target_link_libraries(AsterAssetThirdParty PUBLIC m)
endif()
