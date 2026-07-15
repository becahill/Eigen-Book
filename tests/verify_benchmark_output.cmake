cmake_minimum_required(VERSION 3.20)

foreach(required_variable IN ITEMS
        EIGENBOOK_BENCHMARK_EXECUTABLE
        EIGENBOOK_BENCHMARK_EXPECTED_CONFIG
        EIGENBOOK_BENCHMARK_FORMAT)
    if(NOT DEFINED ${required_variable} OR "${${required_variable}}" STREQUAL "")
        message(FATAL_ERROR "${required_variable} is required")
    endif()
endforeach()

execute_process(
    COMMAND "${EIGENBOOK_BENCHMARK_EXECUTABLE}"
        --operations 20
        --iterations 1
        --format "${EIGENBOOK_BENCHMARK_FORMAT}"
    RESULT_VARIABLE benchmark_result
    OUTPUT_VARIABLE benchmark_output
    ERROR_VARIABLE benchmark_error)
if(NOT benchmark_result EQUAL 0)
    message(FATAL_ERROR
        "benchmark ${EIGENBOOK_BENCHMARK_FORMAT} smoke failed (${benchmark_result})\n"
        "stdout:\n${benchmark_output}\n"
        "stderr:\n${benchmark_error}")
endif()

function(require_substring value description)
    string(FIND "${benchmark_output}" "${value}" match_index)
    if(match_index EQUAL -1)
        message(FATAL_ERROR
            "benchmark output is missing ${description}: ${value}\n${benchmark_output}")
    endif()
endfunction()

function(verify_configuration_flags compiler flags)
    if(EIGENBOOK_BENCHMARK_EXPECTED_CONFIG STREQUAL "Debug")
        if(flags MATCHES "(^|[ \t])-O3([ \t]|$)" OR flags MATCHES "-march=native")
            message(FATAL_ERROR
                "Debug benchmark metadata contains Release-only flags: ${flags}")
        endif()
    elseif(EIGENBOOK_BENCHMARK_EXPECTED_CONFIG STREQUAL "Release" AND
           compiler MATCHES "Clang|GNU")
        if(NOT flags MATCHES "(^|[ \t])-O3([ \t]|$)" OR
           NOT flags MATCHES "-march=native")
            message(FATAL_ERROR
                "Release benchmark metadata omits required optimization flags: ${flags}")
        endif()
    endif()
endfunction()

if(EIGENBOOK_BENCHMARK_FORMAT STREQUAL "text")
    require_substring(
        "Build type: ${EIGENBOOK_BENCHMARK_EXPECTED_CONFIG}"
        "the selected build configuration")
    require_substring("Git commit: " "Git commit provenance")
    require_substring("Git worktree (at configure): " "Git worktree provenance")
    require_substring("Clustered hash-map cancels" "the clustered cancellation workload")
    require_substring("unavailable" "unavailable small-sample percentiles")

    string(REGEX MATCH "Compiler: ([^\r\n]*)" compiler_line "${benchmark_output}")
    if(NOT compiler_line)
        message(FATAL_ERROR "benchmark text output omits compiler metadata")
    endif()
    set(compiler "${CMAKE_MATCH_1}")

    string(REGEX MATCH "Optimization flags: ([^\r\n]*)" flags_line "${benchmark_output}")
    if(NOT flags_line)
        message(FATAL_ERROR "benchmark text output omits optimization metadata")
    endif()
    set(flags "${CMAKE_MATCH_1}")
    verify_configuration_flags("${compiler}" "${flags}")
elseif(EIGENBOOK_BENCHMARK_FORMAT STREQUAL "json")
    string(JSON root_type ERROR_VARIABLE json_error TYPE "${benchmark_output}")
    if(NOT json_error STREQUAL "NOTFOUND" OR NOT root_type STREQUAL "OBJECT")
        message(FATAL_ERROR "invalid benchmark JSON: ${json_error}\n${benchmark_output}")
    endif()

    string(JSON schema GET "${benchmark_output}" schema)
    if(NOT schema STREQUAL "eigenbook.benchmark.v2")
        message(FATAL_ERROR "unexpected benchmark schema: ${schema}")
    endif()

    string(JSON build_type GET "${benchmark_output}" context build_type)
    if(NOT build_type STREQUAL EIGENBOOK_BENCHMARK_EXPECTED_CONFIG)
        message(FATAL_ERROR
            "benchmark reports ${build_type}, expected ${EIGENBOOK_BENCHMARK_EXPECTED_CONFIG}")
    endif()

    string(JSON compiler GET "${benchmark_output}" context compiler)
    string(JSON flags GET "${benchmark_output}" context optimization_flags)
    verify_configuration_flags("${compiler}" "${flags}")

    string(JSON git_commit GET "${benchmark_output}" context git_commit)
    if(git_commit STREQUAL "")
        message(FATAL_ERROR "benchmark JSON has an empty Git commit field")
    endif()
    string(JSON git_worktree GET "${benchmark_output}" context git_worktree)
    if(NOT git_worktree MATCHES "^(clean|dirty|unavailable)$")
        message(FATAL_ERROR "unexpected Git worktree state: ${git_worktree}")
    endif()

    string(JSON p50_type TYPE "${benchmark_output}" results 0 benchmarks 0 p50_ns)
    string(JSON p95_type TYPE "${benchmark_output}" results 0 benchmarks 0 p95_ns)
    string(JSON p99_type TYPE "${benchmark_output}" results 0 benchmarks 0 p99_ns)
    string(JSON latency_samples GET
        "${benchmark_output}" results 0 benchmarks 0 latency_samples)
    if(NOT p50_type STREQUAL "NULL" OR
       NOT p95_type STREQUAL "NULL" OR
       NOT p99_type STREQUAL "NULL" OR
       NOT latency_samples EQUAL 0)
        message(FATAL_ERROR
            "small-sample JSON percentiles must be null, got "
            "${p50_type}/${p95_type}/${p99_type} from ${latency_samples} samples")
    endif()

    string(JSON benchmark_count LENGTH "${benchmark_output}" results 0 benchmarks)
    math(EXPR last_benchmark "${benchmark_count} - 1")
    set(found_clustered_workload FALSE)
    foreach(index RANGE 0 ${last_benchmark})
        string(JSON scenario GET "${benchmark_output}" results 0 benchmarks ${index} scenario)
        if(scenario STREQUAL "Clustered hash-map cancels")
            set(found_clustered_workload TRUE)
            break()
        endif()
    endforeach()
    if(NOT found_clustered_workload)
        message(FATAL_ERROR "benchmark JSON omits the clustered cancellation workload")
    endif()
else()
    message(FATAL_ERROR "unsupported benchmark format: ${EIGENBOOK_BENCHMARK_FORMAT}")
endif()
