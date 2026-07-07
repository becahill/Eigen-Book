if(NOT DEFINED EIGENBOOK_BUILD_DIR)
    message(FATAL_ERROR "EIGENBOOK_BUILD_DIR is required")
endif()
if(NOT DEFINED EIGENBOOK_CONSUMER_SOURCE_DIR)
    message(FATAL_ERROR "EIGENBOOK_CONSUMER_SOURCE_DIR is required")
endif()
if(NOT DEFINED EIGENBOOK_CONSUMER_BUILD_DIR)
    message(FATAL_ERROR "EIGENBOOK_CONSUMER_BUILD_DIR is required")
endif()
if(NOT DEFINED EIGENBOOK_INSTALL_PREFIX)
    message(FATAL_ERROR "EIGENBOOK_INSTALL_PREFIX is required")
endif()
if(NOT DEFINED EIGENBOOK_INSTALL_INCLUDEDIR)
    message(FATAL_ERROR "EIGENBOOK_INSTALL_INCLUDEDIR is required")
endif()
if(NOT DEFINED EIGENBOOK_INSTALL_CMAKEDIR)
    message(FATAL_ERROR "EIGENBOOK_INSTALL_CMAKEDIR is required")
endif()

if(NOT DEFINED EIGENBOOK_BUILD_CONFIG)
    set(EIGENBOOK_BUILD_CONFIG "")
endif()

file(REMOVE_RECURSE
    "${EIGENBOOK_INSTALL_PREFIX}"
    "${EIGENBOOK_CONSUMER_BUILD_DIR}")

set(install_args
    --install "${EIGENBOOK_BUILD_DIR}"
    --component development
    --prefix "${EIGENBOOK_INSTALL_PREFIX}")
if(NOT EIGENBOOK_BUILD_CONFIG STREQUAL "")
    list(APPEND install_args --config "${EIGENBOOK_BUILD_CONFIG}")
endif()

execute_process(
    COMMAND "${CMAKE_COMMAND}" ${install_args}
    RESULT_VARIABLE install_result)
if(NOT install_result EQUAL 0)
    message(FATAL_ERROR "EigenBook install failed with exit code ${install_result}")
endif()

set(installed_include_dir
    "${EIGENBOOK_INSTALL_PREFIX}/${EIGENBOOK_INSTALL_INCLUDEDIR}")
if(IS_ABSOLUTE "${EIGENBOOK_INSTALL_INCLUDEDIR}")
    set(installed_include_dir "${EIGENBOOK_INSTALL_INCLUDEDIR}")
endif()

set(installed_config_dir
    "${EIGENBOOK_INSTALL_PREFIX}/${EIGENBOOK_INSTALL_CMAKEDIR}")
if(IS_ABSOLUTE "${EIGENBOOK_INSTALL_CMAKEDIR}")
    set(installed_config_dir "${EIGENBOOK_INSTALL_CMAKEDIR}")
endif()

foreach(installed_file IN ITEMS
        "${installed_include_dir}/MatchingEngine.hpp"
        "${installed_config_dir}/EigenBookConfig.cmake"
        "${installed_config_dir}/EigenBookConfigVersion.cmake"
        "${installed_config_dir}/EigenBookTargets.cmake")
    if(NOT EXISTS "${installed_file}")
        message(FATAL_ERROR "Expected installed file is missing: ${installed_file}")
    endif()
endforeach()

set(configure_args
    -S "${EIGENBOOK_CONSUMER_SOURCE_DIR}"
    -B "${EIGENBOOK_CONSUMER_BUILD_DIR}"
    "-DCMAKE_PREFIX_PATH=${EIGENBOOK_INSTALL_PREFIX}")
if(NOT EIGENBOOK_BUILD_CONFIG STREQUAL "")
    list(APPEND configure_args "-DCMAKE_BUILD_TYPE=${EIGENBOOK_BUILD_CONFIG}")
endif()

execute_process(
    COMMAND "${CMAKE_COMMAND}" ${configure_args}
    RESULT_VARIABLE configure_result)
if(NOT configure_result EQUAL 0)
    message(FATAL_ERROR
        "EigenBook consumer configure failed with exit code ${configure_result}")
endif()

set(build_args
    --build "${EIGENBOOK_CONSUMER_BUILD_DIR}"
    --target run_eigenbook_consumer_smoke)
if(NOT EIGENBOOK_BUILD_CONFIG STREQUAL "")
    list(APPEND build_args --config "${EIGENBOOK_BUILD_CONFIG}")
endif()

execute_process(
    COMMAND "${CMAKE_COMMAND}" ${build_args}
    RESULT_VARIABLE build_result)
if(NOT build_result EQUAL 0)
    message(FATAL_ERROR
        "EigenBook consumer build/run failed with exit code ${build_result}")
endif()
