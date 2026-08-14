#include "rp/resp.hpp"

#include <cctype>
#include <limits>

namespace rp {
namespace {

constexpr std::string_view kCrlf = "\r\n";

ParseResult incomplete() { return {ParseStatus::kIncomplete, 0, {}, ""}; }

ParseResult protocol_error(std::string msg) {
  ParseResult r;
  r.status = ParseStatus::kError;
  r.error = std::move(msg);
  return r;
}

// Parse a signed decimal integer occupying exactly [begin, end) of `in`.
// Rejects empty input, stray signs, non-digits and overflow -- all of which
// the reference implementation's stoi() either accepted or threw on.
bool parse_int(std::string_view text, std::int64_t& out) {
  if (text.empty()) return false;

  std::size_t i = 0;
  bool negative = false;
  if (text[0] == '-' || text[0] == '+') {
    negative = (text[0] == '-');
    i = 1;
    if (text.size() == 1) return false;
  }

  std::int64_t value = 0;
  for (; i < text.size(); ++i) {
    const unsigned char c = static_cast<unsigned char>(text[i]);
    if (!std::isdigit(c)) return false;
    const int digit = c - '0';
    if (value > (std::numeric_limits<std::int64_t>::max() - digit) / 10) {
      return false;  // overflow
    }
    value = value * 10 + digit;
  }
  out = negative ? -value : value;
  return true;
}

// Inline command: a bare CRLF-terminated line, whitespace separated.
// redis-cli in raw mode and telnet users produce these.
ParseResult parse_inline(std::string_view in) {
  const std::size_t eol = in.find(kCrlf);
  if (eol == std::string_view::npos) {
    if (in.size() > kMaxInlineLength) {
      return protocol_error("ERR Protocol error: too big inline request");
    }
    return incomplete();
  }
  if (eol > kMaxInlineLength) {
    return protocol_error("ERR Protocol error: too big inline request");
  }

  ParseResult r;
  r.status = ParseStatus::kOk;
  r.consumed = eol + kCrlf.size();

  const std::string_view line = in.substr(0, eol);
  std::size_t i = 0;
  while (i < line.size()) {
    while (i < line.size() &&
           std::isspace(static_cast<unsigned char>(line[i]))) {
      ++i;
    }
    const std::size_t start = i;
    while (i < line.size() &&
           !std::isspace(static_cast<unsigned char>(line[i]))) {
      ++i;
    }
    if (i > start) r.args.emplace_back(line.substr(start, i - start));
  }
  return r;  // an empty line yields zero args; the caller ignores it
}

}  // namespace

ParseResult parse_request(std::string_view in) {
  if (in.empty()) return incomplete();
  if (in.front() != '*') return parse_inline(in);

  std::size_t pos = 0;
  const std::size_t header_end = in.find(kCrlf, pos);
  if (header_end == std::string_view::npos) {
    // Guard against a client that streams digits forever without a CRLF.
    if (in.size() > 64) {
      return protocol_error("ERR Protocol error: too big mbulk count string");
    }
    return incomplete();
  }

  std::int64_t argc = 0;
  if (!parse_int(in.substr(1, header_end - 1), argc)) {
    return protocol_error("ERR Protocol error: invalid multibulk length");
  }
  if (argc > kMaxMultibulkLength) {
    return protocol_error("ERR Protocol error: invalid multibulk length");
  }
  pos = header_end + kCrlf.size();

  if (argc <= 0) {  // "*0\r\n" and "*-1\r\n" are no-ops, not errors
    ParseResult r;
    r.status = ParseStatus::kOk;
    r.consumed = pos;
    return r;
  }

  std::vector<std::string> args;
  args.reserve(static_cast<std::size_t>(argc < 1024 ? argc : 1024));

  for (std::int64_t i = 0; i < argc; ++i) {
    if (pos >= in.size()) return incomplete();
    if (in[pos] != '$') {
      return protocol_error(std::string("ERR Protocol error: expected '$', got '") +
                            in[pos] + "'");
    }

    const std::size_t len_end = in.find(kCrlf, pos);
    if (len_end == std::string_view::npos) {
      if (in.size() - pos > 64) {
        return protocol_error("ERR Protocol error: too big bulk count string");
      }
      return incomplete();
    }

    std::int64_t len = 0;
    if (!parse_int(in.substr(pos + 1, len_end - pos - 1), len) || len < 0 ||
        len > kMaxBulkLength) {
      return protocol_error("ERR Protocol error: invalid bulk length");
    }

    const std::size_t body = len_end + kCrlf.size();
    const std::size_t need = body + static_cast<std::size_t>(len) + kCrlf.size();
    if (in.size() < need) return incomplete();  // body still in flight
    if (in.substr(body + static_cast<std::size_t>(len), 2) != kCrlf) {
      return protocol_error("ERR Protocol error: unbalanced bulk payload");
    }

    args.emplace_back(in.substr(body, static_cast<std::size_t>(len)));
    pos = need;
  }

  ParseResult r;
  r.status = ParseStatus::kOk;
  r.consumed = pos;
  r.args = std::move(args);
  return r;
}

namespace reply {

std::string wrong_arity(std::string_view command) {
  return error("ERR wrong number of arguments for '" + std::string(command) +
               "' command");
}

}  // namespace reply

bool glob_match(std::string_view pattern, std::string_view str) {
  std::size_t p = 0, s = 0;
  std::size_t star_p = std::string_view::npos, star_s = 0;

  while (s < str.size()) {
    if (p < pattern.size()) {
      const char pc = pattern[p];

      if (pc == '*') {
        star_p = p++;
        star_s = s;
        continue;
      }
      if (pc == '?') {
        ++p;
        ++s;
        continue;
      }
      if (pc == '[') {
        std::size_t q = p + 1;
        bool negate = false;
        if (q < pattern.size() && pattern[q] == '^') {
          negate = true;
          ++q;
        }
        bool matched = false;
        while (q < pattern.size() && pattern[q] != ']') {
          if (pattern[q] == '\\' && q + 1 < pattern.size()) {
            ++q;
            if (pattern[q] == str[s]) matched = true;
          } else if (q + 2 < pattern.size() && pattern[q + 1] == '-' &&
                     pattern[q + 2] != ']') {
            char lo = pattern[q], hi = pattern[q + 2];
            if (lo > hi) std::swap(lo, hi);
            if (str[s] >= lo && str[s] <= hi) matched = true;
            q += 2;
          } else if (pattern[q] == str[s]) {
            matched = true;
          }
          ++q;
        }
        if (negate) matched = !matched;
        if (matched) {
          p = (q < pattern.size()) ? q + 1 : q;
          ++s;
          continue;
        }
      } else {
        char literal = pc;
        std::size_t next = p + 1;
        if (pc == '\\' && p + 1 < pattern.size()) {
          literal = pattern[p + 1];
          next = p + 2;
        }
        if (literal == str[s]) {
          p = next;
          ++s;
          continue;
        }
      }
    }

    // Mismatch: backtrack to the last '*' and let it swallow one more char.
    if (star_p != std::string_view::npos) {
      p = star_p + 1;
      s = ++star_s;
      continue;
    }
    return false;
  }

  while (p < pattern.size() && pattern[p] == '*') ++p;
  return p == pattern.size();
}

}  // namespace rp
