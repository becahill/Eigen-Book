cmake_minimum_required(VERSION 3.20)

project(EigenBookConsumerSmoke LANGUAGES CXX)

find_package(EigenBook CONFIG REQUIRED)

add_executable(eigenbook_consumer_smoke
    main.cpp)
target_link_libraries(eigenbook_consumer_smoke PRIVATE EigenBook::eigenbook)

add_custom_target(run_eigenbook_consumer_smoke
    COMMAND $<TARGET_FILE:eigenbook_consumer_smoke>
    DEPENDS eigenbook_consumer_smoke
    VERBATIM)
