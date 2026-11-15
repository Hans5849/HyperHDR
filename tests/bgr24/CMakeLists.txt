cmake_minimum_required(VERSION 3.16)
project(Bgr24ReferenceTests LANGUAGES CXX)
enable_testing()
# Header-only decoder pieces (include/utils/Bgr24.h): no Qt, no hardware.
add_executable(bgr24-reference-test main.cpp)
target_include_directories(bgr24-reference-test PRIVATE ../../include)
target_compile_features(bgr24-reference-test PRIVATE cxx_std_17)
target_compile_options(bgr24-reference-test PRIVATE -Wall -Wextra -Werror)
add_test(NAME bgr24-reference COMMAND bgr24-reference-test)
