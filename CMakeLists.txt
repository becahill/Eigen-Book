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
option(EIGENBOOK_BUILD_FUZZERS "Build Clang libFuzzer harnesses" OFF)
option(EIGENBOOK_BUILD_PYTHON "Build the eigenbook._eigenbook Python extension" OFF)
option(EIGENBOOK_ENABLE_ASAN "Enable AddressSanitizer on supported compilers" OFF)
option(EIGENBOOK_ENABLE_UBSAN "Enable UndefinedBehaviorSanitizer on supported compilers" OFF)
option(EIGENBOOK_ENABLE_CLANG_TIDY
    "Run focused clang-tidy checks over Eigen-Book production headers" OFF)
set(EIGENBOOK_CLANG_TIDY_EXECUTABLE "clang-tidy" CACHE STRING
    "clang-tidy executable used when EIGENBOOK_ENABLE_CLANG_TIDY is ON")
set(EIGENBOOK_BENCHMARK_OPERATIONS "50000" CACHE STRING
    "Operations per workload in the run_benchmarks target")
set(EIGENBOOK_BENCHMARK_ITERATIONS "1" CACHE STRING
    "Complete workload iterations in the run_benchmarks target")

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

if(EIGENBOOK_ENABLE_CLANG_TIDY)
    find_program(EIGENBOOK_CLANG_TIDY
        NAMES "${EIGENBOOK_CLANG_TIDY_EXECUTABLE}"
        REQUIRED)

    set(EIGENBOOK_CLANG_TIDY_CHECKS
        "clang-analyzer-core.*"
        "clang-analyzer-cplusplus.*"
        "clang-analyzer-deadcode.*"
        "bugprone-dangling-handle"
        "bugprone-misplaced-widening-cast"
        "bugprone-narrowing-conversions"
        "bugprone-sizeof-expression"
        "bugprone-undefined-memory-manipulation"
        "bugprone-use-after-move"
        "performance-for-range-copy"
        "performance-move-const-arg"
        "performance-no-automatic-move"
        "performance-unnecessary-copy-initialization"
        "performance-unnecessary-value-param")
    list(JOIN EIGENBOOK_CLANG_TIDY_CHECKS "," EIGENBOOK_CLANG_TIDY_CHECKS_ARG)

    add_library(eigenbook_static_analysis OBJECT
        analysis/clang_tidy.cpp)
    target_link_libraries(eigenbook_static_analysis PRIVATE EigenBook::eigenbook)
    set_target_properties(eigenbook_static_analysis PROPERTIES
        CXX_CLANG_TIDY
            "${EIGENBOOK_CLANG_TIDY};--checks=-*,${EIGENBOOK_CLANG_TIDY_CHECKS_ARG};--warnings-as-errors=*;--header-filter=^${CMAKE_CURRENT_SOURCE_DIR}/include/")
endif()

if(EIGENBOOK_BUILD_PYTHON)
    find_package(Python 3.10 COMPONENTS Interpreter Development.Module REQUIRED)
    set(PYBIND11_FINDPYTHON ON)
    find_package(pybind11 3.0 CONFIG REQUIRED)

    pybind11_add_module(_eigenbook MODULE
        src/bindings.cpp)
    target_link_libraries(_eigenbook PRIVATE EigenBook::eigenbook)
    target_compile_definitions(_eigenbook PRIVATE
        EIGENBOOK_PYTHON_VERSION="${PROJECT_VERSION}"
        EIGENBOOK_PYTHON_COMPILER="${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}"
        EIGENBOOK_PYTHON_BUILD_TYPE="$<CONFIG>")

    install(
        TARGETS _eigenbook
        LIBRARY DESTINATION eigenbook COMPONENT python
        RUNTIME DESTINATION eigenbook COMPONENT python)
endif()

if(EIGENBOOK_BUILD_TESTS)
    if(BUILD_TESTING)
        add_executable(eigenbook_tests
            tests/test_eigenbook.cpp)
        target_link_libraries(eigenbook_tests PRIVATE EigenBook::eigenbook)

        add_executable(eigenbook_no_allocation_tests
            tests/AllocationTracker.cpp
            tests/test_no_allocations.cpp)
        target_link_libraries(eigenbook_no_allocation_tests PRIVATE EigenBook::eigenbook)

        add_test(NAME eigenbook_tests COMMAND eigenbook_tests)
        add_test(NAME eigenbook_no_allocation_tests COMMAND eigenbook_no_allocation_tests)
        set_tests_properties(
            eigenbook_no_allocation_tests
            PROPERTIES LABELS allocation)
    endif()
