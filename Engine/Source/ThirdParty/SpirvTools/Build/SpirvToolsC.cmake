# SPIRV-Tools C API 공유 라이브러리(SPIRV-Tools-c) CMake 래퍼다. Build.ps1·Build.sh가 작업 디렉터리에 CMakeLists.txt로 복사하고
# -DROCKS_SPIRV_TOOLS_SOURCE=<원본 트리>로 구성한다. 업스트림 원본은 고치지 않는다.
#
# 업스트림 정적 라이브러리(SPIRV-Tools-static·SPIRV-Tools-opt)를 SPIRV_TOOLS_SHAREDLIB·SPIRV_TOOLS_IMPLEMENTATION으로 컴파일하면
# libspirv.h의 SPIRV_TOOLS_EXPORT 선언(C API와 최적화기 C API)이 dllexport(Windows)·visibility default(macOS)가 된다. 두 정적
# 라이브러리를 whole-archive로 공유 라이브러리 하나에 묶어 그 선언만 내보낸다(나머지는 숨김).
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
# SPIRV-Tools-opt가 SPIRV-Tools-static을 일반 의존으로도 링크하므로 같은 항목의 링크 방식을 whole-archive로 맞춘다.
set_property(TARGET SPIRV-Tools-c PROPERTY LINK_LIBRARY_OVERRIDE_SPIRV-Tools-static WHOLE_ARCHIVE)
set_target_properties(SPIRV-Tools-c PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}
  LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}
  PDB_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
