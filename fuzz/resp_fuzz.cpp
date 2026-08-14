// libFuzzer entry point for the RESP parser (clang only).
//
//   cmake -B build-fuzz -DRP_BUILD_FUZZ=ON -DCMAKE_CXX_COMPILER=clang++ \
//         -DCMAKE_BUILD_TYPE=RelWithDebInfo
//   cmake --build build-fuzz --target resp_fuzz
//   ./build-fuzz/fuzz/resp_fuzz fuzz/corpus -max_total_time=3600
//
// Phase 2's gate: one clean fuzzing hour with ASan+UBSan and no crashes.
// Record the hours run in bench/results.md -- "fuzzed for N hours, zero
// crashes" is a claim worth making only if it actually happened.

#include <cstddef>
#include <cstdint>
#include <string_view>

#include "rp/resp.hpp"

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data,
                                      std::size_t size) {
  const std::string_view input(reinterpret_cast<const char*>(data), size);
  const rp::ParseResult result = rp::parse_request(input);

  // The parser must never claim to have consumed more than it was given.
  if (result.consumed > size) __builtin_trap();
  if (result.status != rp::ParseStatus::kOk && result.consumed != 0) {
    __builtin_trap();
  }
  return 0;
}
