# libFuzzer target. Clang only; guarded so the normal build never sees it.
if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
  message(WARNING "RP_BUILD_FUZZ requires clang; skipping fuzz targets")
  return()
endif()

add_executable(resp_fuzz resp_fuzz.cpp)
target_link_libraries(resp_fuzz PRIVATE rp_core)
target_compile_options(resp_fuzz
  PRIVATE -fsanitize=fuzzer,address,undefined -fno-omit-frame-pointer)
target_link_options(resp_fuzz PRIVATE -fsanitize=fuzzer,address,undefined)
