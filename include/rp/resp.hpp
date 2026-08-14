#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace rp {

// RESP request parsing.
//
// The whole point of this layer is that it is *incremental* and *total*: it
// resumes across arbitrary read boundaries, and no byte sequence -- however
// malformed or hostile -- may read out of bounds or crash. The reference
// implementation failed both: it walked `while (arr[pos] != '\r')` off the end
// of a fixed array on any truncated input.
enum class ParseStatus {
  kOk,          // a complete request is in `args`
  kIncomplete,  // need more bytes; nothing consumed
  kError,       // protocol violation; `error` set, connection must be closed
};

struct ParseResult {
  ParseStatus status = ParseStatus::kIncomplete;
  std::size_t consumed = 0;   // bytes to remove from the input buffer
  std::vector<std::string> args;
  std::string error;
};

// Protocol limits, matching real redis. Exceeding them is a protocol error
// rather than an allocation.
inline constexpr std::int64_t kMaxMultibulkLength = 1024 * 1024;
inline constexpr std::int64_t kMaxBulkLength = 512ll * 1024 * 1024;
inline constexpr std::size_t kMaxInlineLength = 64 * 1024;

// Parse at most one request from the front of `in`.
// Never throws. On kOk, `consumed` bytes form exactly one complete request.
ParseResult parse_request(std::string_view in);

// ---------------------------------------------------------------------------
// Reply encoding
// ---------------------------------------------------------------------------

namespace reply {

inline std::string simple(std::string_view s) {
  return "+" + std::string(s) + "\r\n";
}
inline std::string error(std::string_view s) {
  return "-" + std::string(s) + "\r\n";
}
inline std::string integer(std::int64_t v) {
  return ":" + std::to_string(v) + "\r\n";
}
inline std::string bulk(std::string_view s) {
  return "$" + std::to_string(s.size()) + "\r\n" + std::string(s) + "\r\n";
}
inline std::string null_bulk() { return "$-1\r\n"; }
inline std::string array_header(std::size_t n) {
  return "*" + std::to_string(n) + "\r\n";
}
inline std::string empty_array() { return "*0\r\n"; }

inline const char* ok() { return "+OK\r\n"; }

std::string wrong_arity(std::string_view command);

}  // namespace reply

// Glob-style pattern match, as used by KEYS. Supports * ? [...] [^...] and
// backslash escapes. Iterative on `*` to avoid pathological recursion.
bool glob_match(std::string_view pattern, std::string_view str);

}  // namespace rp
