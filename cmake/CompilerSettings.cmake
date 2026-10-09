# Lua is compiled as C++ and throws through its extern-C declarations. MSVC's
# default /EHsc incorrectly treats those declarations as nonthrowing. Replace
# that exact default with /EHs instead of emitting contradictory flags (D9025).
# These are directory-scope values: do not rewrite a caller's cached settings.
if(MSVC)
  get_cmake_property(asterCompilerVariables VARIABLES)
  foreach(asterFlagVariable IN LISTS asterCompilerVariables)
    if(asterFlagVariable MATCHES "^CMAKE_CXX_FLAGS($|_[A-Z_]+$)")
      set(asterExceptionFlags " ${${asterFlagVariable}} ")
      while(asterExceptionFlags MATCHES "[ \t][-/]EHsc[ \t]")
        string(REGEX REPLACE "([ \t])[-/]EHsc([ \t])" "\\1/EHs\\2"
          asterExceptionFlags "${asterExceptionFlags}")
      endwhile()
      if(asterExceptionFlags MATCHES "[ \t][-/]EH[as]-[ \t]")
        message(FATAL_ERROR "Aster requires C++ exception unwinding; remove /EHs- or /EHa- from ${asterFlagVariable}")
      endif()
      string(STRIP "${asterExceptionFlags}" ${asterFlagVariable})
    endif()
  endforeach()
  # Preserve a custom asynchronous /EHa model; only supply /EHs when the user
  # removed the base exception flags entirely. Per-config /EHa can still apply.
  if(NOT " ${CMAKE_CXX_FLAGS} " MATCHES "[ \t][-/]EH[as][^ \t]*[ \t]")
    string(APPEND CMAKE_CXX_FLAGS " /EHs")
  endif()
endif()
