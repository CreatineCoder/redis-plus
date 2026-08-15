#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "rp/aof.hpp"
#include "rp/commands.hpp"
#include "rp/store.hpp"

namespace rp {

struct PersistenceConfig {
  std::string dir = ".";
  std::string dbfilename = "dump.rdb";
  std::string aof_filename = "appendonly.aof";

  bool rdb_enabled = true;
  bool aof_enabled = false;  // redis default
  FsyncPolicy fsync_policy = FsyncPolicy::kEverySec;

  // Snapshot when at least `save_changes` writes have happened in the last
  // `save_seconds`. 0 disables automatic snapshots.
  int save_seconds = 300;
  std::int64_t save_changes = 100;

  // Rewrite the AOF once it exceeds this many commands. 0 disables.
  std::uint64_t aof_rewrite_threshold = 1'000'000;

  std::string rdb_path() const { return dir + "/" + dbfilename; }
  std::string aof_path() const { return dir + "/" + aof_filename; }
};

// Owns durability: loads state at boot, records every write, and snapshots on
// a schedule.
//
// Everything here runs on the event-loop thread except the background save,
// which works from a copy. Real redis forks and lets copy-on-write share the
// pages; a portable build cannot fork, so BGSAVE here copies the keyspace
// first. That is an honest memory cost -- roughly the size of the live data
// during the save -- and it is why `background_save` refuses to start a second
// one while the first is running.
class Persistence : public PersistenceOps {
 public:
  Persistence(std::shared_ptr<Store> store, PersistenceConfig config);
  ~Persistence() override;

  // Restore at boot: the AOF wins when enabled, because it is the more recent
  // and more complete record. Returns false only on genuine corruption; a
  // missing file is a fresh server.
  bool load(RespHandler& handler, std::string* error);

  // Record a write. Must be the propagated (canonical) form.
  void on_write(const Args& args);

  // Called from the server cron: flushes the AOF, honours the fsync policy,
  // and triggers scheduled snapshots and rewrites.
  void cron();

  // PersistenceOps
  bool save(std::string* error) override;
  bool background_save(std::string* error) override;
  bool rewrite_aof(std::string* error) override;
  std::string info_section() override;

  std::uint64_t dirty_writes() const { return dirty_; }
  bool background_save_in_progress() const { return saving_.load(); }

  // Blocks until any in-flight background save finishes. Call before exit.
  void shutdown();

 private:
  bool write_snapshot(const std::vector<Record>& records, std::string* error);

  std::shared_ptr<Store> store_;
  PersistenceConfig config_;
  Aof aof_;

  std::uint64_t dirty_ = 0;          // writes since the last successful save
  std::int64_t last_save_ms_ = 0;
  std::int64_t last_save_attempt_ms_ = 0;
  std::uint64_t saves_ok_ = 0;
  std::uint64_t saves_failed_ = 0;
  std::string last_error_;

  std::atomic<bool> saving_{false};
  std::atomic<bool> background_failed_{false};
  std::thread saver_;
};

}  // namespace rp