endif()

if(EIGENBOOK_BUILD_BENCHMARKS)
    if(CMAKE_BUILD_TYPE STREQUAL "Release")
        set(EIGENBOOK_BENCHMARK_OPTIMIZATION_FLAGS
            "${CMAKE_CXX_FLAGS} ${CMAKE_CXX_FLAGS_RELEASE}")
        if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|AppleClang|GNU")
            string(APPEND EIGENBOOK_BENCHMARK_OPTIMIZATION_FLAGS " -O3 -march=native")
        endif()
    else()
        set(EIGENBOOK_BENCHMARK_OPTIMIZATION_FLAGS
            "none configured for ${CMAKE_BUILD_TYPE}")
    endif()
    string(STRIP "${EIGENBOOK_BENCHMARK_OPTIMIZATION_FLAGS}"
        EIGENBOOK_BENCHMARK_OPTIMIZATION_FLAGS)

    configure_file(
        benchmarks/BenchmarkBuildInfo.hpp.in
        generated/BenchmarkBuildInfo.hpp
        @ONLY)

    add_executable(eigenbook_bench
        benchmarks/bench.cpp)
    target_link_libraries(eigenbook_bench PRIVATE EigenBook::eigenbook)
    target_include_directories(eigenbook_bench PRIVATE
        "${CMAKE_CURRENT_BINARY_DIR}/generated")

    add_custom_target(run_benchmarks
        COMMAND $<TARGET_FILE:eigenbook_bench>
            --operations "${EIGENBOOK_BENCHMARK_OPERATIONS}"
            --iterations "${EIGENBOOK_BENCHMARK_ITERATIONS}"
        DEPENDS eigenbook_bench
        COMMENT "Running reproducible Eigen-Book benchmark workloads"
        USES_TERMINAL
        VERBATIM)
endif()

if(EIGENBOOK_BUILD_EXAMPLES)
    add_executable(eigenbook_basic_usage
        examples/basic_usage.cpp)
    target_link_libraries(eigenbook_basic_usage PRIVATE EigenBook::eigenbook)

    add_executable(eigenbook_snapshot_usage
        examples/snapshot_usage.cpp)
    target_link_libraries(eigenbook_snapshot_usage PRIVATE EigenBook::eigenbook)

    add_executable(eigenbook_replay_usage
        examples/replay_usage.cpp)
    target_link_libraries(eigenbook_replay_usage PRIVATE EigenBook::eigenbook)

    add_executable(eigenbook_journal_usage
        examples/journal_usage.cpp)
    target_link_libraries(eigenbook_journal_usage PRIVATE EigenBook::eigenbook)
endif()

