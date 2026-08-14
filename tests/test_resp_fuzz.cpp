// Deterministic randomized robustness tests for the parser.
//
// These run everywhere, in CI, with a fixed seed -- unlike the libFuzzer
// target in fuzz/, which needs clang. The contract under test is total: for
// ANY input, parse_request must return, must not read out of bounds, and must
// never report kOk while consuming more bytes than it was given.
//
// Build with -fsanitize=address,undefined for these to be worth much.

#include <gtest/gtest.h>

#include <random>
#include <string>
#include <vector>

#include "rp/commands.hpp"
#include "rp/resp.hpp"

namespace {

void check_invariants(const std::string& input) {
  const rp::ParseResult r = rp::parse_request(input);
  ASSERT_LE(r.consumed, input.size()) << "consumed past end of input";
  if (r.status != rp::ParseStatus::kOk) {
    EXPECT_EQ(r.consumed, 0u) << "consumed bytes without a complete request";
  }
}

TEST(RespFuzz, RandomBytes) {
  std::mt19937 rng(12345);  // fixed seed: failures are reproducible
  std::uniform_int_distribution<int> byte(0, 255);
  std::uniform_int_distribution<int> len(0, 512);

  for (int iter = 0; iter < 20000; ++iter) {
    std::string input;
    const int n = len(rng);
    input.reserve(n);
    for (int i = 0; i < n; ++i) input.push_back(static_cast<char>(byte(rng)));
    check_invariants(input);
  }
}

// Random bytes rarely produce near-valid protocol. Mutating real requests
// reaches the interesting branches -- truncated headers, lying lengths.
TEST(RespFuzz, MutatedValidRequests) {
  const std::vector<std::string> seeds = {
      "*1\r\n$4\r\nPING\r\n",
      "*3\r\n$3\r\nSET\r\n$3\r\nfoo\r\n$3\r\nbar\r\n",
      "*2\r\n$3\r\nGET\r\n$0\r\n\r\n",
      "PING\r\n",
      "*5\r\n$3\r\nSET\r\n$1\r\nk\r\n$1\r\nv\r\n$2\r\nPX\r\n$3\r\n100\r\n",
  };

  std::mt19937 rng(67890);
  std::uniform_int_distribution<int> byte(0, 255);

  for (const auto& seed : seeds) {
    for (int iter = 0; iter < 5000; ++iter) {
      std::string input = seed;
      std::uniform_int_distribution<std::size_t> pos(0, input.size() - 1);
      const int mutations = 1 + (iter % 3);
      for (int m = 0; m < mutations; ++m) {
        switch (iter % 3) {
          case 0:
            input[pos(rng)] = static_cast<char>(byte(rng));
            break;
          case 1:
            input.resize(pos(rng));  // truncate
            break;
          case 2:
            input.insert(pos(rng), 1, static_cast<char>(byte(rng)));
            break;
        }
        if (input.empty()) break;
      }
      check_invariants(input);
    }
  }
}

// Feeding a stream one byte at a time must eventually yield exactly the same
// commands as feeding it whole -- the core incremental-parsing guarantee.
TEST(RespFuzz, ByteAtATimeMatchesWholeStream) {
  const std::string stream =
      "*3\r\n$3\r\nSET\r\n$1\r\nk\r\n$1\r\nv\r\n"
      "*2\r\n$3\r\nGET\r\n$1\r\nk\r\n"
      "*1\r\n$4\r\nPING\r\n"
      "*2\r\n$4\r\nECHO\r\n$5\r\nhello\r\n";

  auto store_whole = std::make_shared<rp::Store>();
  rp::RespHandler whole_handler(store_whole);
  rp::Buffer whole_in, whole_out;
  whole_in.append(stream);
  whole_handler.on_data(whole_in, whole_out);
  const std::string expected(whole_out.readable());

  auto store_drip = std::make_shared<rp::Store>();
  rp::RespHandler drip_handler(store_drip);
  rp::Buffer drip_in, drip_out;
  for (const char c : stream) {
    drip_in.append(&c, 1);
    drip_handler.on_data(drip_in, drip_out);
  }

  EXPECT_EQ(std::string(drip_out.readable()), expected);
  EXPECT_FALSE(expected.empty());
}

// Same, but with random split points rather than every byte.
TEST(RespFuzz, RandomSplitsMatchWholeStream) {
  std::string stream;
  for (int i = 0; i < 50; ++i) {
    stream += "*3\r\n$3\r\nSET\r\n$2\r\nk" + std::to_string(i % 10) +
              "\r\n$1\r\nv\r\n*1\r\n$4\r\nPING\r\n";
  }

  auto store_whole = std::make_shared<rp::Store>();
  rp::RespHandler whole_handler(store_whole);
  rp::Buffer whole_in, whole_out;
  whole_in.append(stream);
  whole_handler.on_data(whole_in, whole_out);
  const std::string expected(whole_out.readable());

  std::mt19937 rng(24680);
  std::uniform_int_distribution<std::size_t> chunk(1, 37);

  for (int trial = 0; trial < 200; ++trial) {
    auto store = std::make_shared<rp::Store>();
    rp::RespHandler handler(store);
    rp::Buffer in, out;
    std::size_t offset = 0;
    while (offset < stream.size()) {
      const std::size_t n = std::min(chunk(rng), stream.size() - offset);
      in.append(stream.data() + offset, n);
      handler.on_data(in, out);
      offset += n;
    }
    EXPECT_EQ(std::string(out.readable()), expected) << "trial " << trial;
  }
}

}  // namespace
