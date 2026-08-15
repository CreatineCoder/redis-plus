#include "rp/commands.hpp"

#include <gtest/gtest.h>

#include <memory>
#include <string>

#include "rp/buffer.hpp"

namespace {

struct CommandTest : public ::testing::Test {
  std::int64_t now = 1'000'000;
  std::shared_ptr<rp::Store> store =
      std::make_shared<rp::Store>([this] { return now; });
  rp::CommandTable table{*store};

  std::string run(const rp::Args& args) { return table.dispatch(args).reply; }
  rp::CommandResult run_full(const rp::Args& args) {
    return table.dispatch(args);
  }
};

// Phase 4: what gets persisted is not always what the client sent. Relative
// expiries must become absolute, or replaying the file later would extend
// every TTL by however long the file sat unused.
TEST_F(CommandTest, PropagatesSetWithAbsoluteExpiry) {
  const auto result = run_full({"SET", "k", "v", "EX", "60"});
  EXPECT_TRUE(result.dirty);
  ASSERT_EQ(result.propagate.size(), 5u);
  EXPECT_EQ(result.propagate[0], "SET");
  EXPECT_EQ(result.propagate[3], "PXAT");
  EXPECT_EQ(result.propagate[4], std::to_string(now + 60000));
}

TEST_F(CommandTest, PropagatesExpireAsPexpireat) {
  run({"SET", "k", "v"});
  const auto result = run_full({"EXPIRE", "k", "30"});
  ASSERT_EQ(result.propagate.size(), 3u);
  EXPECT_EQ(result.propagate[0], "PEXPIREAT");
  EXPECT_EQ(result.propagate[2], std::to_string(now + 30000));
}

TEST_F(CommandTest, ReadsAreNotDirty) {
  run({"SET", "k", "v"});
  EXPECT_FALSE(run_full({"GET", "k"}).dirty);
  EXPECT_FALSE(run_full({"TTL", "k"}).dirty);
  EXPECT_FALSE(run_full({"KEYS", "*"}).dirty);
  EXPECT_FALSE(run_full({"PING"}).dirty);
}

// A write that did not actually change anything must not be persisted.
TEST_F(CommandTest, NoOpWritesAreNotDirty) {
  EXPECT_FALSE(run_full({"DEL", "absent"}).dirty);
  EXPECT_FALSE(run_full({"EXPIRE", "absent", "10"}).dirty);
  EXPECT_FALSE(run_full({"PERSIST", "absent"}).dirty);
  run({"SET", "k", "v"});
  EXPECT_FALSE(run_full({"SET", "k", "other", "NX"}).dirty);
}

TEST_F(CommandTest, SuccessfulWritesAreDirty) {
  EXPECT_TRUE(run_full({"SET", "k", "v"}).dirty);
  EXPECT_TRUE(run_full({"DEL", "k"}).dirty);
  EXPECT_TRUE(run_full({"FLUSHALL"}).dirty);
}

TEST_F(CommandTest, Ping) {
  EXPECT_EQ(run({"PING"}), "+PONG\r\n");
  EXPECT_EQ(run({"ping"}), "+PONG\r\n");  // case-insensitive
  EXPECT_EQ(run({"PING", "hi"}), "$2\r\nhi\r\n");
}

TEST_F(CommandTest, Echo) {
  EXPECT_EQ(run({"ECHO", "hey"}), "$3\r\nhey\r\n");
  EXPECT_NE(run({"ECHO"}).find("wrong number of arguments"), std::string::npos);
}

TEST_F(CommandTest, SetGet) {
  EXPECT_EQ(run({"SET", "k", "v"}), "+OK\r\n");
  EXPECT_EQ(run({"GET", "k"}), "$1\r\nv\r\n");
  EXPECT_EQ(run({"GET", "missing"}), "$-1\r\n");
}

TEST_F(CommandTest, SetWithPxExpires) {
  run({"SET", "k", "v", "PX", "100"});
  EXPECT_EQ(run({"GET", "k"}), "$1\r\nv\r\n");
  now += 100;
  EXPECT_EQ(run({"GET", "k"}), "$-1\r\n");
}

TEST_F(CommandTest, SetWithExExpires) {
  run({"SET", "k", "v", "EX", "2"});
  EXPECT_EQ(run({"PTTL", "k"}), ":2000\r\n");
  now += 2000;
  EXPECT_EQ(run({"GET", "k"}), "$-1\r\n");
}

TEST_F(CommandTest, SetWithAbsoluteExpiry) {
  run({"SET", "k", "v", "PXAT", std::to_string(now + 500)});
  EXPECT_EQ(run({"PTTL", "k"}), ":500\r\n");
}

TEST_F(CommandTest, SetNxAndXx) {
  EXPECT_EQ(run({"SET", "k", "a", "NX"}), "+OK\r\n");
  EXPECT_EQ(run({"SET", "k", "b", "NX"}), "$-1\r\n");
  EXPECT_EQ(run({"GET", "k"}), "$1\r\na\r\n");
  EXPECT_EQ(run({"SET", "k", "c", "XX"}), "+OK\r\n");
  EXPECT_EQ(run({"GET", "k"}), "$1\r\nc\r\n");
  EXPECT_EQ(run({"SET", "fresh", "v", "XX"}), "$-1\r\n");
}

TEST_F(CommandTest, SetGetOptionReturnsOldValue) {
  run({"SET", "k", "old"});
  EXPECT_EQ(run({"SET", "k", "new", "GET"}), "$3\r\nold\r\n");
  EXPECT_EQ(run({"GET", "k"}), "$3\r\nnew\r\n");
}

TEST_F(CommandTest, SetClearsTtlUnlessKeepttl) {
  run({"SET", "k", "v", "PX", "5000"});
  run({"SET", "k", "v2"});
  EXPECT_EQ(run({"PTTL", "k"}), ":-1\r\n");

  run({"SET", "j", "v", "PX", "5000"});
  run({"SET", "j", "v2", "KEEPTTL"});
  EXPECT_EQ(run({"PTTL", "j"}), ":5000\r\n");
}

TEST_F(CommandTest, SetRejectsBadOptions) {
  EXPECT_NE(run({"SET", "k", "v", "PX", "abc"}).find("not an integer"),
            std::string::npos);
  EXPECT_NE(run({"SET", "k", "v", "PX", "0"}).find("invalid expire"),
            std::string::npos);
  EXPECT_NE(run({"SET", "k", "v", "BOGUS"}).find("syntax error"),
            std::string::npos);
  EXPECT_NE(run({"SET", "k", "v", "NX", "XX"}).find("syntax error"),
            std::string::npos);
  EXPECT_NE(run({"SET", "k", "v", "PX"}).find("syntax error"),
            std::string::npos);
}

TEST_F(CommandTest, DelAndExists) {
  run({"SET", "a", "1"});
  run({"SET", "b", "2"});
  EXPECT_EQ(run({"EXISTS", "a", "b", "c"}), ":2\r\n");
  EXPECT_EQ(run({"DEL", "a", "b", "c"}), ":2\r\n");
  EXPECT_EQ(run({"EXISTS", "a"}), ":0\r\n");
}

TEST_F(CommandTest, Type) {
  run({"SET", "k", "v"});
  EXPECT_EQ(run({"TYPE", "k"}), "+string\r\n");
  EXPECT_EQ(run({"TYPE", "absent"}), "+none\r\n");
}

TEST_F(CommandTest, Keys) {
  run({"SET", "user:1", "a"});
  run({"SET", "user:2", "b"});
  run({"SET", "other", "c"});
  const std::string reply = run({"KEYS", "user:*"});
  EXPECT_EQ(reply.substr(0, 4), "*2\r\n");
  EXPECT_NE(reply.find("user:1"), std::string::npos);
  EXPECT_EQ(reply.find("other"), std::string::npos);
}

TEST_F(CommandTest, TtlRoundsUp) {
  run({"SET", "k", "v", "PX", "1500"});
  EXPECT_EQ(run({"TTL", "k"}), ":2\r\n");
  EXPECT_EQ(run({"TTL", "absent"}), ":-2\r\n");
  run({"SET", "p", "v"});
  EXPECT_EQ(run({"TTL", "p"}), ":-1\r\n");
}

TEST_F(CommandTest, ExpireFamily) {
  run({"SET", "k", "v"});
  EXPECT_EQ(run({"EXPIRE", "k", "10"}), ":1\r\n");
  EXPECT_EQ(run({"PTTL", "k"}), ":10000\r\n");
  EXPECT_EQ(run({"PERSIST", "k"}), ":1\r\n");
  EXPECT_EQ(run({"PTTL", "k"}), ":-1\r\n");
  EXPECT_EQ(run({"EXPIRE", "absent", "10"}), ":0\r\n");
}

TEST_F(CommandTest, DbsizeAndFlush) {
  run({"SET", "a", "1"});
  run({"SET", "b", "2"});
  EXPECT_EQ(run({"DBSIZE"}), ":2\r\n");
  EXPECT_EQ(run({"FLUSHALL"}), "+OK\r\n");
  EXPECT_EQ(run({"DBSIZE"}), ":0\r\n");
}

TEST_F(CommandTest, ConfigGetReturnsPairs) {
  const std::string reply = run({"CONFIG", "GET", "appendonly"});
  EXPECT_EQ(reply.substr(0, 4), "*2\r\n");
  EXPECT_NE(reply.find("appendonly"), std::string::npos);
}

TEST_F(CommandTest, UnknownCommand) {
  EXPECT_NE(run({"NOSUCHCMD", "x"}).find("unknown command"),
            std::string::npos);
}

// ---------------------------------------------------------------------------
// Handler-level: parsing and dispatch together, over a buffer.
// ---------------------------------------------------------------------------

struct RespHandlerTest : public ::testing::Test {
  std::shared_ptr<rp::Store> store = std::make_shared<rp::Store>();
  rp::RespHandler handler{store};
  rp::Buffer in, out;
};

TEST_F(RespHandlerTest, PipelinedSetGet) {
  in.append("*3\r\n$3\r\nSET\r\n$1\r\nk\r\n$1\r\nv\r\n*2\r\n$3\r\nGET\r\n$1\r\nk\r\n");
  EXPECT_EQ(handler.on_data(in, out), 2u);
  EXPECT_EQ(out.readable(), "+OK\r\n$1\r\nv\r\n");
  EXPECT_TRUE(in.empty());
}

TEST_F(RespHandlerTest, PartialRequestWaitsForRest) {
  in.append("*2\r\n$3\r\nGET\r\n$1\r");
  EXPECT_EQ(handler.on_data(in, out), 0u);
  EXPECT_TRUE(out.empty());
  in.append("\nk\r\n");
  EXPECT_EQ(handler.on_data(in, out), 1u);
  EXPECT_EQ(out.readable(), "$-1\r\n");
}

TEST_F(RespHandlerTest, ProtocolErrorRepliesOnceAndStops) {
  in.append("*abc\r\n*1\r\n$4\r\nPING\r\n");
  EXPECT_EQ(handler.on_data(in, out), 0u);
  const std::string reply(out.readable());
  EXPECT_EQ(reply.front(), '-');
  EXPECT_NE(reply.find("Protocol error"), std::string::npos);
  EXPECT_TRUE(in.empty());  // framing is lost; the rest is discarded
}

TEST_F(RespHandlerTest, EmptyMultibulkProducesNoReply) {
  in.append("*0\r\n*1\r\n$4\r\nPING\r\n");
  EXPECT_EQ(handler.on_data(in, out), 1u);
  EXPECT_EQ(out.readable(), "+PONG\r\n");
}

}  // namespace
