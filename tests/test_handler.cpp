#include "rp/handler.hpp"

#include <gtest/gtest.h>

#include <string>

#include "rp/buffer.hpp"

namespace {

// The framing behaviour that Phase 1 depends on and that the legacy parser got
// wrong: a request split across reads must not be consumed early, and several
// requests arriving in one read must all be answered.
struct HandlerTest : public ::testing::Test {
  rp::PingPongHandler h;
  rp::Buffer in, out;
};

TEST_F(HandlerTest, RespPing) {
  in.append("*1\r\n$4\r\nPING\r\n");
  EXPECT_EQ(h.on_data(in, out), 1u);
  EXPECT_EQ(out.readable(), "+PONG\r\n");
  EXPECT_TRUE(in.empty());
}

TEST_F(HandlerTest, InlinePing) {
  in.append("PING\r\n");
  EXPECT_EQ(h.on_data(in, out), 1u);
  EXPECT_EQ(out.readable(), "+PONG\r\n");
}

TEST_F(HandlerTest, PipelinedRequestsAllAnswered) {
  in.append("*1\r\n$4\r\nPING\r\n*1\r\n$4\r\nPING\r\n*1\r\n$4\r\nPING\r\n");
  EXPECT_EQ(h.on_data(in, out), 3u);
  EXPECT_EQ(out.readable(), "+PONG\r\n+PONG\r\n+PONG\r\n");
  EXPECT_TRUE(in.empty());
}

TEST_F(HandlerTest, PartialRequestIsNotConsumed) {
  in.append("*1\r\n$4\r\nPI");
  EXPECT_EQ(h.on_data(in, out), 0u);
  EXPECT_EQ(in.size(), 10u);
  EXPECT_TRUE(out.empty());

  in.append("NG\r\n");
  EXPECT_EQ(h.on_data(in, out), 1u);
  EXPECT_EQ(out.readable(), "+PONG\r\n");
}

TEST_F(HandlerTest, ByteAtATimeDelivery) {
  const std::string req = "*1\r\n$4\r\nPING\r\n";
  std::size_t total = 0;
  for (const char c : req) {
    in.append(&c, 1);
    total += h.on_data(in, out);
  }
  EXPECT_EQ(total, 1u);
  EXPECT_EQ(out.readable(), "+PONG\r\n");
}

TEST_F(HandlerTest, TrailingPartialAfterCompleteIsRetained) {
  in.append("*1\r\n$4\r\nPING\r\n*1\r\n$4\r\nPI");
  EXPECT_EQ(h.on_data(in, out), 1u);
  EXPECT_EQ(in.size(), 10u);
}

TEST_F(HandlerTest, GarbageMultibulkDoesNotOverrun) {
  in.append("*abc\r\n");
  EXPECT_EQ(h.on_data(in, out), 0u);
}

TEST_F(HandlerTest, OversizedDeclaredLengthIsRejected) {
  in.append("*1\r\n$999999999999\r\n");
  EXPECT_EQ(h.on_data(in, out), 0u);
}

TEST_F(HandlerTest, InfoReturnsBulkString) {
  in.append("*1\r\n$4\r\nINFO\r\n");
  EXPECT_EQ(h.on_data(in, out), 1u);
  const std::string reply(out.readable());
  EXPECT_EQ(reply.front(), '$');
  EXPECT_NE(reply.find("connected_clients:"), std::string::npos);
}

}  // namespace