if(EIGENBOOK_BUILD_FUZZERS)
    if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang|AppleClang")
        message(FATAL_ERROR "EIGENBOOK_BUILD_FUZZERS requires Clang with libFuzzer support")
    endif()

    include(CheckCXXSourceCompiles)
    set(CMAKE_REQUIRED_FLAGS_SAVE "${CMAKE_REQUIRED_FLAGS}")
    set(CMAKE_REQUIRED_FLAGS "${CMAKE_REQUIRED_FLAGS} -fsanitize=fuzzer")
    check_cxx_source_compiles(
        "#include <cstddef>\n#include <cstdint>\nextern \"C\" int LLVMFuzzerTestOneInput(const std::uint8_t*, std::size_t) { return 0; }"
        EIGENBOOK_HAS_LIBFUZZER)
    set(CMAKE_REQUIRED_FLAGS "${CMAKE_REQUIRED_FLAGS_SAVE}")
    unset(CMAKE_REQUIRED_FLAGS_SAVE)
    if(NOT EIGENBOOK_HAS_LIBFUZZER)
        message(FATAL_ERROR
            "EIGENBOOK_BUILD_FUZZERS requires a Clang installation that includes the libFuzzer runtime")
    endif()

    function(eigenbook_add_fuzzer target source)
        add_executable(${target} ${source})
        target_link_libraries(${target} PRIVATE EigenBook::eigenbook)
        target_compile_options(${target} PRIVATE
            -fsanitize=fuzzer,address,undefined
            -fno-omit-frame-pointer)
        target_link_options(${target} PRIVATE
            -fsanitize=fuzzer,address,undefined)
    endfunction()

    eigenbook_add_fuzzer(eigenbook_command_fuzzer fuzz/fuzz_command_decode.cpp)
    eigenbook_add_fuzzer(eigenbook_snapshot_fuzzer fuzz/fuzz_snapshot_restore.cpp)
    eigenbook_add_fuzzer(eigenbook_stateful_fuzzer fuzz/fuzz_stateful_differential.cpp)

    add_executable(eigenbook_generate_fuzz_corpus
        fuzz/generate_corpus.cpp)
    target_link_libraries(eigenbook_generate_fuzz_corpus PRIVATE EigenBook::eigenbook)

    set(EIGENBOOK_FUZZ_CORPUS_DIR "${CMAKE_CURRENT_BINARY_DIR}/fuzz-corpus")
    set(EIGENBOOK_COMMAND_CORPUS_DIR "${EIGENBOOK_FUZZ_CORPUS_DIR}/command")
    set(EIGENBOOK_SNAPSHOT_CORPUS_DIR "${EIGENBOOK_FUZZ_CORPUS_DIR}/snapshot")
    set(EIGENBOOK_STATEFUL_CORPUS_DIR "${EIGENBOOK_FUZZ_CORPUS_DIR}/stateful")
    set(EIGENBOOK_FUZZ_ARTIFACT_DIR "${CMAKE_CURRENT_BINARY_DIR}/fuzz-artifacts")
    set(EIGENBOOK_FUZZ_CORPUS_FILES
        "${EIGENBOOK_COMMAND_CORPUS_DIR}/add"
        "${EIGENBOOK_COMMAND_CORPUS_DIR}/replace"
        "${EIGENBOOK_SNAPSHOT_CORPUS_DIR}/dense"
        "${EIGENBOOK_SNAPSHOT_CORPUS_DIR}/sparse"
        "${EIGENBOOK_STATEFUL_CORPUS_DIR}/command-classes"
        "${EIGENBOOK_STATEFUL_CORPUS_DIR}/capacity-and-invalid"
        "${EIGENBOOK_STATEFUL_CORPUS_DIR}/snapshot-replay"
        "${EIGENBOOK_STATEFUL_CORPUS_DIR}/quantity-overflow")

    add_custom_command(
        OUTPUT ${EIGENBOOK_FUZZ_CORPUS_FILES}
        COMMAND ${CMAKE_COMMAND} -E make_directory
            "${EIGENBOOK_COMMAND_CORPUS_DIR}"
            "${EIGENBOOK_SNAPSHOT_CORPUS_DIR}"
            "${EIGENBOOK_STATEFUL_CORPUS_DIR}"
            "${EIGENBOOK_FUZZ_ARTIFACT_DIR}"
        COMMAND $<TARGET_FILE:eigenbook_generate_fuzz_corpus> "${EIGENBOOK_FUZZ_CORPUS_DIR}"
        DEPENDS eigenbook_generate_fuzz_corpus
        VERBATIM)
    add_custom_target(eigenbook_fuzz_corpus ALL
        DEPENDS ${EIGENBOOK_FUZZ_CORPUS_FILES})
    add_dependencies(eigenbook_command_fuzzer eigenbook_fuzz_corpus)
    add_dependencies(eigenbook_snapshot_fuzzer eigenbook_fuzz_corpus)
    add_dependencies(eigenbook_stateful_fuzzer eigenbook_fuzz_corpus)

    if(BUILD_TESTING)
        file(MAKE_DIRECTORY "${EIGENBOOK_FUZZ_ARTIFACT_DIR}")
        add_test(NAME eigenbook_fuzz_command_smoke
            COMMAND eigenbook_command_fuzzer
                -runs=256
                -seed=1
                -max_len=4096
                "-artifact_prefix=${EIGENBOOK_FUZZ_ARTIFACT_DIR}/"
                "${EIGENBOOK_COMMAND_CORPUS_DIR}")
        add_test(NAME eigenbook_fuzz_snapshot_smoke
            COMMAND eigenbook_snapshot_fuzzer
                -runs=256
                -seed=1
                -max_len=4096
                "-artifact_prefix=${EIGENBOOK_FUZZ_ARTIFACT_DIR}/"
                "${EIGENBOOK_SNAPSHOT_CORPUS_DIR}")
        add_test(NAME eigenbook_fuzz_stateful_smoke
            COMMAND eigenbook_stateful_fuzzer
                -runs=512
                -seed=1
                -max_len=4096
                "-artifact_prefix=${EIGENBOOK_FUZZ_ARTIFACT_DIR}/"
                "${EIGENBOOK_STATEFUL_CORPUS_DIR}")
        set_tests_properties(
            eigenbook_fuzz_command_smoke
            eigenbook_fuzz_snapshot_smoke
            eigenbook_fuzz_stateful_smoke
            PROPERTIES TIMEOUT 60 LABELS fuzz)
    endif()
endif()
