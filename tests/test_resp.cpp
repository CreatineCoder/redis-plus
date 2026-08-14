#include "rp/resp.hpp"

#include <gtest/gtest.h>

#include <string>

namespace {

using rp::parse_request;
using rp::ParseStatus;

rp::ParseResult parse(const std::string& s) { return parse_request(s); }

TEST(Resp, SimpleCommand) {
  const auto r = parse("*1\r\n$4\r\nPING\r\n");
  ASSERT_EQ(r.status, ParseStatus::kOk);
  EXPECT_EQ(r.consumed, 14u);
  ASSERT_EQ(r.args.size(), 1u);
  EXPECT_EQ(r.args[0], "PING");
}

TEST(Resp, MultipleArguments) {
  const auto r = parse("*3\r\n$3\r\nSET\r\n$3\r\nfoo\r\n$3\r\nbar\r\n");
  ASSERT_EQ(r.status, ParseStatus::kOk);
  ASSERT_EQ(r.args.size(), 3u);
  EXPECT_EQ(r.args[1], "foo");
  EXPECT_EQ(r.args[2], "bar");
}

TEST(Resp, ConsumesOnlyTheFirstRequest) {
  const std::string two = "*1\r\n$4\r\nPING\r\n*1\r\n$4\r\nPING\r\n";
  const auto r = parse(two);
  ASSERT_EQ(r.status, ParseStatus::kOk);
  EXPECT_EQ(r.consumed, 14u);
}

// Every proper prefix of a valid request must report kIncomplete and consume
// nothing. This is the invariant the reference parser violated -- it read past
// the end of its buffer on any truncated input.
TEST(Resp, EveryPrefixIsIncompleteNeverCorrupt) {
  const std::string req = "*3\r\n$3\r\nSET\r\n$3\r\nfoo\r\n$5\r\nvalue\r\n";
  for (std::size_t n = 1; n < req.size(); ++n) {
    const auto r = parse(req.substr(0, n));
    EXPECT_EQ(r.status, ParseStatus::kIncomplete) << "prefix length " << n;
    EXPECT_EQ(r.consumed, 0u) << "prefix length " << n;
  }
  EXPECT_EQ(parse(req).status, ParseStatus::kOk);
}

TEST(Resp, EmptyBulkArgument) {
  const auto r = parse("*2\r\n$4\r\nECHO\r\n$0\r\n\r\n");
  ASSERT_EQ(r.status, ParseStatus::kOk);
  ASSERT_EQ(r.args.size(), 2u);
  EXPECT_EQ(r.args[1], "");
}

TEST(Resp, BinarySafeWithEmbeddedCrlfAndNul) {
  const std::string payload("a\r\nb\0c", 6);
  const auto r = parse("*2\r\n$3\r\nGET\r\n$6\r\n" + payload + "\r\n");
  ASSERT_EQ(r.status, ParseStatus::kOk);
  ASSERT_EQ(r.args.size(), 2u);
  EXPECT_EQ(r.args[1], payload);
}

TEST(Resp, InlineCommand) {
  const auto r = parse("PING\r\n");
  ASSERT_EQ(r.status, ParseStatus::kOk);
  ASSERT_EQ(r.args.size(), 1u);
  EXPECT_EQ(r.args[0], "PING");
}

TEST(Resp, InlineCommandWithArguments) {
  const auto r = parse("SET  foo   bar\r\n");
  ASSERT_EQ(r.status, ParseStatus::kOk);
  ASSERT_EQ(r.args.size(), 3u);
  EXPECT_EQ(r.args[2], "bar");
}

TEST(Resp, EmptyMultibulkIsAcceptedAsNoOp) {
  const auto r = parse("*0\r\n");
  EXPECT_EQ(r.status, ParseStatus::kOk);
  EXPECT_EQ(r.consumed, 4u);
  EXPECT_TRUE(r.args.empty());
}

TEST(Resp, NegativeMultibulkIsNoOp) {
  const auto r = parse("*-1\r\n");
  EXPECT_EQ(r.status, ParseStatus::kOk);
  EXPECT_TRUE(r.args.empty());
}

TEST(Resp, RejectsNonNumericMultibulk) {
  EXPECT_EQ(parse("*abc\r\n").status, ParseStatus::kError);
}

TEST(Resp, RejectsHugeMultibulk) {
  EXPECT_EQ(parse("*99999999\r\n").status, ParseStatus::kError);
}

TEST(Resp, RejectsNegativeBulkLength) {
  EXPECT_EQ(parse("*1\r\n$-5\r\n").status, ParseStatus::kError);
}

TEST(Resp, RejectsOversizedBulkLength) {
  EXPECT_EQ(parse("*1\r\n$999999999999\r\n").status, ParseStatus::kError);
}

TEST(Resp, RejectsMissingDollar) {
  EXPECT_EQ(parse("*1\r\n+PING\r\n").status, ParseStatus::kError);
}

TEST(Resp, RejectsUnbalancedPayload) {
  // Declared 4 bytes, but the terminator is not where it should be.
  EXPECT_EQ(parse("*1\r\n$4\r\nPINGX\r\n").status, ParseStatus::kError);
}

TEST(Resp, RejectsIntegerOverflowInLength) {
  EXPECT_EQ(parse("*1\r\n$99999999999999999999999\r\n").status,
            ParseStatus::kError);
}

TEST(Resp, RejectsEndlessDigitStream) {
  EXPECT_EQ(parse("*" + std::string(200, '1')).status, ParseStatus::kError);
}

TEST(Resp, RejectsOversizedInlineRequest) {
  EXPECT_EQ(parse(std::string(rp::kMaxInlineLength + 10, 'x')).status,
            ParseStatus::kError);
}

// ---------------------------------------------------------------------------

TEST(Glob, Literal) {
  EXPECT_TRUE(rp::glob_match("foo", "foo"));
  EXPECT_FALSE(rp::glob_match("foo", "bar"));
}

TEST(Glob, Star) {
  EXPECT_TRUE(rp::glob_match("*", "anything"));
  EXPECT_TRUE(rp::glob_match("*", ""));
  EXPECT_TRUE(rp::glob_match("user:*", "user:1"));
  EXPECT_FALSE(rp::glob_match("user:*", "admin:1"));
  EXPECT_TRUE(rp::glob_match("*:*:*", "a:b:c"));
}

TEST(Glob, QuestionMark) {
  EXPECT_TRUE(rp::glob_match("h?llo", "hello"));
  EXPECT_FALSE(rp::glob_match("h?llo", "hllo"));
}

TEST(Glob, CharacterClass) {
  EXPECT_TRUE(rp::glob_match("h[ae]llo", "hello"));
  EXPECT_FALSE(rp::glob_match("h[ae]llo", "hillo"));
  EXPECT_TRUE(rp::glob_match("h[^e]llo", "hallo"));
  EXPECT_FALSE(rp::glob_match("h[^e]llo", "hello"));
  EXPECT_TRUE(rp::glob_match("k[0-9]", "k5"));
  EXPECT_FALSE(rp::glob_match("k[0-9]", "kx"));
}

// A backtracking matcher can go exponential on this shape; the iterative one
// must not. If this test hangs, the implementation regressed.
TEST(Glob, PathologicalPatternTerminates) {
  EXPECT_FALSE(rp::glob_match("*a*a*a*a*a*a*a*a*b",
                              std::string(64, 'a').c_str()));
}

}  // namespace
