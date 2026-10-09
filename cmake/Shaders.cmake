function(aster_shader target source symbol)
  find_program(ASTER_GLSLANG_VALIDATOR glslangValidator REQUIRED)
  set(shaderDirectory "${CMAKE_CURRENT_BINARY_DIR}/generated/AsterShaders")
  file(MAKE_DIRECTORY "${shaderDirectory}")
  set(spirv "${shaderDirectory}/${symbol}.spv")
  set(header "${shaderDirectory}/${symbol}.h")
  add_custom_command(OUTPUT "${header}"
    COMMAND "${ASTER_GLSLANG_VALIDATOR}" --target-env vulkan1.3 -V "${CMAKE_CURRENT_SOURCE_DIR}/${source}" -o "${spirv}"
    COMMAND "${CMAKE_COMMAND}" "-DINPUT=${spirv}" "-DOUTPUT=${header}" "-DSYMBOL=${symbol}"
      -P "${CMAKE_CURRENT_SOURCE_DIR}/Shaders/EmbedSpirv.cmake"
    DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/${source}" "${CMAKE_CURRENT_SOURCE_DIR}/Shaders/EmbedSpirv.cmake"
    VERBATIM)
  target_sources(${target} PRIVATE "${header}")
  target_include_directories(${target} PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated")
endfunction()
