cmake_minimum_required(VERSION 3.16)
project(BenchReportTests CXX)

# Benchmark reporting (tests/codec_test/BenchReport.h) without Qt or a decoder:
# statistics, metadata, LUT access profile and the text/JSON/CSV writers.
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
add_compile_options(-Wall -Wextra -Werror)

add_executable(bench-report-test main.cpp)
target_include_directories(bench-report-test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../codec_test ${CMAKE_CURRENT_SOURCE_DIR}/../../include)

enable_testing()
add_test(NAME bench-report COMMAND bench-report-test)
