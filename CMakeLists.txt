cmake_minimum_required(VERSION 3.25)

# APPLE에는 iOS 등도 포함되므로 대상 OS 이름으로 지원 범위를 명시한다.
function(rocks_select_platform)
    if(CMAKE_SYSTEM_NAME STREQUAL "Windows")
        set(ROCKS_PLATFORM_DIRECTORY Win64 PARENT_SCOPE)
    elseif(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
        set(ROCKS_PLATFORM_DIRECTORY Mac PARENT_SCOPE)
    else()
        message(FATAL_ERROR "Unsupported target system '${CMAKE_SYSTEM_NAME}': Rocks Foundation supports Windows and macOS only")
    endif()
endfunction()

# 명령행 대상은 컴파일러 탐색보다 먼저 거부한다. toolchain 파일이 project() 중에
# 설정하는 대상도 project() 이후 다시 검사한다.
if(DEFINED CMAKE_SYSTEM_NAME AND NOT CMAKE_SYSTEM_NAME STREQUAL "")
    rocks_select_platform()
endif()

project(RocksFoundation VERSION 0.1.0 LANGUAGES CXX)
rocks_select_platform()
message(STATUS "Rocks target system: ${CMAKE_SYSTEM_NAME} (${ROCKS_PLATFORM_DIRECTORY})")

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
option(ROCKS_BUILD_OBJECT "Build Object integration after Core" ON)
option(ROCKS_ASAN "Enable AddressSanitizer for the CPU Foundation" OFF)

add_library(rocks_options INTERFACE)
if(MSVC)
    string(REPLACE "/EHsc" "" CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS}")
    target_compile_options(rocks_options INTERFACE /W4 /WX /GR- /EHs-c- /Zc:__cplusplus)
    target_compile_definitions(rocks_options INTERFACE NOMINMAX WIN32_LEAN_AND_MEAN)
else()
    target_compile_options(rocks_options INTERFACE -Wall -Wextra -Wpedantic -Werror -fno-exceptions -fno-rtti)
endif()
if(ROCKS_PLATFORM_DIRECTORY STREQUAL "Win64")
    target_compile_definitions(rocks_options INTERFACE ROCKS_PLATFORM_WINDOWS=1)
else()
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
    Engine/PlatformBase/Private/${ROCKS_PLATFORM_DIRECTORY}/*.cpp
    Engine/PlatformBase/Private/Common/*.cpp)
file(GLOB_RECURSE ROCKS_CORE_SOURCES CONFIGURE_DEPENDS Engine/Core/Private/*.cpp)
file(GLOB_RECURSE ROCKS_TASK_SOURCES CONFIGURE_DEPENDS Engine/Task/Private/*.cpp)
file(GLOB_RECURSE ROCKS_LOCALIZATION_SOURCES CONFIGURE_DEPENDS Engine/Localization/Private/*.cpp)
file(GLOB_RECURSE ROCKS_SERIALIZATION_SOURCES CONFIGURE_DEPENDS Engine/Serialization/Private/*.cpp)
file(GLOB_RECURSE ROCKS_OBJECT_SERIALIZATION_SOURCES CONFIGURE_DEPENDS Engine/ObjectSerialization/Private/*.cpp)
file(GLOB_RECURSE ROCKS_OBJECT_SOURCES CONFIGURE_DEPENDS Engine/ObjectRuntime/Private/*.cpp)

add_library(rocks_platform STATIC ${ROCKS_PLATFORM_SOURCES})
target_include_directories(rocks_platform PUBLIC Engine/PlatformBase/Public)
target_link_libraries(rocks_platform PUBLIC rocks_base)

add_library(rocks_core STATIC ${ROCKS_CORE_SOURCES})
target_include_directories(rocks_core PUBLIC Engine/Core/Public)
target_link_libraries(rocks_core PUBLIC rocks_platform)

# Localization is a Core consumer; Core and Object do not depend on display text.
add_library(rocks_localization STATIC ${ROCKS_LOCALIZATION_SOURCES})
target_include_directories(rocks_localization PUBLIC Engine/Localization/Public)
target_link_libraries(rocks_localization PUBLIC rocks_core)

add_library(rocks_serialization STATIC ${ROCKS_SERIALIZATION_SOURCES})
target_include_directories(rocks_serialization PUBLIC Engine/Serialization/Public)
target_link_libraries(rocks_serialization PUBLIC rocks_core)

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

add_library(rocks_object_serialization STATIC ${ROCKS_OBJECT_SERIALIZATION_SOURCES})
target_include_directories(rocks_object_serialization PUBLIC Engine/ObjectSerialization/Public)
target_link_libraries(rocks_object_serialization PUBLIC rocks_object rocks_serialization)


endif()

file(GLOB_RECURSE ROCKS_HOST_TEST_SOURCES CONFIGURE_DEPENDS
    Tests/Support/Private/${ROCKS_PLATFORM_DIRECTORY}/*.cpp)
if(ROCKS_ASAN)
    list(APPEND ROCKS_HOST_TEST_SOURCES Tests/Support/Private/Asan/Enabled.cpp)
else()
    list(APPEND ROCKS_HOST_TEST_SOURCES Tests/Support/Private/Asan/Disabled.cpp)
endif()

file(GLOB_RECURSE ROCKS_BOOTSTRAP_TEST_SOURCES CONFIGURE_DEPENDS
    Tests/Foundation/*.cpp Tests/Core/*.cpp Tests/Platform/*.cpp Tests/Localization/*.cpp Tests/Serialization/*.cpp)
add_executable(CoreFoundationTests ${ROCKS_BOOTSTRAP_TEST_SOURCES} ${ROCKS_HOST_TEST_SOURCES})
target_include_directories(CoreFoundationTests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} Tests/Support)
target_compile_definitions(CoreFoundationTests PRIVATE ROCKS_TESTING=1)
target_link_libraries(CoreFoundationTests PRIVATE rocks_task rocks_localization rocks_serialization)
set(ROCKS_TEST_EXECUTABLE CoreFoundationTests)

if(ROCKS_BUILD_OBJECT)
file(GLOB_RECURSE ROCKS_TEST_SOURCES CONFIGURE_DEPENDS
    Tests/Foundation/*.cpp Tests/Core/*.cpp Tests/Platform/*.cpp Tests/Object/*.cpp Tests/Localization/*.cpp Tests/Serialization/*.cpp Tests/Reflection/*.cpp Tests/ObjectSerialization/*.cpp)
add_executable(FoundationTests ${ROCKS_TEST_SOURCES} ${ROCKS_HOST_TEST_SOURCES})
target_include_directories(FoundationTests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} Tests/Support)
target_compile_definitions(FoundationTests PRIVATE ROCKS_TESTING=1)
target_link_libraries(FoundationTests PRIVATE rocks_object_serialization rocks_localization)

set(ROCKS_TEST_EXECUTABLE FoundationTests)
endif()

# Each public header is compiled as the first and only include in its translation unit.
file(GLOB_RECURSE ROCKS_PUBLIC_HEADERS CONFIGURE_DEPENDS Engine/*/Public/*.h)
set(ROCKS_HEADER_SOURCES)
foreach(header IN LISTS ROCKS_PUBLIC_HEADERS)
    if(NOT ROCKS_BUILD_OBJECT AND header MATCHES "/(ObjectRuntime|ObjectSerialization|MetaSchema)/")
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
    target_link_libraries(rocks_header_checks PRIVATE rocks_object_serialization rocks_localization)
else()
    target_link_libraries(rocks_header_checks PRIVATE rocks_task rocks_localization rocks_serialization)
endif()

find_package(Python3 REQUIRED COMPONENTS Interpreter)
enable_testing()

if(ROCKS_BUILD_OBJECT)
    set(ROCKS_GENERATED_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/Generated")
    set(ROCKS_REFLECTION_FIXTURE "${ROCKS_GENERATED_DIRECTORY}/ReflectionFixture.generated.h")
    if(DEFINED ENV{ROCKS_CLANG_FORMAT})
        set(ROCKS_META_FORMATTER "$ENV{ROCKS_CLANG_FORMAT}")
    else()
        find_program(ROCKS_META_FORMATTER NAMES clang-format REQUIRED)
    endif()
    set(ROCKS_META_FLAGS --clang-arg=-DROCKS_TESTING=1)
    if(WIN32)
        list(APPEND ROCKS_META_FLAGS --clang-arg=-DROCKS_PLATFORM_WINDOWS=1
            --clang-arg=-DNOMINMAX --clang-arg=-DWIN32_LEAN_AND_MEAN)
    else()
        list(APPEND ROCKS_META_FLAGS --clang-arg=-DROCKS_PLATFORM_MAC=1)
    endif()
    if(ROCKS_ASAN)
        if(MSVC)
            list(APPEND ROCKS_META_FLAGS --clang-arg=/fsanitize=address)
        else()
            list(APPEND ROCKS_META_FLAGS --clang-arg=-fsanitize=address)
        endif()
    endif()
    set(ROCKS_META_COMMAND ${Python3_EXECUTABLE}
        ${CMAKE_CURRENT_SOURCE_DIR}/Tools/MetaCompiler/metacompiler.py
        --input ${CMAKE_CURRENT_SOURCE_DIR}/Tests/Reflection/ReflectionFixture.h
        --include Tests/Reflection/ReflectionFixture.h
        --output ${ROCKS_REFLECTION_FIXTURE}
        --compiler ${CMAKE_CXX_COMPILER} ${ROCKS_META_FLAGS})
    add_custom_command(OUTPUT ${ROCKS_REFLECTION_FIXTURE}
        COMMAND ${ROCKS_META_COMMAND}
        DEPENDS Tools/MetaCompiler/metacompiler.py Build/audit_warning_policy.py Build/build.py
            Tests/Reflection/ReflectionFixture.h ${ROCKS_PUBLIC_HEADERS}
            .clang-format .editorconfig "${ROCKS_META_FORMATTER}"
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
        COMMENT "Generating Clang-validated Rocks reflection metadata"
        VERBATIM)
    add_custom_target(rocks_generated_metadata DEPENDS ${ROCKS_REFLECTION_FIXTURE})
    add_dependencies(FoundationTests rocks_generated_metadata)
    add_dependencies(rocks_header_checks rocks_generated_metadata)
    target_include_directories(FoundationTests PRIVATE ${ROCKS_GENERATED_DIRECTORY})
    target_include_directories(rocks_header_checks PRIVATE
        ${ROCKS_GENERATED_DIRECTORY} ${CMAKE_CURRENT_SOURCE_DIR})
    file(GENERATE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/HeaderChecks/generated_reflection.cpp"
        CONTENT "#include <ReflectionFixture.generated.h>\n")
    target_sources(rocks_header_checks PRIVATE
        "${CMAKE_CURRENT_BINARY_DIR}/HeaderChecks/generated_reflection.cpp")
    add_test(NAME generated_metadata_current COMMAND ${ROCKS_META_COMMAND} --check)
    set_tests_properties(generated_metadata_current PROPERTIES TIMEOUT 120)
    add_test(NAME metacompiler_negative_controls
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/Tests/Reflection/test_metacompiler.py
            --compiler ${CMAKE_CXX_COMPILER} ${ROCKS_META_FLAGS})
    set_tests_properties(metacompiler_negative_controls PROPERTIES TIMEOUT 180)
endif()

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

# A deadlock in a lifetime/concurrency fixture must fail the job instead of hanging CI.
set_tests_properties(foundation harness_negative_controls PROPERTIES TIMEOUT 120)
if(ROCKS_ASAN)
    set_tests_properties(asan_negative_control PROPERTIES TIMEOUT 30)
endif()

add_test(NAME platform_boundary_audit
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/Build/audit_platform_boundary.py ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME platform_boundary_negative_controls
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/Tests/Integration/test_platform_boundary.py)

# These probes inherit the production warning profile but never enter the default build.
# CTest proves that clean code compiles and an intentional warning is rejected.
file(GENERATE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/WarningChecks/clean.cpp"
    CONTENT "int RocksWarningProbe() noexcept { return 0; }\n")
file(GENERATE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/WarningChecks/warning.cpp"
    CONTENT "int RocksWarningProbe() noexcept { int unusedWarningProbe = 7; return 0; }\n")
add_library(rocks_warning_clean_probe OBJECT EXCLUDE_FROM_ALL
    "${CMAKE_CURRENT_BINARY_DIR}/WarningChecks/clean.cpp")
add_library(rocks_warning_reject_probe OBJECT EXCLUDE_FROM_ALL
    "${CMAKE_CURRENT_BINARY_DIR}/WarningChecks/warning.cpp")
target_link_libraries(rocks_warning_clean_probe PRIVATE rocks_options)
target_link_libraries(rocks_warning_reject_probe PRIVATE rocks_options)
add_test(NAME warning_policy
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/Tests/Integration/test_warning_policy.py
        ${CMAKE_CURRENT_BINARY_DIR} ${CMAKE_COMMAND})
set_tests_properties(warning_policy PROPERTIES TIMEOUT 120)
