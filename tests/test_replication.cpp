// Replication tests: real masters and real replicas over real sockets.
//
// Nothing here stubs the transport. A replica performs the full handshake,
// receives an actual RDB payload and applies an actual command stream, because
// the failures worth catching (an off-by-two after the RDB, a lost first write,
// a divergent keyspace) only appear when the bytes are real.

#include "rp/replication.hpp"

#include <asio.hpp>
#include <gtest/gtest.h>

#include <chrono>
#include <functional>
#include <future>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "rp/commands.hpp"
#include "rp/server.hpp"
#include "rp/store.hpp"

namespace {

using namespace std::chrono_literals;

// One complete server: store, handler, replication, event loop, own thread.
struct Node {
  std::shared_ptr<rp::Store> store = std::make_shared<rp::Store>();
  std::shared_ptr<rp::RespHandler> handler;
  std::shared_ptr<rp::Replication> replication;
  std::unique_ptr<rp::Server> server;
  std::thread thread;
  std::uint16_t port = 0;

  void start() {
    handler = std::make_shared<rp::RespHandler>(store);
    replication = std::make_shared<rp::Replication>(store);

    rp::ServerConfig cfg;
    cfg.bind_address = "127.0.0.1";
    cfg.port = 0;
    cfg.cron_interval_ms = 20;

    auto store_ref = store;
    server = rp::make_server(rp::Backend::kAsio, cfg, handler,
                             [store_ref] { store_ref->active_expire_cycle(); });
    port = server->port();

    replication->attach(server.get());
    replication->set_listening_port(port);
    auto handler_ref = handler;
    replication->set_apply(
        [handler_ref](const rp::Args& a) { handler_ref->apply_silently(a); });

    handler->set_replication(replication.get());
    auto replication_ref = replication;
    handler->set_propagate(
        [replication_ref](const rp::Args& a) { replication_ref->propagate(a); });

    thread = std::thread([this] { server->run(); });
  }

  void stop() {
    if (replication) replication->stop_replication();
    if (server) server->stop();
    if (thread.joinable()) thread.join();
  }

  ~Node() { stop(); }

  // Read state on the event-loop thread; touching the store from the test
  // thread would be a race that happens to pass.
  template <typename F>
  auto on_loop(F fn) -> decltype(fn()) {
    std::promise<decltype(fn())> promise;
    auto future = promise.get_future();
    server->post([&] { promise.set_value(fn()); });
    EXPECT_EQ(future.wait_for(5s), std::future_status::ready);
    return future.get();
  }

  std::size_t key_count() { return on_loop([this] { return store->size(); }); }

  std::string get(const std::string& key) {
    return on_loop([this, &key] {
      const auto value = store->get(key);
      return value.value_or(std::string("<nil>"));
    });
  }
};

// Replication is asynchronous by design, so every assertion about a replica is
// necessarily "eventually". Polling with a deadline, never a fixed sleep.
bool eventually(const std::function<bool()>& predicate,
                std::chrono::milliseconds timeout = 5s) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    if (predicate()) return true;
    std::this_thread::sleep_for(10ms);
  }
  return predicate();
}

// A plain client, for the things only a client can test (READONLY, INFO).
std::string talk(std::uint16_t port, const std::vector<rp::Args>& commands,
                 std::size_t reply_bytes = 4096) {
  asio::io_context io;
  asio::ip::tcp::socket socket(io);
  socket.connect(asio::ip::tcp::endpoint(
      asio::ip::make_address("127.0.0.1"), port));

  std::string request;
  for (const auto& args : commands) {
    request += "*" + std::to_string(args.size()) + "\r\n";
    for (const auto& arg : args) {
      request += "$" + std::to_string(arg.size()) + "\r\n" + arg + "\r\n";
    }
  }
  asio::write(socket, asio::buffer(request));

  std::string reply(reply_bytes, '\0');
  asio::error_code ec;
  const std::size_t n = socket.read_some(asio::buffer(reply), ec);
  reply.resize(n);
  return reply;
}

struct ReplicationTest : public ::testing::Test {
  Node master;
  Node replica;

  void SetUp() override {
    master.start();
    replica.start();
  }
  void TearDown() override {
    replica.stop();
    master.stop();
  }

  void link() {
    std::string error;
    ASSERT_TRUE(replica.replication->replicaof("127.0.0.1", master.port, &error))
        << error;
    ASSERT_TRUE(eventually([this] {
      return replica.replication->link_state() == rp::LinkState::kConnected;
    })) << "replica never completed the handshake";
  }
};

// --- unit ------------------------------------------------------------------

TEST(ReplicationUnit, ClassifiesWriteCommands) {
  EXPECT_TRUE(rp::is_write_command("SET"));
  EXPECT_TRUE(rp::is_write_command("DEL"));
  EXPECT_TRUE(rp::is_write_command("FLUSHALL"));
  EXPECT_TRUE(rp::is_write_command("PEXPIREAT"));
  EXPECT_FALSE(rp::is_write_command("GET"));
  EXPECT_FALSE(rp::is_write_command("PING"));
  EXPECT_FALSE(rp::is_write_command("INFO"));
}

