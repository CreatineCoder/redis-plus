#include "rp/handler.hpp"

#include <cctype>
#include <string>
#include <string_view>

#include "rp/stats.hpp"

namespace rp {
namespace {

// Minimal, deliberately incomplete request framing: find a complete RESP array
// or inline command and report its length. Phase 2 replaces this entirely with
// a real incremental parser -- this only needs to be correct enough that
// redis-benchmark PING and redis-cli PING/ECHO work, so Phase 1's connection
// layer can be measured.
//
// Returns 0 if the buffer does not yet hold a complete request.
std::size_t framed_length(std::string_view in) {
  if (in.empty()) return 0;

  if (in.front() != '*') {  // inline command, terminated by CRLF
    const std::size_t eol = in.find("\r\n");
    return eol == std::string_view::npos ? 0 : eol + 2;
  }

  std::size_t pos = in.find("\r\n");
  if (pos == std::string_view::npos) return 0;

  long long argc = 0;
  const auto count = in.substr(1, pos - 1);
  for (const char c : count) {
    if (!std::isdigit(static_cast<unsigned char>(c))) return 0;
    argc = argc * 10 + (c - '0');
    if (argc > 1024 * 1024) return 0;
  }
  pos += 2;

  for (long long i = 0; i < argc; ++i) {
    if (pos >= in.size() || in[pos] != '$') return 0;
    const std::size_t hdr = in.find("\r\n", pos);
    if (hdr == std::string_view::npos) return 0;

    long long len = 0;
    for (std::size_t j = pos + 1; j < hdr; ++j) {
      if (!std::isdigit(static_cast<unsigned char>(in[j]))) return 0;
      len = len * 10 + (in[j] - '0');
      if (len > 512ll * 1024 * 1024) return 0;
    }
    pos = hdr + 2 + static_cast<std::size_t>(len) + 2;
    if (pos > in.size()) return 0;  // body not fully arrived yet
  }
  return pos;
}

bool contains_ci(std::string_view haystack, std::string_view needle) {
  if (needle.size() > haystack.size()) return false;
  for (std::size_t i = 0; i + needle.size() <= haystack.size(); ++i) {
    bool ok = true;
    for (std::size_t j = 0; j < needle.size(); ++j) {
      if (std::toupper(static_cast<unsigned char>(haystack[i + j])) !=
          needle[j]) {
        ok = false;
        break;
      }
    }
    if (ok) return true;
  }
  return false;
}

}  // namespace

std::size_t PingPongHandler::on_data(Buffer& in, Buffer& out) {
  std::size_t handled = 0;
  for (;;) {
    const std::string_view view = in.readable();
    const std::size_t n = framed_length(view);
    if (n == 0) break;

    const std::string_view request = view.substr(0, n);
    if (contains_ci(request, "COMMAND")) {
      out.append("*0\r\n");
    } else if (contains_ci(request, "INFO")) {
      const std::string info = Stats::instance().to_info();
      out.append("$" + std::to_string(info.size()) + "\r\n");
      out.append(info);
      out.append("\r\n");
    } else {
      out.append("+PONG\r\n");
    }

    in.consume(n);
    ++handled;
  }
  return handled;
}

}  // namespace rp
