#include "rp/persistence.hpp"

#include <utility>

#include "rp/rdb.hpp"

namespace rp {

Persistence::Persistence(std::shared_ptr<Store> store, PersistenceConfig config)
    : store_(std::move(store)), config_(std::move(config)) {
  last_save_ms_ = now_ms();
}

Persistence::~Persistence() { shutdown(); }

void Persistence::shutdown() {
  if (saver_.joinable()) saver_.join();
  aof_.close();
}

bool Persistence::load(RespHandler& handler, std::string* error) {
  if (config_.aof_enabled) {
    // The AOF is authoritative when enabled: it records every write, while a
    // snapshot is only as fresh as the last save. Loading both would double
    // apply.
    std::uint64_t applied = 0, truncated = 0;
    if (!Aof::replay(
            config_.aof_path(),
            [&handler](const Args& args) { handler.apply_silently(args); },
            &applied, &truncated, error)) {
      return false;
    }
    if (truncated > 0) {
      // Expected after a crash: the last command was half-written. The bytes
      // are dropped when the file is next rewritten.
      last_error_ = "recovered from a torn AOF tail (" +
                    std::to_string(truncated) + " bytes discarded)";
    }
    if (!aof_.open(config_.aof_path(), config_.fsync_policy, error)) {
      return false;
    }
    return true;
  }

  if (!config_.rdb_enabled) return true;

  std::vector<Record> records;
  if (!rdb_load_file(config_.rdb_path(), &records, error)) return false;
  for (const auto& record : records) store_->load_record(record);
  return true;
}

void Persistence::on_write(const Args& args) {
  ++dirty_;
  if (config_.aof_enabled) aof_.append(args);
}

bool Persistence::write_snapshot(const std::vector<Record>& records,
                                 std::string* error) {
  return rdb_save_file(config_.rdb_path(), records, error);
}

bool Persistence::save(std::string* error) {
  if (!config_.rdb_enabled) {
    if (error) *error = "RDB snapshots are disabled";
    return false;
  }
  last_save_attempt_ms_ = now_ms();
  if (!write_snapshot(store_->snapshot(), error)) {
    ++saves_failed_;
    if (error) last_error_ = *error;
    return false;
  }
  ++saves_ok_;
  dirty_ = 0;
  last_save_ms_ = now_ms();
  return true;
}

bool Persistence::background_save(std::string* error) {
  if (!config_.rdb_enabled) {
    if (error) *error = "RDB snapshots are disabled";
    return false;
  }
  if (saving_.load()) {
    if (error) *error = "Background save already in progress";
    return false;
  }

  // Snapshot on this thread (cheap, and the store is only safe to touch here),
  // then serialize and write on another. This is the copy real redis avoids
  // with fork+COW; the trade is documented in the header.
  auto records = std::make_shared<std::vector<Record>>(store_->snapshot());

  if (saver_.joinable()) saver_.join();
  saving_.store(true);
  background_failed_.store(false);
  dirty_ = 0;
  last_save_attempt_ms_ = now_ms();

  saver_ = std::thread([this, records] {
    std::string err;
    if (!write_snapshot(*records, &err)) background_failed_.store(true);
    saving_.store(false);
  });
  return true;
}

bool Persistence::rewrite_aof(std::string* error) {
  if (!config_.aof_enabled) {
    if (error) *error = "AOF is disabled";
    return false;
  }
  return aof_.rewrite(config_.aof_path(), store_->snapshot(), error);
}

void Persistence::cron() {
  const std::int64_t now = now_ms();

  if (config_.aof_enabled) {
    std::string error;
    if (!aof_.flush(now, &error)) last_error_ = error;

    if (config_.aof_rewrite_threshold > 0 &&
        aof_.commands_written() > config_.aof_rewrite_threshold) {
      std::string rewrite_error;
      if (!rewrite_aof(&rewrite_error)) last_error_ = rewrite_error;
    }
  }

  // Reap a finished background save so the thread is joined promptly rather
  // than at the next BGSAVE.
  if (!saving_.load() && saver_.joinable()) {
    saver_.join();
    if (background_failed_.load()) {
      ++saves_failed_;
      last_error_ = "background save failed";
    } else {
      ++saves_ok_;
      last_save_ms_ = now;
    }
  }

  if (config_.rdb_enabled && config_.save_seconds > 0 &&
      config_.save_changes > 0 && !saving_.load()) {
    const bool enough_changes =
        dirty_ >= static_cast<std::uint64_t>(config_.save_changes);
    const bool enough_time =
        now - last_save_ms_ >= config_.save_seconds * 1000LL;
    if (enough_changes && enough_time) {
      std::string error;
      background_save(&error);
    }
  }
}

std::string Persistence::info_section() {
  std::string out;
  out += "loading:0\r\n";
  out += "rdb_enabled:" + std::string(config_.rdb_enabled ? "1" : "0") + "\r\n";
  out += "rdb_changes_since_last_save:" + std::to_string(dirty_) + "\r\n";
  out += "rdb_bgsave_in_progress:" +
         std::string(saving_.load() ? "1" : "0") + "\r\n";
  out += "rdb_last_save_time:" + std::to_string(last_save_ms_ / 1000) + "\r\n";
  out += "rdb_saves_ok:" + std::to_string(saves_ok_) + "\r\n";
  out += "rdb_saves_failed:" + std::to_string(saves_failed_) + "\r\n";
  out += "aof_enabled:" + std::string(config_.aof_enabled ? "1" : "0") + "\r\n";
  out += "aof_fsync_policy:" +
         std::string(fsync_policy_name(config_.fsync_policy)) + "\r\n";
  out += "aof_commands_written:" + std::to_string(aof_.commands_written()) +
         "\r\n";
  out += "aof_bytes_written:" + std::to_string(aof_.bytes_written()) + "\r\n";
  out += "aof_fsyncs:" + std::to_string(aof_.fsyncs()) + "\r\n";
  if (!last_error_.empty()) {
    out += "last_persistence_message:" + last_error_ + "\r\n";
  }
  return out;
}

}  // namespace rp
