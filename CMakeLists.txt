cmake_minimum_required(VERSION 3.20)

project(
    tinydb
    VERSION 1.0.0
    LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(TINYDB_BUILD_TESTS "Build the tinydb test suite" ON)

if(TINYDB_BUILD_TESTS)

    add_subdirectory(external/googletest)

    enable_testing()

    add_executable(tinydb_tests tests/test.cpp)

    target_include_directories(tinydb_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

    target_link_libraries(tinydb_tests PRIVATE GTest::gtest_main)

    include(GoogleTest)
    gtest_discover_tests(tinydb_tests)

endif()

