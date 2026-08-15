// Persistence tests: AOF encoding/replay, and the restore-at-boot path.
//
// The property that matters is durability across a restart, so most of these
// build a server's state, throw the objects away, and rebuild from the files
// alone -- the same thing a real restart does.

#include "rp/persistence.hpp"

#include <gtest/gtest.h>

#include <cstdio>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include "rp/aof.hpp"
#include "rp/commands.hpp"
#include "rp/rdb.hpp"
#include "rp/store.hpp"

namespace {

struct PersistenceTest : public ::testing::Test {
  rp::PersistenceConfig config;

  void SetUp() override {
    config.dir = ".";
    config.dbfilename = "test_persist.rdb";
    config.aof_filename = "test_persist.aof";
    cleanup();
  }
  void TearDown() override { cleanup(); }

  void cleanup() {
    std::remove(config.rdb_path().c_str());
    std::remove(config.aof_path().c_str());
    std::remove((config.aof_path() + ".rewrite").c_str());
  }

  // One server's lifetime: build it, run commands through it, tear it down.
  struct Instance {
    std::shared_ptr<rp::Store> store;
    std::shared_ptr<rp::RespHandler> handler;
    std::shared_ptr<rp::Persistence> persistence;

    void run(const rp::Args& args) {
      rp::Buffer in, out;
      in.append(rp::Aof::encode(args));
      handler->on_data(in, out);
    }
  };

