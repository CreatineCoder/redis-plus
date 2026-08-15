cmake_minimum_required(VERSION 3.20)
project(redis_plus LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(RP_BUILD_TESTS "Build unit + integration tests" ON)
option(RP_BUILD_BENCH "Build benchmark harness" ON)
option(RP_BUILD_FUZZ "Build libFuzzer targets (clang only)" OFF)

find_package(Threads REQUIRED)

# Dependencies come from vcpkg when a toolchain file is supplied; otherwise
# CMake fetches them itself, so the project builds on a machine with nothing
# but a compiler and CMake.
find_package(asio CONFIG QUIET)
if(NOT asio_FOUND)
  message(STATUS "asio not found via find_package; fetching it")
  include(FetchContent)
  FetchContent_Declare(asio
    GIT_REPOSITORY https://github.com/chriskohlhoff/asio.git
    GIT_TAG asio-1-30-2
    GIT_SHALLOW TRUE
  )
  FetchContent_MakeAvailable(asio)
  # asio ships no CMake package; expose the same target name either way.
  add_library(asio_fetched INTERFACE)
  target_include_directories(asio_fetched INTERFACE ${asio_SOURCE_DIR}/asio/include)
  target_link_libraries(asio_fetched INTERFACE Threads::Threads)
  add_library(asio::asio ALIAS asio_fetched)
endif()

add_library(rp_core
  src/server.cpp
  src/asio_server.cpp
  src/poll_server.cpp
  src/handler.cpp
  src/resp.cpp
  src/store.cpp
  src/commands.cpp
  src/rdb.cpp
  src/aof.cpp
  src/persistence.cpp
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

# MinGW links libstdc++/libgcc/winpthread dynamically by default, so the
# executable refuses to start unless the compiler's bin directory happens to be
# on PATH -- which it is not for anyone who just runs the binary. Link them in.
if(MINGW)
  target_link_options(rp_core INTERFACE
    -static-libgcc -static-libstdc++ -Wl,-Bstatic,--whole-archive -lwinpthread
    -Wl,--no-whole-archive)
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
