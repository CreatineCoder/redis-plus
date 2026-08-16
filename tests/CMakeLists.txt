find_package(GTest CONFIG QUIET)
if(NOT GTest_FOUND)
  message(STATUS "GTest not found via find_package; fetching it")
  include(FetchContent)
  FetchContent_Declare(googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG v1.14.0
    GIT_SHALLOW TRUE
  )
  set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
  set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
  FetchContent_MakeAvailable(googletest)
endif()

add_executable(rp_tests
  test_buffer.cpp
  test_handler.cpp
  test_resp.cpp
  test_resp_fuzz.cpp
  test_store.cpp
  test_expiry.cpp
  test_commands.cpp
  test_rdb.cpp
  test_persistence.cpp
  test_replication.cpp
  test_server.cpp
)
target_link_libraries(rp_tests PRIVATE rp_core GTest::gtest GTest::gtest_main)

include(GoogleTest)
gtest_discover_tests(rp_tests)
