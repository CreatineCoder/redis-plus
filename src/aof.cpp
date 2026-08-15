#include "rp/aof.hpp"

#include <cstring>
#include <fstream>
#include <string_view>

#include "rp/resp.hpp"
#include "rp/store.hpp"

#if defined(_WIN32)
#include <io.h>
#else
#include <unistd.h>
#endif

namespace rp {
namespace {

bool fail(std::string* error, std::string message) {
  if (error) *error = std::move(message);
  return false;
}

void fsync_handle(std::FILE* file) {
  if (file == nullptr) return;
  std::fflush(file);
#if defined(_WIN32)
  ::_commit(::_fileno(file));
#else
  ::fsync(::fileno(file));
#endif
}

}  // namespace

FsyncPolicy parse_fsync_policy(const std::string& name, bool* ok) {
  *ok = true;
  if (name == "always") return FsyncPolicy::kAlways;
  if (name == "everysec") return FsyncPolicy::kEverySec;
  if (name == "no") return FsyncPolicy::kNo;
  *ok = false;
  return FsyncPolicy::kEverySec;
}

const char* fsync_policy_name(FsyncPolicy policy) {
  switch (policy) {
    case FsyncPolicy::kAlways: return "always";
    case FsyncPolicy::kNo: return "no";
    case FsyncPolicy::kEverySec:
    default: return "everysec";
  }
}

std::string Aof::encode(const std::vector<std::string>& args) {
  std::string out = reply::array_header(args.size());
  for (const auto& arg : args) out += reply::bulk(arg);
  return out;
}

bool Aof::open(const std::string& path, FsyncPolicy policy,
               std::string* error) {
  close();
  file_ = std::fopen(path.c_str(), "ab");
  if (file_ == nullptr) {
    return fail(error, "cannot open AOF " + path + " for appending");
  }
  path_ = path;
  policy_ = policy;
  buffer_.clear();
  return true;
}

void Aof::close() {
  if (file_ == nullptr) return;
  if (!buffer_.empty()) {
    std::fwrite(buffer_.data(), 1, buffer_.size(), file_);
    buffer_.clear();
  }
  fsync_handle(file_);
  std::fclose(file_);
  file_ = nullptr;
}

void Aof::append(const std::vector<std::string>& args) {
  if (file_ == nullptr || args.empty()) return;
  buffer_ += encode(args);
  ++commands_written_;
}

bool Aof::flush(std::int64_t now_ms, std::string* error) {
  if (file_ == nullptr) return true;

  if (!buffer_.empty()) {
    const std::size_t written =
        std::fwrite(buffer_.data(), 1, buffer_.size(), file_);
    if (written != buffer_.size()) {
      return fail(error, "short write to AOF " + path_);
    }
    bytes_written_ += written;
    buffer_.clear();
  }

  switch (policy_) {
    case FsyncPolicy::kAlways:
      fsync_handle(file_);
      ++fsyncs_;
      last_fsync_ms_ = now_ms;
      break;
    case FsyncPolicy::kEverySec:
      if (now_ms - last_fsync_ms_ >= 1000) {
        fsync_handle(file_);
        ++fsyncs_;
        last_fsync_ms_ = now_ms;
      }
      break;
    case FsyncPolicy::kNo:
      std::fflush(file_);
      break;
  }
  return true;
}

bool Aof::rewrite(const std::string& path, const std::vector<Record>& records,
                  std::string* error) {
  const std::string temp = path + ".rewrite";
  {
    std::ofstream out(temp, std::ios::binary | std::ios::trunc);
    if (!out) return fail(error, "cannot open " + temp);

    for (const auto& record : records) {
      // Absolute deadlines only. A rewrite that emitted a relative TTL would
      // silently extend every key's life by however long the file sat unused.
      std::vector<std::string> args = {"SET", record.key, record.value};
      if (record.expire_at != kNoExpiry) {
        args.push_back("PXAT");
        args.push_back(std::to_string(record.expire_at));
      }
      const std::string encoded = encode(args);
      out.write(encoded.data(), static_cast<std::streamsize>(encoded.size()));
    }
    out.flush();
    if (!out) {
      std::remove(temp.c_str());
      return fail(error, "write failed during AOF rewrite");
    }
  }

  const bool was_open = is_open();
  const FsyncPolicy policy = policy_;
  close();

  std::remove(path.c_str());
  if (std::rename(temp.c_str(), path.c_str()) != 0) {
    std::remove(temp.c_str());
    if (was_open) open(path, policy, nullptr);
    return fail(error, "rename failed during AOF rewrite");
  }

  commands_written_ = records.size();
  return was_open ? open(path, policy, error) : true;
}

bool Aof::replay(const std::string& path,
                 const std::function<void(const std::vector<std::string>&)>& apply,
                 std::uint64_t* commands_applied, std::uint64_t* truncated_bytes,
                 std::string* error) {
  if (commands_applied) *commands_applied = 0;
  if (truncated_bytes) *truncated_bytes = 0;

  std::ifstream file(path, std::ios::binary);
  if (!file) return true;  // no AOF yet

  const std::string data((std::istreambuf_iterator<char>(file)),
                         std::istreambuf_iterator<char>());
  std::string_view remaining(data);

  while (!remaining.empty()) {
    const ParseResult result = parse_request(remaining);

    if (result.status == ParseStatus::kIncomplete) {
      // Torn tail: the process died mid-write. Expected after a crash.
      if (truncated_bytes) *truncated_bytes = remaining.size();
      break;
    }
    if (result.status == ParseStatus::kError) {
      return fail(error, "corrupt AOF at offset " +
                             std::to_string(data.size() - remaining.size()) +
                             ": " + result.error);
    }

    if (!result.args.empty()) {
      apply(result.args);
      if (commands_applied) ++*commands_applied;
    }
    remaining.remove_prefix(result.consumed);
  }

  return true;
}

}  // namespace rp
