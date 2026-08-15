// RDB format tests.
//
// Binary formats fail silently, so these check the bytes, not just the
// round-trip: a writer and reader that are wrong in the same way would pass a
// round-trip test forever and produce a file no other tool can read.

#include "rp/rdb.hpp"

#include <gtest/gtest.h>

#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

namespace {

using rp::Record;

std::vector<Record> parse_or_fail(const std::string& payload) {
  std::vector<Record> out;
  std::string error;
  EXPECT_TRUE(rp::rdb_parse(payload, &out, &error)) << error;
  return out;
}

TEST(Rdb, HeaderIsRealRedisMagic) {
  const std::string payload = rp::rdb_serialize({});
  EXPECT_EQ(payload.substr(0, 9), "REDIS0011")
      << "redis-check-rdb and a real replica both key off this";
}

TEST(Rdb, EndsWithEofOpcodeAndChecksum) {
  const std::string payload = rp::rdb_serialize({{"k", "v", rp::kNoExpiry}});
  ASSERT_GE(payload.size(), 9u);
  EXPECT_EQ(static_cast<unsigned char>(payload[payload.size() - 9]), 0xFF);

  // The trailer must be the CRC64 of everything before it.
  const std::uint64_t expected =
      rp::crc64(0, payload.data(), payload.size() - 8);
  std::uint64_t stored = 0;
  for (int i = 0; i < 8; ++i) {
    stored |= static_cast<std::uint64_t>(
                  static_cast<unsigned char>(payload[payload.size() - 8 + i]))
              << (i * 8);
  }
  EXPECT_EQ(stored, expected);
}

TEST(Rdb, EmptyDatabaseRoundTrips) {
  EXPECT_TRUE(parse_or_fail(rp::rdb_serialize({})).empty());
}

TEST(Rdb, SingleKeyRoundTrips) {
  const auto records = parse_or_fail(
      rp::rdb_serialize({{"key", "value", rp::kNoExpiry}}));
  ASSERT_EQ(records.size(), 1u);
  EXPECT_EQ(records[0].key, "key");
  EXPECT_EQ(records[0].value, "value");
  EXPECT_EQ(records[0].expire_at, rp::kNoExpiry);
}

TEST(Rdb, ExpiryIsPreservedExactly) {
  const std::int64_t deadline = 1'700'000'000'123LL;
  const auto records =
      parse_or_fail(rp::rdb_serialize({{"k", "v", deadline}}));
  ASSERT_EQ(records.size(), 1u);
  EXPECT_EQ(records[0].expire_at, deadline);
}

TEST(Rdb, ManyKeysRoundTrip) {
  std::vector<Record> in;
  for (int i = 0; i < 5000; ++i) {
    in.push_back({"key:" + std::to_string(i), "value:" + std::to_string(i),
                  (i % 3 == 0) ? 1'700'000'000'000LL + i : rp::kNoExpiry});
  }
  const auto out = parse_or_fail(rp::rdb_serialize(in));
  ASSERT_EQ(out.size(), in.size());
  for (std::size_t i = 0; i < in.size(); ++i) {
    EXPECT_EQ(out[i].key, in[i].key);
    EXPECT_EQ(out[i].value, in[i].value);
    EXPECT_EQ(out[i].expire_at, in[i].expire_at);
  }
}

// Each length-encoding branch has its own on-disk shape; a bug in one is
// invisible until someone stores a string of exactly the wrong size.
TEST(Rdb, AllLengthEncodingBoundaries) {
  for (const std::size_t len : {0u, 1u, 63u, 64u, 65u, 16383u, 16384u, 70000u}) {
    const std::string value(len, 'x');
    const auto out = parse_or_fail(rp::rdb_serialize({{"k", value, rp::kNoExpiry}}));
    ASSERT_EQ(out.size(), 1u) << "length " << len;
    EXPECT_EQ(out[0].value.size(), len) << "length " << len;
    EXPECT_EQ(out[0].value, value) << "length " << len;
  }
}

TEST(Rdb, BinarySafeKeysAndValues) {
  const std::string key("a\0b\r\n", 5);
  const std::string value("\0\xff\xfe binary \0", 14);
  const auto out = parse_or_fail(rp::rdb_serialize({{key, value, rp::kNoExpiry}}));
  ASSERT_EQ(out.size(), 1u);
  EXPECT_EQ(out[0].key, key);
  EXPECT_EQ(out[0].value, value);
}

// --- corruption must be loud ----------------------------------------------

TEST(Rdb, RejectsBadMagic) {
  std::vector<Record> out;
  std::string error;
  EXPECT_FALSE(rp::rdb_parse("NOTAREDISFILE....", &out, &error));
  EXPECT_NE(error.find("magic"), std::string::npos);
}

TEST(Rdb, RejectsFlippedByte) {
  std::string payload = rp::rdb_serialize({{"key", "value", rp::kNoExpiry}});
  payload[payload.size() - 12] ^= 0x01;  // inside the data, before the CRC

  std::vector<Record> out;
  std::string error;
  EXPECT_FALSE(rp::rdb_parse(payload, &out, &error));
  EXPECT_NE(error.find("checksum"), std::string::npos);
  EXPECT_TRUE(out.empty()) << "a corrupt file must never be partially applied";
}

TEST(Rdb, RejectsTruncationAtEveryOffset) {
  const std::string payload =
      rp::rdb_serialize({{"k1", "v1", 1'700'000'000'000LL}, {"k2", "v2", -1}});
  for (std::size_t n = 1; n < payload.size(); ++n) {
    std::vector<Record> out;
    std::string error;
    EXPECT_FALSE(rp::rdb_parse(payload.substr(0, n), &out, &error))
        << "accepted a file truncated to " << n << " bytes";
  }
}

TEST(Rdb, RejectsEmptyPayload) {
  std::vector<Record> out;
  std::string error;
  EXPECT_FALSE(rp::rdb_parse("", &out, &error));
}

// --- file handling ---------------------------------------------------------

struct RdbFileTest : public ::testing::Test {
  std::string path = "test_dump.rdb";
  void TearDown() override {
    std::remove(path.c_str());
    std::remove((path + ".tmp").c_str());
  }
};

TEST_F(RdbFileTest, SaveThenLoad) {
  const std::vector<Record> in = {{"a", "1", -1}, {"b", "2", 1'700'000'000'000LL}};
  std::string error;
  ASSERT_TRUE(rp::rdb_save_file(path, in, &error)) << error;

  std::vector<Record> out;
  ASSERT_TRUE(rp::rdb_load_file(path, &out, &error)) << error;
  EXPECT_EQ(out.size(), 2u);
}

TEST_F(RdbFileTest, MissingFileIsNotAnError) {
  std::vector<Record> out;
  std::string error;
  EXPECT_TRUE(rp::rdb_load_file("definitely_not_here.rdb", &out, &error));
  EXPECT_TRUE(out.empty());
}

TEST_F(RdbFileTest, SaveOverwritesAtomically) {
  std::string error;
  ASSERT_TRUE(rp::rdb_save_file(path, {{"old", "1", -1}}, &error));
  ASSERT_TRUE(rp::rdb_save_file(path, {{"new", "2", -1}}, &error));

  std::vector<Record> out;
  ASSERT_TRUE(rp::rdb_load_file(path, &out, &error)) << error;
  ASSERT_EQ(out.size(), 1u);
  EXPECT_EQ(out[0].key, "new");
}

TEST_F(RdbFileTest, CorruptFileOnDiskIsRejected) {
  std::string error;
  ASSERT_TRUE(rp::rdb_save_file(path, {{"k", "v", -1}}, &error));
  {
    std::fstream f(path, std::ios::binary | std::ios::in | std::ios::out);
    f.seekp(12);
    f.put('\xEE');
  }
  std::vector<Record> out;
  EXPECT_FALSE(rp::rdb_load_file(path, &out, &error));
}

}  // namespace
