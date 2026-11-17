# CMake wrapper for the SPIRV-Tools C API shared library (SPIRV-Tools-c). Build.ps1 and Build.sh copy it into the work directory as CMakeLists.txt and
# configure it with -DROCKS_SPIRV_TOOLS_SOURCE=<source tree>. Upstream sources are not modified.
#
# Compiling the upstream static libraries (SPIRV-Tools-static, SPIRV-Tools-opt) with SPIRV_TOOLS_SHAREDLIB and SPIRV_TOOLS_IMPLEMENTATION makes
# libspirv.h's SPIRV_TOOLS_EXPORT declarations (the C API and the optimizer C API) dllexport (Windows) or visibility default (macOS). The two static
# libraries are bundled with whole-archive into one shared library that exports only those declarations (everything else is hidden).
cmake_minimum_required(VERSION 3.24)
project(RocksSpirvToolsC LANGUAGES C CXX)

if(NOT ROCKS_SPIRV_TOOLS_SOURCE)
  message(FATAL_ERROR "ROCKS_SPIRV_TOOLS_SOURCE must name the SPIRV-Tools source tree")
endif()

set(CMAKE_CXX_VISIBILITY_PRESET hidden)
set(CMAKE_VISIBILITY_INLINES_HIDDEN ON)
set(SPIRV_SKIP_TESTS ON CACHE BOOL "" FORCE)
set(SPIRV_SKIP_EXECUTABLES ON CACHE BOOL "" FORCE)
set(SPIRV_TOOLS_BUILD_STATIC ON CACHE BOOL "" FORCE)
set(SPIRV_TOOLS_USE_MIMALLOC OFF CACHE BOOL "" FORCE)
set(SPIRV_WERROR OFF CACHE BOOL "" FORCE)
set(SKIP_SPIRV_TOOLS_INSTALL ON CACHE BOOL "" FORCE)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
add_compile_definitions(SPIRV_TOOLS_SHAREDLIB SPIRV_TOOLS_IMPLEMENTATION)
add_subdirectory(${ROCKS_SPIRV_TOOLS_SOURCE} spirv-tools EXCLUDE_FROM_ALL)

file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/SpirvToolsC.cpp "// SPIRV-Tools-c: exports come from the whole-archive static libraries.\n")
add_library(SPIRV-Tools-c SHARED ${CMAKE_CURRENT_BINARY_DIR}/SpirvToolsC.cpp)
target_link_libraries(SPIRV-Tools-c PRIVATE
  "$<LINK_LIBRARY:WHOLE_ARCHIVE,SPIRV-Tools-opt>"
  "$<LINK_LIBRARY:WHOLE_ARCHIVE,SPIRV-Tools-static>")
# SPIRV-Tools-opt also links SPIRV-Tools-static as a normal dependency, so the link mode of that same item is set to whole-archive.
set_property(TARGET SPIRV-Tools-c PROPERTY LINK_LIBRARY_OVERRIDE_SPIRV-Tools-static WHOLE_ARCHIVE)
set_target_properties(SPIRV-Tools-c PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}
  LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}
  PDB_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
