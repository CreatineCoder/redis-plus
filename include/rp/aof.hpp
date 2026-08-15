#pragma once

#include <cstdint>
#include <cstdio>
#include <functional>
#include <string>
#include <vector>

namespace rp {

// Append-only file.
//
// The AOF is simply the stream of write commands in RESP -- the same encoding
// clients speak -- so replay is the existing parser plus the existing command
// table, with no second code path to keep in sync.
//
// Durability is a genuine trade, not a setting to hide: `always` fsyncs every
// write (slowest, loses nothing), `everysec` fsyncs once a second (fast, can
// lose up to a second of writes on a power cut), `no` leaves it to the OS
// (fastest, loses whatever the page cache held). Phase 4's benchmark reports
// the throughput cost of each.
enum class FsyncPolicy { kAlways, kEverySec, kNo };

FsyncPolicy parse_fsync_policy(const std::string& name, bool* ok);
const char* fsync_policy_name(FsyncPolicy policy);

class Aof {
 public:
  ~Aof() { close(); }

  bool open(const std::string& path, FsyncPolicy policy, std::string* error);
  bool is_open() const { return file_ != nullptr; }
  void close();

  // Buffer one write command. Not written to the OS until flush().
  void append(const std::vector<std::string>& args);

  // Push the buffer to the OS, then fsync if the policy is due. Called once
  // per event-loop iteration, so a batch of pipelined writes costs one write.
  bool flush(std::int64_t now_ms, std::string* error);

  // Replace the file with a minimal one that recreates `records` -- the AOF
  // otherwise grows without bound as keys are rewritten. Atomic: builds a
  // temp file and renames.
  bool rewrite(const std::string& path, const std::vector<struct Record>& records,
               std::string* error);

  std::uint64_t commands_written() const { return commands_written_; }
  std::uint64_t fsyncs() const { return fsyncs_; }
  std::uint64_t bytes_written() const { return bytes_written_; }

  // Replay a file, invoking `apply` for each complete command.
  //
  // A torn tail -- the last command half-written when the process died -- is
  // expected after a crash, not corruption: replay stops there, reports the
  // truncated byte count, and succeeds. A malformed command in the *middle*
  // is real corruption and fails.
  static bool replay(const std::string& path,
                     const std::function<void(const std::vector<std::string>&)>& apply,
                     std::uint64_t* commands_applied,
                     std::uint64_t* truncated_bytes, std::string* error);

  // RESP encoding of one command; also what Phase 5 streams to replicas.
  static std::string encode(const std::vector<std::string>& args);

 private:
  std::FILE* file_ = nullptr;
  std::string path_;
  FsyncPolicy policy_ = FsyncPolicy::kEverySec;
  std::string buffer_;
  std::int64_t last_fsync_ms_ = 0;
  std::uint64_t commands_written_ = 0;
  std::uint64_t fsyncs_ = 0;
  std::uint64_t bytes_written_ = 0;
};

}  // namespace rp
