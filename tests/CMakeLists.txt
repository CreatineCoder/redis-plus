find_package(GTest CONFIG REQUIRED)

add_executable(rp_tests
  test_buffer.cpp
  test_handler.cpp
  test_server.cpp
)
target_link_libraries(rp_tests PRIVATE rp_core GTest::gtest GTest::gtest_main)

include(GoogleTest)
gtest_discover_tests(rp_tests)
