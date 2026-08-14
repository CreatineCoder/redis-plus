// End-to-end tests over a real TCP socket. These are the tests that actually
// exercise Phase 1's reason for existing: partial reads, pipelining, many
// concurrent connections, and writes larger than a kernel socket buffer.

#include <asio.hpp>
#include <gtest/gtest.h>

#include <chrono>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "rp/commands.hpp"
#include "rp/server.hpp"
#include "rp/stats.hpp"
#include "rp/store.hpp"

namespace {

using asio::ip::tcp;

class ServerFixture : public ::testing::TestWithParam<rp::Backend> {
 protected:
  void SetUp() override {
    rp::ServerConfig cfg;
    cfg.bind_address = "127.0.0.1";
    cfg.port = 0;  // let the OS choose, so tests never collide with a real redis
    store_ = std::make_shared<rp::Store>();
    server_ = rp::make_server(GetParam(), cfg,
                              std::make_shared<rp::RespHandler>(store_));
    port_ = server_->port();
    thread_ = std::thread([this] { server_->run(); });
  }

  void TearDown() override {
    server_->stop();
    if (thread_.joinable()) thread_.join();
  }

  tcp::socket connect(asio::io_context& io) {
    tcp::socket s(io);
    s.connect(tcp::endpoint(asio::ip::make_address("127.0.0.1"), port_));
    return s;
  }

  static std::string read_exactly(tcp::socket& s, std::size_t n) {
    std::string buf(n, '\0');
    asio::read(s, asio::buffer(buf.data(), n));
    return buf;
  }

