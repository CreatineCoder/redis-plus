cmake_minimum_required(VERSION 3.20)
project(redis_plus LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(RP_BUILD_TESTS "Build unit + integration tests" ON)
option(RP_BUILD_BENCH "Build benchmark harness" ON)
option(RP_BUILD_FUZZ "Build libFuzzer targets (clang only)" OFF)

find_package(asio CONFIG REQUIRED)
find_package(Threads REQUIRED)

add_library(rp_core
  src/server.cpp
  src/asio_server.cpp
  src/poll_server.cpp
  src/handler.cpp
  src/resp.cpp
  src/store.cpp
  src/commands.cpp
  src/stats.cpp
)
target_include_directories(rp_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(rp_core PUBLIC asio::asio Threads::Threads)
target_compile_definitions(rp_core PUBLIC ASIO_STANDALONE ASIO_NO_DEPRECATED)

if(WIN32)
  # asio needs a modern Windows API level for the IOCP reactor.
  target_compile_definitions(rp_core PUBLIC _WIN32_WINNT=0x0A00)
  target_link_libraries(rp_core PUBLIC ws2_32 mswsock)
endif()

if(MSVC)
  target_compile_options(rp_core PRIVATE /W4 /permissive-)
else()
  target_compile_options(rp_core PRIVATE -Wall -Wextra -Wpedantic)
endif()

add_executable(redis-plus src/main.cpp)
target_link_libraries(redis-plus PRIVATE rp_core)

if(RP_BUILD_BENCH)
  add_subdirectory(bench)
endif()

if(RP_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()

if(RP_BUILD_FUZZ)
  add_subdirectory(fuzz)
endif()
