cmake_minimum_required(VERSION 3.25)
project(RocksFoundation VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
option(ROCKS_BUILD_OBJECT "Build Object integration after Core" ON)
option(ROCKS_ASAN "Enable AddressSanitizer for the CPU Foundation" OFF)

if(NOT WIN32 AND NOT APPLE)
    message(FATAL_ERROR "This first Foundation implements Windows/macOS only; Linux is not yet implemented")
endif()

add_library(rocks_options INTERFACE)
if(MSVC)
    string(REPLACE "/EHsc" "" CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS}")
    target_compile_options(rocks_options INTERFACE /W4 /WX /GR- /EHs-c- /Zc:__cplusplus)
    target_compile_definitions(rocks_options INTERFACE _CRT_SECURE_NO_WARNINGS NOMINMAX WIN32_LEAN_AND_MEAN)
else()
    target_compile_options(rocks_options INTERFACE -Wall -Wextra -Werror -fno-exceptions -fno-rtti)
endif()
if(WIN32)
    set(ROCKS_PLATFORM_DIRECTORY Win64)
    target_compile_definitions(rocks_options INTERFACE ROCKS_PLATFORM_WINDOWS=1)
else()
    set(ROCKS_PLATFORM_DIRECTORY Mac)
    target_compile_definitions(rocks_options INTERFACE ROCKS_PLATFORM_MAC=1)
endif()
if(ROCKS_ASAN)
    if(MSVC)
        set(CMAKE_MSVC_RUNTIME_LIBRARY MultiThreadedDLL)
        string(REPLACE "/RTC1" "" CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG}")
        target_compile_options(rocks_options INTERFACE /fsanitize=address)
        execute_process(COMMAND ${CMAKE_CXX_COMPILER} /clang:-print-resource-dir
            OUTPUT_VARIABLE ROCKS_CLANG_RESOURCE OUTPUT_STRIP_TRAILING_WHITESPACE COMMAND_ERROR_IS_FATAL ANY)
        find_library(ROCKS_ASAN_IMPORT NAMES clang_rt.asan_dynamic clang_rt.asan_dynamic-x86_64
            PATHS "${ROCKS_CLANG_RESOURCE}/lib/x86_64-unknown-windows-msvc" "${ROCKS_CLANG_RESOURCE}/lib/x86_64-pc-windows-msvc" "${ROCKS_CLANG_RESOURCE}/lib/windows" NO_DEFAULT_PATH REQUIRED)
        find_library(ROCKS_ASAN_THUNK NAMES clang_rt.asan_dynamic_runtime_thunk clang_rt.asan_dynamic_runtime_thunk-x86_64
            PATHS "${ROCKS_CLANG_RESOURCE}/lib/x86_64-unknown-windows-msvc" "${ROCKS_CLANG_RESOURCE}/lib/x86_64-pc-windows-msvc" "${ROCKS_CLANG_RESOURCE}/lib/windows" NO_DEFAULT_PATH REQUIRED)
        target_link_libraries(rocks_options INTERFACE "${ROCKS_ASAN_IMPORT}")
        target_link_options(rocks_options INTERFACE /INCREMENTAL:NO /INCLUDE:__asan_seh_interceptor "/WHOLEARCHIVE:${ROCKS_ASAN_THUNK}")
    else()
        target_compile_options(rocks_options INTERFACE -fsanitize=address -fno-omit-frame-pointer)
        target_link_options(rocks_options INTERFACE -fsanitize=address)
    endif()
endif()

add_library(rocks_base INTERFACE)
target_include_directories(rocks_base INTERFACE Engine/Base/Public)
target_link_libraries(rocks_base INTERFACE rocks_options)

file(GLOB_RECURSE ROCKS_PLATFORM_SOURCES CONFIGURE_DEPENDS
    Engine/PlatformBase/Private/${ROCKS_PLATFORM_DIRECTORY}/*.cpp)
file(GLOB_RECURSE ROCKS_CORE_SOURCES CONFIGURE_DEPENDS Engine/Core/Private/*.cpp)
file(GLOB_RECURSE ROCKS_TASK_SOURCES CONFIGURE_DEPENDS Engine/Task/Private/*.cpp)
file(GLOB_RECURSE ROCKS_OBJECT_SOURCES CONFIGURE_DEPENDS Engine/ObjectRuntime/Private/*.cpp)

add_library(rocks_platform STATIC ${ROCKS_PLATFORM_SOURCES})
target_include_directories(rocks_platform PUBLIC Engine/PlatformBase/Public)
target_link_libraries(rocks_platform PUBLIC rocks_base)

add_library(rocks_core STATIC ${ROCKS_CORE_SOURCES})
target_include_directories(rocks_core PUBLIC Engine/Core/Public)
target_link_libraries(rocks_core PUBLIC rocks_platform)

add_library(rocks_meta INTERFACE)
target_include_directories(rocks_meta INTERFACE Engine/MetaSchema/Public)
target_link_libraries(rocks_meta INTERFACE rocks_core)

add_library(rocks_task STATIC ${ROCKS_TASK_SOURCES})
target_include_directories(rocks_task PUBLIC Engine/Task/Public)
target_link_libraries(rocks_task PUBLIC rocks_core)

if(ROCKS_BUILD_OBJECT)
add_library(rocks_object STATIC ${ROCKS_OBJECT_SOURCES})
target_include_directories(rocks_object PUBLIC Engine/ObjectRuntime/Public)
target_link_libraries(rocks_object PUBLIC rocks_meta rocks_task)

endif()

file(GLOB_RECURSE ROCKS_BOOTSTRAP_TEST_SOURCES CONFIGURE_DEPENDS
    Tests/Foundation/*.cpp Tests/Core/*.cpp Tests/Platform/*.cpp)
add_executable(CoreFoundationTests ${ROCKS_BOOTSTRAP_TEST_SOURCES})
target_include_directories(CoreFoundationTests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} Tests/Support)
target_compile_definitions(CoreFoundationTests PRIVATE ROCKS_TESTING=1)
target_link_libraries(CoreFoundationTests PRIVATE rocks_task)
set(ROCKS_TEST_EXECUTABLE CoreFoundationTests)

if(ROCKS_BUILD_OBJECT)
file(GLOB_RECURSE ROCKS_TEST_SOURCES CONFIGURE_DEPENDS
    Tests/Foundation/*.cpp Tests/Core/*.cpp Tests/Platform/*.cpp Tests/Object/*.cpp)
add_executable(FoundationTests ${ROCKS_TEST_SOURCES})
target_include_directories(FoundationTests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} Tests/Support)
target_compile_definitions(FoundationTests PRIVATE ROCKS_TESTING=1)
target_link_libraries(FoundationTests PRIVATE rocks_object)

set(ROCKS_TEST_EXECUTABLE FoundationTests)
endif()

# Each public header is compiled as the first and only include in its translation unit.
file(GLOB_RECURSE ROCKS_PUBLIC_HEADERS CONFIGURE_DEPENDS Engine/*/Public/*.h)
set(ROCKS_HEADER_SOURCES)
foreach(header IN LISTS ROCKS_PUBLIC_HEADERS)
    if(NOT ROCKS_BUILD_OBJECT AND header MATCHES "/(ObjectRuntime|MetaSchema)/")
        continue()
    endif()
    string(REGEX REPLACE ".*/Public/" "" include_path "${header}")
    string(SHA1 key "${include_path}")
    set(source "${CMAKE_CURRENT_BINARY_DIR}/HeaderChecks/${key}.cpp")
    file(GENERATE OUTPUT "${source}" CONTENT "#include <${include_path}>\n")
    list(APPEND ROCKS_HEADER_SOURCES "${source}")
endforeach()
add_library(rocks_header_checks OBJECT ${ROCKS_HEADER_SOURCES})
if(ROCKS_BUILD_OBJECT)
    target_link_libraries(rocks_header_checks PRIVATE rocks_object)
else()
    target_link_libraries(rocks_header_checks PRIVATE rocks_task)
endif()

find_package(Python3 REQUIRED COMPONENTS Interpreter)
enable_testing()
add_test(NAME foundation COMMAND ${ROCKS_TEST_EXECUTABLE} --seed 7351 --json ${CMAKE_CURRENT_BINARY_DIR}/foundation-report.json)
add_test(NAME harness_negative_controls
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/Tests/Integration/test_harness.py $<TARGET_FILE:${ROCKS_TEST_EXECUTABLE}>)
add_test(NAME runtime_stl_audit
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/Build/audit_runtime.py ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME scaffold_transaction
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/Tests/Integration/test_scaffold.py)
add_custom_target(format-check
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/Build/format.py --check
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})

add_test(NAME build_evidence_contract
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/Tests/Integration/test_build_evidence.py)

if(ROCKS_ASAN)
    add_test(NAME asan_negative_control
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/Tests/Integration/test_asan_probe.py $<TARGET_FILE:${ROCKS_TEST_EXECUTABLE}>)
endif()
