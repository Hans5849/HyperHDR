cmake_minimum_required(VERSION 3.16)
project(ColorReferenceTests CXX)

# Colour-quality references (ColorReference.h) and a CPU backend built from
# include/utils/Bgr24.h: no Qt. The real FrameDecoder dispatch runs the same
# cases in tests/codec_test (DecoderTest --color-quality).
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
add_compile_options(-Wall -Wextra -Werror)

add_executable(color-reference-test main.cpp)
target_include_directories(color-reference-test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../../include)

enable_testing()
add_test(NAME color-reference COMMAND color-reference-test)
