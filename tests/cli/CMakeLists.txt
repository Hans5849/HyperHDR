cmake_minimum_required(VERSION 3.16)
project(HyperHDRCommandLineTests LANGUAGES CXX)

# Builds the real commandline::Parser sources against Qt Core only; no daemon,
# grabber or network code is linked.
find_package(Qt6 COMPONENTS Core QUIET)
if(NOT Qt6_FOUND)
	find_package(Qt5 5.10 COMPONENTS Core REQUIRED)
	set(QT_CORE Qt5::Core)
else()
	set(QT_CORE Qt6::Core)
endif()

enable_testing()

set(ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../..)
add_executable(commandline-test
	main.cpp
	${ROOT}/sources/commandline/Option.cpp
	${ROOT}/sources/commandline/BooleanOption.cpp
	${ROOT}/sources/commandline/Parser.cpp
)
target_include_directories(commandline-test PRIVATE ${ROOT}/include)
target_compile_features(commandline-test PRIVATE cxx_std_17)
target_link_libraries(commandline-test PRIVATE ${QT_CORE})
add_test(NAME commandline-parser COMMAND commandline-test)
set_tests_properties(commandline-parser PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
