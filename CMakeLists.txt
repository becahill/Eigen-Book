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
option(EIGENBOOK_BUILD_PYTHON "Build the eigenbook_py Python extension" OFF)
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

if(EIGENBOOK_BUILD_PYTHON)
    include(FetchContent)

    find_package(Python 3.8 COMPONENTS Interpreter Development.Module NumPy REQUIRED)
    set(PYBIND11_FINDPYTHON ON)
    FetchContent_Declare(
        pybind11
        GIT_REPOSITORY https://github.com/pybind/pybind11.git
        GIT_TAG v3.0.4
        GIT_SHALLOW TRUE)
    FetchContent_MakeAvailable(pybind11)

    pybind11_add_module(eigenbook_py MODULE
        src/bindings.cpp)
    target_link_libraries(eigenbook_py PRIVATE EigenBook::eigenbook)

    if(EIGENBOOK_BUILD_TESTS AND BUILD_TESTING)
        add_test(
            NAME eigenbook_python_bindings
            COMMAND ${CMAKE_COMMAND} -E env
                "PYTHONPATH=$<TARGET_FILE_DIR:eigenbook_py>"
                ${Python_EXECUTABLE}
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_python_bindings.py")
        add_test(
            NAME eigenbook_numpy_event_buffer
            COMMAND ${CMAKE_COMMAND} -E env
                "PYTHONPATH=$<TARGET_FILE_DIR:eigenbook_py>"
                ${Python_EXECUTABLE}
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_numpy_event_buffer.py")
    endif()
endif()

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

    add_executable(eigenbook_replay_usage
        examples/replay_usage.cpp)
    target_link_libraries(eigenbook_replay_usage PRIVATE EigenBook::eigenbook)
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

    add_executable(eigenbook_generate_fuzz_corpus
        fuzz/generate_corpus.cpp)
    target_link_libraries(eigenbook_generate_fuzz_corpus PRIVATE EigenBook::eigenbook)

    set(EIGENBOOK_FUZZ_CORPUS_DIR "${CMAKE_CURRENT_BINARY_DIR}/fuzz-corpus")
    set(EIGENBOOK_COMMAND_CORPUS_DIR "${EIGENBOOK_FUZZ_CORPUS_DIR}/command")
    set(EIGENBOOK_SNAPSHOT_CORPUS_DIR "${EIGENBOOK_FUZZ_CORPUS_DIR}/snapshot")
    set(EIGENBOOK_FUZZ_CORPUS_FILES
        "${EIGENBOOK_COMMAND_CORPUS_DIR}/add"
        "${EIGENBOOK_COMMAND_CORPUS_DIR}/replace"
        "${EIGENBOOK_SNAPSHOT_CORPUS_DIR}/dense"
        "${EIGENBOOK_SNAPSHOT_CORPUS_DIR}/sparse")

    add_custom_command(
        OUTPUT ${EIGENBOOK_FUZZ_CORPUS_FILES}
        COMMAND ${CMAKE_COMMAND} -E make_directory
            "${EIGENBOOK_COMMAND_CORPUS_DIR}"
            "${EIGENBOOK_SNAPSHOT_CORPUS_DIR}"
        COMMAND $<TARGET_FILE:eigenbook_generate_fuzz_corpus> "${EIGENBOOK_FUZZ_CORPUS_DIR}"
        DEPENDS eigenbook_generate_fuzz_corpus
        VERBATIM)
    add_custom_target(eigenbook_fuzz_corpus ALL
        DEPENDS ${EIGENBOOK_FUZZ_CORPUS_FILES})
    add_dependencies(eigenbook_command_fuzzer eigenbook_fuzz_corpus)
    add_dependencies(eigenbook_snapshot_fuzzer eigenbook_fuzz_corpus)

    if(BUILD_TESTING)
        add_test(NAME eigenbook_fuzz_command_smoke
            COMMAND eigenbook_command_fuzzer
                -runs=256
                -max_len=4096
                "${EIGENBOOK_COMMAND_CORPUS_DIR}")
        add_test(NAME eigenbook_fuzz_snapshot_smoke
            COMMAND eigenbook_snapshot_fuzzer
                -runs=256
                -max_len=4096
                "${EIGENBOOK_SNAPSHOT_CORPUS_DIR}")
        set_tests_properties(
            eigenbook_fuzz_command_smoke
            eigenbook_fuzz_snapshot_smoke
            PROPERTIES TIMEOUT 60 LABELS fuzz)
    endif()
endif()