  std::shared_ptr<rp::Store> store_;
  std::unique_ptr<rp::Server> server_;
  std::thread thread_;
  std::uint16_t port_ = 0;
};

// Phase 2 over a real socket: state set on one connection is visible from
// another, and expiry is honoured end to end.
TEST_P(ServerFixture, SetGetAcrossConnections) {
  asio::io_context io;
  auto writer = connect(io);
  asio::write(writer, asio::buffer(std::string(
                          "*3\r\n$3\r\nSET\r\n$1\r\nk\r\n$5\r\nvalue\r\n")));
  EXPECT_EQ(read_exactly(writer, 5), "+OK\r\n");

  auto reader = connect(io);
  asio::write(reader,
              asio::buffer(std::string("*2\r\n$3\r\nGET\r\n$1\r\nk\r\n")));
  EXPECT_EQ(read_exactly(reader, 11), "$5\r\nvalue\r\n");
}

TEST_P(ServerFixture, KeyExpiresOverTheWire) {
  asio::io_context io;
  auto s = connect(io);
  asio::write(s, asio::buffer(std::string("*5\r\n$3\r\nSET\r\n$1\r\nk\r\n$1\r\nv"
                                          "\r\n$2\r\nPX\r\n$2\r\n50\r\n")));
  EXPECT_EQ(read_exactly(s, 5), "+OK\r\n");

  std::this_thread::sleep_for(std::chrono::milliseconds(120));
  asio::write(s, asio::buffer(std::string("*2\r\n$3\r\nGET\r\n$1\r\nk\r\n")));
  EXPECT_EQ(read_exactly(s, 5), "$-1\r\n");
}

// A multi-megabyte value exercises the read path across many recv() calls --
// impossible with the reference implementation's 1024-byte buffer.
TEST_P(ServerFixture, LargeValueRoundTrip) {
  const std::string value(4 * 1024 * 1024, 'z');
  const std::string request = "*3\r\n$3\r\nSET\r\n$3\r\nbig\r\n$" +
                              std::to_string(value.size()) + "\r\n" + value +
                              "\r\n";
  asio::io_context io;
  auto s = connect(io);
  std::thread writer([&] { asio::write(s, asio::buffer(request)); });
  EXPECT_EQ(read_exactly(s, 5), "+OK\r\n");
  writer.join();

  asio::write(s, asio::buffer(std::string("*2\r\n$3\r\nGET\r\n$3\r\nbig\r\n")));
  const std::string header = "$" + std::to_string(value.size()) + "\r\n";
  const std::string reply = read_exactly(s, header.size() + value.size() + 2);
  EXPECT_EQ(reply.substr(0, header.size()), header);
  EXPECT_EQ(reply.substr(header.size(), value.size()), value);
}

TEST_P(ServerFixture, RespondsToPing) {
  asio::io_context io;
  auto s = connect(io);
  asio::write(s, asio::buffer(std::string("*1\r\n$4\r\nPING\r\n")));
  EXPECT_EQ(read_exactly(s, 7), "+PONG\r\n");
}

// A command split across two TCP segments must produce exactly one reply.
// The legacy server would have parsed garbage here.
TEST_P(ServerFixture, HandlesRequestSplitAcrossPackets) {
  asio::io_context io;
  auto s = connect(io);
  asio::write(s, asio::buffer(std::string("*1\r\n$4\r\nPI")));
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  asio::write(s, asio::buffer(std::string("NG\r\n")));
  EXPECT_EQ(read_exactly(s, 7), "+PONG\r\n");
}

TEST_P(ServerFixture, HandlesDeepPipelining) {
  constexpr int kCount = 10000;
  std::string batch;
  for (int i = 0; i < kCount; ++i) batch += "*1\r\n$4\r\nPING\r\n";

  asio::io_context io;
  auto s = connect(io);
  asio::write(s, asio::buffer(batch));

  const std::string replies = read_exactly(s, 7 * kCount);
  EXPECT_EQ(replies.size(), 7u * kCount);
  EXPECT_EQ(replies.substr(0, 7), "+PONG\r\n");
  EXPECT_EQ(replies.substr(replies.size() - 7), "+PONG\r\n");
}

// Many replies queued without the client reading forces the deferred-write
// path (async_write / POLLOUT). An inline blocking send would deadlock or
// truncate here.
TEST_P(ServerFixture, LargeResponseIsFullyDelivered) {
  constexpr int kCount = 50000;
  std::string batch;
  for (int i = 0; i < kCount; ++i) batch += "*1\r\n$4\r\nPING\r\n";

  asio::io_context io;
  auto s = connect(io);
  std::thread writer([&] { asio::write(s, asio::buffer(batch)); });
  const std::string replies = read_exactly(s, 7 * kCount);
  writer.join();

  EXPECT_EQ(replies.size(), 7u * kCount);
}

TEST_P(ServerFixture, ManyConcurrentConnections) {
  constexpr int kConns = 200;
  asio::io_context io;
  std::vector<tcp::socket> sockets;
  sockets.reserve(kConns);
  for (int i = 0; i < kConns; ++i) sockets.push_back(connect(io));

  for (auto& s : sockets) {
    asio::write(s, asio::buffer(std::string("*1\r\n$4\r\nPING\r\n")));
  }
  for (auto& s : sockets) EXPECT_EQ(read_exactly(s, 7), "+PONG\r\n");

  EXPECT_GE(rp::Stats::instance().connections_peak.load(),
            static_cast<std::uint64_t>(kConns));
}

TEST_P(ServerFixture, DisconnectDuringTrafficDoesNotKillServer) {
  {
    asio::io_context io;
    auto s = connect(io);
    asio::write(s, asio::buffer(std::string("*1\r\n$4\r\nPING\r\n")));
    s.close();  // abandon without reading the reply
  }
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  asio::io_context io;
  auto s = connect(io);
  asio::write(s, asio::buffer(std::string("*1\r\n$4\r\nPING\r\n")));
  EXPECT_EQ(read_exactly(s, 7), "+PONG\r\n");
}

#if defined(__unix__) || defined(__APPLE__)
INSTANTIATE_TEST_SUITE_P(Backends, ServerFixture,
                         ::testing::Values(rp::Backend::kAsio,
                                           rp::Backend::kPoll));
#else
INSTANTIATE_TEST_SUITE_P(Backends, ServerFixture,
                         ::testing::Values(rp::Backend::kAsio));
#endif

}  // namespace
