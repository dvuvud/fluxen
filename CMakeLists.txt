cmake_minimum_required(VERSION 3.20)

project(
    fluxen
    VERSION 1.0.0.4
    LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

include_directories(${CMAKE_CURRENT_SOURCE_DIR})

if(WIN32)
    add_definitions(-DNOMINMAX)
    add_definitions(-D_CRT_SECURE_NO_WARNINGS)
endif()

option(FLUXEN_BUILD_EXAMPLES "Build examples" ON)

if(FLUXEN_BUILD_EXAMPLES)
    add_executable(settings examples/settings.cpp)
    add_executable(cache examples/cache.cpp)
endif()

option(FLUXEN_BUILD_TESTS "Build the fluxen test suite" ON)

if(FLUXEN_BUILD_TESTS)
    add_subdirectory(external/googletest)
    enable_testing()
    add_executable(fluxen_tests tests/test.cpp)
    add_executable(fluxen_concurrency_tests tests/test_concurrent.cpp)
    target_link_libraries(fluxen_tests PRIVATE GTest::gtest_main)
    target_link_libraries(fluxen_concurrency_tests PRIVATE GTest::gtest_main)
    include(GoogleTest)
    gtest_discover_tests(fluxen_tests)
    gtest_discover_tests(fluxen_concurrency_tests)
endif()

option(FLUXEN_BUILD_BENCHMARKS "Build the fluxen benchmarks" ON)
if(FLUXEN_BUILD_BENCHMARKS)
    set(BENCHMARK_ENABLE_TESTING OFF CACHE BOOL "" FORCE)
    set(BENCHMARK_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
    add_subdirectory(external/googlebenchmark)
    find_package(SQLite3 REQUIRED)
    add_executable(fluxen_benchmarks benchmarks/bench.cpp)
    add_executable(fluxen_concurrency_benchmarks benchmarks/bench_concurrent.cpp)
    target_link_libraries(fluxen_benchmarks PRIVATE benchmark::benchmark_main SQLite::SQLite3)
    target_link_libraries(fluxen_concurrency_benchmarks PRIVATE benchmark::benchmark_main SQLite::SQLite3)
endif()

