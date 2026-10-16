cmake_minimum_required(VERSION 3.16)
project(sdrouter LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

add_compile_options(
    -Wall -Wextra -Werror
    -Wshadow -Wnon-virtual-dtor -Woverloaded-virtual
    -Wcast-align -Wpointer-arith -Wold-style-cast
)

option(ENABLE_ASAN "Build with AddressSanitizer and UBSan" OFF)
option(ENABLE_TSAN "Build with ThreadSanitizer"            OFF)

if(ENABLE_ASAN)
    add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
    add_link_options(-fsanitize=address,undefined)
endif()

if(ENABLE_TSAN)
    add_compile_options(-fsanitize=thread -fno-omit-frame-pointer)
    add_link_options(-fsanitize=thread)
endif()

find_package(Threads REQUIRED)

# Everything except main(), so the tests can link against it.
add_library(sdrouter_core STATIC
    src/address.cpp
    src/control_plane.cpp
    src/data_plane.cpp
    src/interface.cpp
    src/log.cpp
    src/neighbor_table.cpp
    src/packet.cpp
    src/qsbr.cpp
    src/route_table.cpp
)
target_include_directories(sdrouter_core PUBLIC include)
target_link_libraries(sdrouter_core PUBLIC Threads::Threads)

add_executable(sdrouter src/main.cpp)
target_link_libraries(sdrouter PRIVATE sdrouter_core)
set_target_properties(sdrouter PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})

enable_testing()

foreach(test_name test_route_table test_concurrency)
    add_executable(${test_name} tests/${test_name}.cpp)
    target_link_libraries(${test_name} PRIVATE sdrouter_core)
    add_test(NAME ${test_name} COMMAND ${test_name})
endforeach()

# Not a test: a measurement, run on demand via `make bench`.
add_executable(bench_snapshot tests/bench_snapshot.cpp)
target_link_libraries(bench_snapshot PRIVATE sdrouter_core)
