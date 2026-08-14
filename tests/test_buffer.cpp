#include "rp/buffer.hpp"

#include <gtest/gtest.h>

#include <string>

using rp::Buffer;

TEST(Buffer, StartsEmpty) {
  Buffer b;
  EXPECT_TRUE(b.empty());
  EXPECT_EQ(b.size(), 0u);
  EXPECT_EQ(b.readable(), "");
}

TEST(Buffer, AppendThenRead) {
  Buffer b;
  b.append("hello");
  EXPECT_EQ(b.size(), 5u);
  EXPECT_EQ(b.readable(), "hello");
}

TEST(Buffer, PartialConsumeLeavesRemainder) {
  Buffer b;
  b.append("PING\r\nECHO\r\n");
  b.consume(6);
  EXPECT_EQ(b.readable(), "ECHO\r\n");
}

TEST(Buffer, ConsumeBeyondSizeIsClamped) {
  Buffer b;
  b.append("abc");
  b.consume(999);
  EXPECT_TRUE(b.empty());
}

TEST(Buffer, AppendAfterPartialConsumeKeepsOrder) {
  Buffer b;
  b.append("abcdef");
  b.consume(3);
  b.append("ghi");
  EXPECT_EQ(b.readable(), "defghi");
}

TEST(Buffer, HandlesEmbeddedNulls) {
  Buffer b;
  const std::string payload("a\0b", 3);
  b.append(payload.data(), payload.size());
  EXPECT_EQ(b.size(), 3u);
  EXPECT_EQ(std::string(b.readable()), payload);
}

// The reclaim path is what keeps a long-lived pipelining connection from
// growing without bound. Many small append/consume cycles must not grow
// capacity indefinitely.
TEST(Buffer, ReclaimsSpaceOverManySmallCycles) {
  Buffer b;
  for (int i = 0; i < 100000; ++i) {
    b.append("*1\r\n$4\r\nPING\r\n");
    b.consume(14);
  }
  EXPECT_TRUE(b.empty());
  EXPECT_LT(b.capacity(), 1u << 20);
}

TEST(Buffer, SurvivesLargeValue) {
  Buffer b;
  const std::string big(4 * 1024 * 1024, 'x');
  b.append(big);
  EXPECT_EQ(b.size(), big.size());
  b.consume(big.size());
  EXPECT_TRUE(b.empty());
}
