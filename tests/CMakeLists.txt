cmake_minimum_required(VERSION 3.20)

add_executable(publisher_tests
    unit/test_config_loader.cpp
    unit/test_state_machine.cpp
    unit/test_metrics_store.cpp
    unit/test_backoff.cpp
)

target_link_libraries(publisher_tests PRIVATE publisher_lib)

target_include_directories(publisher_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/src
)

enable_testing()
add_test(NAME publisher_tests COMMAND publisher_tests)