TEST(ReplicationUnit, MasterStartsWithAReplidAndZeroOffset) {
  auto store = std::make_shared<rp::Store>();
  rp::Replication replication(store);
  EXPECT_EQ(replication.replid().size(), 40u);
  EXPECT_EQ(replication.master_offset(), 0);
  EXPECT_FALSE(replication.is_replica());
}

// Two masters must not share a replication ID, or a replica could be pointed
// at the wrong one and believe it was already in sync.
TEST(ReplicationUnit, ReplidsAreDistinct) {
  auto store = std::make_shared<rp::Store>();
  rp::Replication a(store), b(store);
  EXPECT_NE(a.replid(), b.replid());
}

// --- full resync -----------------------------------------------------------

TEST_F(ReplicationTest, FullResyncTransfersExistingKeyspace) {
  master.on_loop([this] {
    for (int i = 0; i < 100; ++i) {
      master.store->set("pre:" + std::to_string(i), "v" + std::to_string(i));
    }
    return 0;
  });

  link();

  ASSERT_TRUE(eventually([this] { return replica.key_count() == 100; }))
      << "replica has " << replica.key_count() << " keys, expected 100";
  EXPECT_EQ(replica.get("pre:42"), "v42");
}

TEST_F(ReplicationTest, FullResyncOfAnEmptyMasterWorks) {
  link();
  EXPECT_EQ(replica.key_count(), 0u);
}

// A resync replaces the replica's keyspace: the master is authoritative, so
// data the replica had and the master does not must disappear.
TEST_F(ReplicationTest, FullResyncClearsStaleReplicaData) {
  replica.on_loop([this] {
    replica.store->set("stale", "should be gone");
    return 0;
  });
  master.on_loop([this] {
    master.store->set("real", "from master");
    return 0;
  });

  link();

  ASSERT_TRUE(eventually([this] { return replica.get("real") == "from master"; }));
  EXPECT_EQ(replica.get("stale"), "<nil>");
}