  Instance boot(std::string* error = nullptr) {
    Instance in;
    in.store = std::make_shared<rp::Store>();
    in.handler = std::make_shared<rp::RespHandler>(in.store);
    in.persistence = std::make_shared<rp::Persistence>(in.store, config);

    std::string local;
    const bool ok = in.persistence->load(*in.handler, error ? error : &local);
    if (error == nullptr) EXPECT_TRUE(ok) << local;

    auto persistence = in.persistence;
    in.handler->set_persistence(persistence.get());
    in.handler->set_propagate(
        [persistence](const rp::Args& args) { persistence->on_write(args); });
    return in;
  }
};

// --- AOF encoding ----------------------------------------------------------

TEST_F(PersistenceTest, AofEncodingIsResp) {
  EXPECT_EQ(rp::Aof::encode({"SET", "k", "v"}),
            "*3\r\n$3\r\nSET\r\n$1\r\nk\r\n$1\r\nv\r\n");
}

TEST_F(PersistenceTest, AofReplayReadsBackWhatWasWritten) {
  {
    rp::Aof aof;
    std::string error;
    ASSERT_TRUE(aof.open(config.aof_path(), rp::FsyncPolicy::kAlways, &error))
        << error;
    aof.append({"SET", "a", "1"});
    aof.append({"SET", "b", "2"});
    ASSERT_TRUE(aof.flush(0, &error)) << error;
  }

  std::vector<rp::Args> seen;
  std::uint64_t applied = 0, truncated = 0;
  std::string error;
  ASSERT_TRUE(rp::Aof::replay(
      config.aof_path(), [&](const rp::Args& a) { seen.push_back(a); },
      &applied, &truncated, &error))
      << error;

  EXPECT_EQ(applied, 2u);
  EXPECT_EQ(truncated, 0u);
  ASSERT_EQ(seen.size(), 2u);
  EXPECT_EQ(seen[1][1], "b");
}

// A process killed mid-write leaves a partial command. That is the normal
// crash case, not corruption: replay must recover everything before it.
TEST_F(PersistenceTest, AofTornTailIsRecoveredNotRejected) {
  {
    std::ofstream f(config.aof_path(), std::ios::binary);
    f << "*3\r\n$3\r\nSET\r\n$1\r\na\r\n$1\r\n1\r\n";
    f << "*3\r\n$3\r\nSET\r\n$1\r\nb";  // died here
  }

  std::vector<rp::Args> seen;
  std::uint64_t applied = 0, truncated = 0;
  std::string error;
  ASSERT_TRUE(rp::Aof::replay(
      config.aof_path(), [&](const rp::Args& a) { seen.push_back(a); },
      &applied, &truncated, &error))
      << error;

  EXPECT_EQ(applied, 1u);
  EXPECT_GT(truncated, 0u);
}

// Garbage in the middle is not a torn tail -- it means the file is damaged,
// and silently skipping it would resurrect a keyspace that never existed.
TEST_F(PersistenceTest, AofCorruptionInTheMiddleIsRejected) {
  {
    std::ofstream f(config.aof_path(), std::ios::binary);
    f << "*3\r\n$3\r\nSET\r\n$1\r\na\r\n$1\r\n1\r\n";
    f << "*XX\r\n";
    f << "*3\r\n$3\r\nSET\r\n$1\r\nb\r\n$1\r\n2\r\n";
  }

  std::uint64_t applied = 0, truncated = 0;
  std::string error;
  EXPECT_FALSE(rp::Aof::replay(
      config.aof_path(), [](const rp::Args&) {}, &applied, &truncated, &error));
  EXPECT_NE(error.find("corrupt"), std::string::npos);
}

// --- restart durability ----------------------------------------------------

TEST_F(PersistenceTest, RdbSurvivesRestart) {
  {
    auto server = boot();
    server.run({"SET", "alpha", "one"});
    server.run({"SET", "beta", "two"});
    std::string error;
    ASSERT_TRUE(server.persistence->save(&error)) << error;
  }

  auto restarted = boot();
  EXPECT_EQ(restarted.store->size(), 2u);
  ASSERT_TRUE(restarted.store->get("alpha").has_value());
  EXPECT_EQ(*restarted.store->get("alpha"), "one");
}

// Writes made after the last snapshot are gone. Stating this in a test keeps
// anyone from claiming RDB alone is crash-safe.
TEST_F(PersistenceTest, RdbLosesWritesMadeAfterTheLastSave) {
  {
    auto server = boot();
    server.run({"SET", "saved", "yes"});
    std::string error;
    ASSERT_TRUE(server.persistence->save(&error)) << error;
    server.run({"SET", "unsaved", "lost"});
  }

  auto restarted = boot();
  EXPECT_TRUE(restarted.store->get("saved").has_value());
  EXPECT_FALSE(restarted.store->get("unsaved").has_value());
}

TEST_F(PersistenceTest, AofSurvivesRestartWithoutAnExplicitSave) {
  config.aof_enabled = true;
  {
    auto server = boot();
    server.run({"SET", "a", "1"});
    server.run({"SET", "b", "2"});
    server.run({"DEL", "a"});
    server.persistence->cron();  // flush
    server.persistence->shutdown();
  }

  auto restarted = boot();
  EXPECT_FALSE(restarted.store->get("a").has_value()) << "DEL was not replayed";
  ASSERT_TRUE(restarted.store->get("b").has_value());
  EXPECT_EQ(*restarted.store->get("b"), "2");
}

// The reason relative expiries are rewritten to absolute before persisting:
// replaying `SET k v EX 60` an hour later must not give the key another
// minute of life.
TEST_F(PersistenceTest, ExpiryDeadlinesAreAbsoluteAcrossRestart) {
  config.aof_enabled = true;
  std::int64_t deadline = 0;
  {
    auto server = boot();
    server.run({"SET", "k", "v", "EX", "600"});
    deadline = server.store->clock() + server.store->pttl("k");
    server.persistence->cron();
    server.persistence->shutdown();
  }

  auto restarted = boot();
  const std::int64_t after = restarted.store->clock() + restarted.store->pttl("k");
  EXPECT_NEAR(static_cast<double>(after), static_cast<double>(deadline), 50.0)
      << "TTL drifted across restart";
}

TEST_F(PersistenceTest, AlreadyExpiredKeysAreNotRestored) {
  {
    auto server = boot();
    server.run({"SET", "gone", "v", "PXAT", "1"});  // deadline in 1970
    server.run({"SET", "here", "v"});
    std::string error;
    ASSERT_TRUE(server.persistence->save(&error)) << error;
  }

  auto restarted = boot();
  EXPECT_FALSE(restarted.store->get("gone").has_value());
  EXPECT_TRUE(restarted.store->get("here").has_value());
}

TEST_F(PersistenceTest, AofRewriteCompactsButPreservesState) {
  config.aof_enabled = true;
  {
    auto server = boot();
    for (int i = 0; i < 500; ++i) {
      server.run({"SET", "counter", std::to_string(i)});  // same key, 500 times
    }
    server.run({"SET", "other", "x"});
    server.persistence->cron();

    std::string error;
    ASSERT_TRUE(server.persistence->rewrite_aof(&error)) << error;
    server.persistence->shutdown();
  }

  std::uint64_t applied = 0, truncated = 0;
  std::string error;
  ASSERT_TRUE(rp::Aof::replay(config.aof_path(), [](const rp::Args&) {},
                              &applied, &truncated, &error))
      << error;
  EXPECT_EQ(applied, 2u) << "rewrite should collapse to one command per key";

  auto restarted = boot();
  ASSERT_TRUE(restarted.store->get("counter").has_value());
  EXPECT_EQ(*restarted.store->get("counter"), "499");
  EXPECT_TRUE(restarted.store->get("other").has_value());
}

// Refusing to start beats starting with silently missing data.
TEST_F(PersistenceTest, BootFailsLoudlyOnACorruptSnapshot) {
  {
    auto server = boot();
    server.run({"SET", "k", "v"});
    std::string error;
    ASSERT_TRUE(server.persistence->save(&error)) << error;
  }
  {
    std::fstream f(config.rdb_path(), std::ios::binary | std::ios::in | std::ios::out);
    f.seekp(11);
    f.put('\x7F');
  }

  std::string error;
  auto server = boot(&error);
  EXPECT_FALSE(error.empty()) << "corrupt snapshot was accepted silently";
}

TEST_F(PersistenceTest, BackgroundSaveCompletes) {
  auto server = boot();
  for (int i = 0; i < 1000; ++i) {
    server.run({"SET", "k" + std::to_string(i), "v"});
  }

  std::string error;
  ASSERT_TRUE(server.persistence->background_save(&error)) << error;
  while (server.persistence->background_save_in_progress()) {
    server.persistence->cron();
  }
  server.persistence->shutdown();

  std::vector<rp::Record> records;
  ASSERT_TRUE(rp::rdb_load_file(config.rdb_path(), &records, &error)) << error;
  EXPECT_EQ(records.size(), 1000u);
}

TEST_F(PersistenceTest, SecondBackgroundSaveIsRefusedWhileOneRuns) {
  auto server = boot();
  server.run({"SET", "k", "v"});

  std::string error;
  ASSERT_TRUE(server.persistence->background_save(&error));
  if (server.persistence->background_save_in_progress()) {
    std::string second;
    EXPECT_FALSE(server.persistence->background_save(&second));
  }
  server.persistence->shutdown();
}

TEST_F(PersistenceTest, DirtyCounterTracksUnsavedWrites) {
  auto server = boot();
  EXPECT_EQ(server.persistence->dirty_writes(), 0u);
  server.run({"SET", "a", "1"});
  server.run({"GET", "a"});  // reads do not count
  EXPECT_EQ(server.persistence->dirty_writes(), 1u);

  std::string error;
  ASSERT_TRUE(server.persistence->save(&error)) << error;
  EXPECT_EQ(server.persistence->dirty_writes(), 0u);
}

}  // namespace
