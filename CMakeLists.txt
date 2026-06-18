cmake_minimum_required(VERSION 3.20)

project(EigenBook
    VERSION 0.1.0
    DESCRIPTION "C++20 low-latency limit order book and matching engine"
    LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(EIGENBOOK_BUILD_TESTS "Build Eigen-Book unit tests" ON)
option(EIGENBOOK_BUILD_BENCHMARKS "Build Eigen-Book benchmark executable" ON)
option(EIGENBOOK_BUILD_EXAMPLES "Build Eigen-Book usage examples" ON)
option(EIGENBOOK_ENABLE_ASAN "Enable AddressSanitizer on supported compilers" OFF)
option(EIGENBOOK_ENABLE_UBSAN "Enable UndefinedBehaviorSanitizer on supported compilers" OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build type" FORCE)
endif()
set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS Debug Release RelWithDebInfo MinSizeRel)

add_library(eigenbook INTERFACE)
add_library(EigenBook::eigenbook ALIAS eigenbook)

target_compile_features(eigenbook INTERFACE cxx_std_20)
target_include_directories(eigenbook INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)

if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|AppleClang|GNU")
    target_compile_options(eigenbook INTERFACE
        -Wall
        -Wextra
        -Werror
        $<$<CONFIG:Release>:-O3>
        $<$<CONFIG:Release>:-march=native>)

    set(EIGENBOOK_SANITIZERS "")
    if(EIGENBOOK_ENABLE_ASAN)
        list(APPEND EIGENBOOK_SANITIZERS address)
    endif()
    if(EIGENBOOK_ENABLE_UBSAN)
        list(APPEND EIGENBOOK_SANITIZERS undefined)
    endif()

    if(EIGENBOOK_SANITIZERS)
        list(JOIN EIGENBOOK_SANITIZERS "," EIGENBOOK_SANITIZER_FLAGS)
        target_compile_options(eigenbook INTERFACE
            -fsanitize=${EIGENBOOK_SANITIZER_FLAGS}
            -fno-omit-frame-pointer)
        target_link_options(eigenbook INTERFACE
            -fsanitize=${EIGENBOOK_SANITIZER_FLAGS})
    endif()
elseif(EIGENBOOK_ENABLE_ASAN OR EIGENBOOK_ENABLE_UBSAN)
    message(WARNING "Sanitizers are only configured for Clang/GNU-like compilers")
endif()

include(CTest)

if(EIGENBOOK_BUILD_TESTS)
    if(BUILD_TESTING)
        add_executable(eigenbook_tests
            tests/test_eigenbook.cpp)
        target_link_libraries(eigenbook_tests PRIVATE EigenBook::eigenbook)

        add_test(NAME eigenbook_tests COMMAND eigenbook_tests)
    endif()
endif()

if(EIGENBOOK_BUILD_BENCHMARKS)
    add_executable(eigenbook_bench
        benchmarks/bench.cpp)
    target_link_libraries(eigenbook_bench PRIVATE EigenBook::eigenbook)
endif()

if(EIGENBOOK_BUILD_EXAMPLES)
    add_executable(eigenbook_basic_usage
        examples/basic_usage.cpp)
    target_link_libraries(eigenbook_basic_usage PRIVATE EigenBook::eigenbook)

    add_executable(eigenbook_snapshot_usage
        examples/snapshot_usage.cpp)
    target_link_libraries(eigenbook_snapshot_usage PRIVATE EigenBook::eigenbook)
endif()