TEST_F(ReplicationTest, ExpiryDeadlinesSurviveFullResync) {
  master.on_loop([this] {
    master.store->set("ttl", "v", master.store->clock() + 60'000);
    return 0;
  });
  link();

  ASSERT_TRUE(eventually([this] { return replica.get("ttl") == "v"; }));
  const std::int64_t remaining =
      replica.on_loop([this] { return replica.store->pttl("ttl"); });
  EXPECT_GT(remaining, 50'000);
  EXPECT_LE(remaining, 60'000);
}

// --- streaming -------------------------------------------------------------

TEST_F(ReplicationTest, WritesAfterSyncReachTheReplica) {
  link();

  talk(master.port, {{"SET", "streamed", "yes"}});
  EXPECT_TRUE(eventually([this] { return replica.get("streamed") == "yes"; }))
      << "write never arrived at the replica";
}

TEST_F(ReplicationTest, ManyWritesArriveInOrderAndComplete) {
  link();

  std::vector<rp::Args> batch;
  for (int i = 0; i < 500; ++i) {
    batch.push_back({"SET", "k" + std::to_string(i), std::to_string(i)});
  }
  talk(master.port, batch, 1 << 16);

  ASSERT_TRUE(eventually([this] { return replica.key_count() == 500u; }))
      << "replica settled at " << replica.key_count() << " of 500 keys";
  EXPECT_EQ(replica.get("k0"), "0");
  EXPECT_EQ(replica.get("k499"), "499");
}

TEST_F(ReplicationTest, DeletesPropagate) {
  link();
  talk(master.port, {{"SET", "doomed", "v"}});
  ASSERT_TRUE(eventually([this] { return replica.get("doomed") == "v"; }));

  talk(master.port, {{"DEL", "doomed"}});
  EXPECT_TRUE(eventually([this] { return replica.get("doomed") == "<nil>"; }))
      << "deletion did not propagate";
}

// The master rewrites relative expiries to absolute before propagating. If it
// did not, the replica would compute its own deadline from its own clock and
// the two would drift apart.
TEST_F(ReplicationTest, RelativeExpiriesPropagateAsAbsoluteDeadlines) {
  link();
  talk(master.port, {{"SET", "k", "v", "EX", "100"}});
  ASSERT_TRUE(eventually([this] { return replica.get("k") == "v"; }));

  const std::int64_t master_ttl =
      master.on_loop([this] { return master.store->pttl("k"); });
  const std::int64_t replica_ttl =
      replica.on_loop([this] { return replica.store->pttl("k"); });
  EXPECT_LT(std::abs(master_ttl - replica_ttl), 1000)
      << "TTLs diverged: master " << master_ttl << " vs replica " << replica_ttl;
}

TEST_F(ReplicationTest, ReadsOnTheMasterAreNotPropagated) {
  link();
  talk(master.port, {{"SET", "k", "v"}});
  ASSERT_TRUE(eventually([this] { return replica.get("k") == "v"; }));

  const std::int64_t before = master.replication->master_offset();
  talk(master.port, {{"GET", "k"}, {"PING"}, {"DBSIZE"}});
  std::this_thread::sleep_for(200ms);
  EXPECT_EQ(master.replication->master_offset(), before)
      << "a read advanced the replication offset";
}

// --- offsets and INFO ------------------------------------------------------

TEST_F(ReplicationTest, OffsetAdvancesAndTheReplicaAcknowledges) {
  link();
  talk(master.port, {{"SET", "a", "1"}, {"SET", "b", "2"}});

  ASSERT_TRUE(eventually([this] { return master.replication->master_offset() > 0; }));
  EXPECT_TRUE(eventually(
      [this] {
        return replica.replication->replica_offset() ==
               master.replication->master_offset();
      },
      8s))
      << "offsets never converged: master "
      << master.replication->master_offset() << " vs replica "
      << replica.replication->replica_offset();
}

TEST_F(ReplicationTest, InfoReportsRolesAndReplicaCount) {
  link();
  ASSERT_TRUE(eventually([this] { return master.replication->replica_count() == 1; }));

  const std::string master_info = talk(master.port, {{"INFO"}}, 1 << 14);
  EXPECT_NE(master_info.find("role:master"), std::string::npos);
  EXPECT_NE(master_info.find("connected_slaves:1"), std::string::npos);
  EXPECT_NE(master_info.find("master_replid:"), std::string::npos);

  const std::string replica_info = talk(replica.port, {{"INFO"}}, 1 << 14);
  EXPECT_NE(replica_info.find("role:slave"), std::string::npos);
  EXPECT_NE(replica_info.find("master_link_status:up"), std::string::npos);
}

// --- read-only enforcement -------------------------------------------------

TEST_F(ReplicationTest, ReplicaRefusesClientWrites) {
  link();
  const std::string reply = talk(replica.port, {{"SET", "nope", "v"}});
  EXPECT_NE(reply.find("READONLY"), std::string::npos)
      << "replica accepted a client write; the datasets would fork. Got: "
      << reply;
}

TEST_F(ReplicationTest, ReplicaStillServesReads) {
  link();
  talk(master.port, {{"SET", "readable", "yes"}});
  ASSERT_TRUE(eventually([this] { return replica.get("readable") == "yes"; }));

  const std::string reply = talk(replica.port, {{"GET", "readable"}});
  EXPECT_NE(reply.find("yes"), std::string::npos);
}

// --- lifecycle -------------------------------------------------------------

TEST_F(ReplicationTest, ReplicaofNoOneStopsReplicating) {
  link();
  talk(master.port, {{"SET", "before", "v"}});
  ASSERT_TRUE(eventually([this] { return replica.get("before") == "v"; }));

  replica.replication->stop_replication();
  EXPECT_FALSE(replica.replication->is_replica());

  talk(master.port, {{"SET", "after", "v"}});
  std::this_thread::sleep_for(400ms);
  EXPECT_EQ(replica.get("after"), "<nil>")
      << "still receiving writes after REPLICAOF NO ONE";

  // And it accepts writes again once it is its own master.
  const std::string reply = talk(replica.port, {{"SET", "own", "v"}});
  EXPECT_NE(reply.find("+OK"), std::string::npos);
}

TEST_F(ReplicationTest, MasterDropsAReplicaThatDisconnects) {
  link();
  ASSERT_TRUE(eventually([this] { return master.replication->replica_count() == 1; }));

  replica.stop();
  EXPECT_TRUE(eventually([this] { return master.replication->replica_count() == 0; }))
      << "master kept feeding a dead replica";
}

// --- two replicas ----------------------------------------------------------

TEST(ReplicationFanout, TwoReplicasBothConverge) {
  Node master, first, second;
  master.start();
  first.start();
  second.start();

  master.on_loop([&] {
    master.store->set("seed", "0");
    return 0;
  });

  std::string error;
  ASSERT_TRUE(first.replication->replicaof("127.0.0.1", master.port, &error)) << error;
  ASSERT_TRUE(second.replication->replicaof("127.0.0.1", master.port, &error)) << error;
  ASSERT_TRUE(eventually([&] { return master.replication->replica_count() == 2; }));

  std::vector<rp::Args> batch;
  for (int i = 0; i < 200; ++i) {
    batch.push_back({"SET", "fan:" + std::to_string(i), std::to_string(i)});
  }
  talk(master.port, batch, 1 << 16);

  EXPECT_TRUE(eventually([&] { return first.key_count() == 201u; }, 8s))
      << "replica 1 has " << first.key_count();
  EXPECT_TRUE(eventually([&] { return second.key_count() == 201u; }, 8s))
      << "replica 2 has " << second.key_count();
  EXPECT_EQ(first.get("fan:199"), "199");
  EXPECT_EQ(second.get("fan:199"), "199");

  second.stop();
  first.stop();
  master.stop();
}

}  // namespace
