#include "rp/commands.hpp"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>

#include "rp/resp.hpp"
#include "rp/stats.hpp"

namespace rp {
namespace {

std::string upper(std::string_view s) {
  std::string out(s);
  std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) {
    return static_cast<char>(std::toupper(c));
  });
  return out;
}

bool iequals(std::string_view a, std::string_view b) {
  if (a.size() != b.size()) return false;
  for (std::size_t i = 0; i < a.size(); ++i) {
    if (std::toupper(static_cast<unsigned char>(a[i])) !=
        std::toupper(static_cast<unsigned char>(b[i]))) {
      return false;
    }
  }
  return true;
}

bool to_int64(const std::string& s, std::int64_t& out) {
  if (s.empty() || s.size() > 20) return false;
  std::size_t i = (s[0] == '-' || s[0] == '+') ? 1 : 0;
  if (i == s.size()) return false;
  std::int64_t v = 0;
  for (; i < s.size(); ++i) {
    if (!std::isdigit(static_cast<unsigned char>(s[i]))) return false;
    const int d = s[i] - '0';
    if (v > (std::numeric_limits<std::int64_t>::max() - d) / 10) return false;
    v = v * 10 + d;
  }
  out = (s[0] == '-') ? -v : v;
  return true;
}

const char* kNotInteger = "ERR value is not an integer or out of range";
const char* kSyntaxError = "ERR syntax error";
const char* kInvalidExpire = "ERR invalid expire time in 'set' command";

// ---------------------------------------------------------------------------

std::string cmd_ping(const Args& a) {
  if (a.size() == 1) return reply::simple("PONG");
  if (a.size() == 2) return reply::bulk(a[1]);
  return reply::wrong_arity("ping");
}

std::string cmd_echo(const Args& a) {
  if (a.size() != 2) return reply::wrong_arity("echo");
  return reply::bulk(a[1]);
}

// SET key value [EX s | PX ms | EXAT ts | PXAT ts] [NX|XX] [KEEPTTL] [GET]
std::string cmd_set(Store& store, const Args& a) {
  if (a.size() < 3) return reply::wrong_arity("set");

  std::int64_t expire_at = kNoExpiry;
  bool keepttl = false, nx = false, xx = false, get = false;

  for (std::size_t i = 3; i < a.size(); ++i) {
    const std::string opt = upper(a[i]);
    const bool relative = (opt == "EX" || opt == "PX");
    const bool absolute = (opt == "EXAT" || opt == "PXAT");

    if (relative || absolute) {
      if (expire_at != kNoExpiry || keepttl) return reply::error(kSyntaxError);
      if (i + 1 >= a.size()) return reply::error(kSyntaxError);
      std::int64_t n = 0;
      if (!to_int64(a[++i], n)) return reply::error(kNotInteger);

      const bool seconds = (opt == "EX" || opt == "EXAT");
      if (seconds) {
        if (n > std::numeric_limits<std::int64_t>::max() / 1000) {
          return reply::error(kInvalidExpire);
        }
        n *= 1000;
      }
      // A non-positive relative TTL is an error; redis rejects it rather than
      // storing a key that is born dead.
      if (relative && n <= 0) return reply::error(kInvalidExpire);
      expire_at = relative ? store.clock() + n : n;
    } else if (opt == "KEEPTTL") {
      if (expire_at != kNoExpiry) return reply::error(kSyntaxError);
      keepttl = true;
    } else if (opt == "NX") {
      if (xx) return reply::error(kSyntaxError);
      nx = true;
    } else if (opt == "XX") {
      if (nx) return reply::error(kSyntaxError);
      xx = true;
    } else if (opt == "GET") {
      get = true;
    } else {
      return reply::error(kSyntaxError);
    }
  }

  const auto previous = store.get(a[1]);
  if ((nx && previous.has_value()) || (xx && !previous.has_value())) {
    return get ? (previous ? reply::bulk(*previous) : reply::null_bulk())
               : reply::null_bulk();
  }

  if (keepttl) {
    const std::int64_t remaining = store.pttl(a[1]);
    expire_at = (remaining >= 0) ? store.clock() + remaining : kNoExpiry;
  }
  store.set(a[1], a[2], expire_at);

  if (get) return previous ? reply::bulk(*previous) : reply::null_bulk();
  return reply::ok();
}

std::string cmd_get(Store& store, const Args& a) {
  if (a.size() != 2) return reply::wrong_arity("get");
  const auto v = store.get(a[1]);
  return v ? reply::bulk(*v) : reply::null_bulk();
}

std::string cmd_del(Store& store, const Args& a) {
  if (a.size() < 2) return reply::wrong_arity("del");
  std::int64_t removed = 0;
  for (std::size_t i = 1; i < a.size(); ++i) {
    if (store.erase(a[i])) ++removed;
  }
  return reply::integer(removed);
}

std::string cmd_exists(Store& store, const Args& a) {
  if (a.size() < 2) return reply::wrong_arity("exists");
  std::int64_t found = 0;
  for (std::size_t i = 1; i < a.size(); ++i) {
    if (store.contains(a[i])) ++found;  // counts duplicates, as redis does
  }
  return reply::integer(found);
}

std::string cmd_type(Store& store, const Args& a) {
  if (a.size() != 2) return reply::wrong_arity("type");
  return store.contains(a[1]) ? reply::simple("string") : reply::simple("none");
}

std::string cmd_keys(Store& store, const Args& a) {
  if (a.size() != 2) return reply::wrong_arity("keys");
  const auto found = store.keys(a[1]);
  std::string out = reply::array_header(found.size());
  for (const auto& k : found) out += reply::bulk(k);
  return out;
}

// EXPIRE/PEXPIRE/EXPIREAT/PEXPIREAT collapse to one absolute-ms deadline.
std::string cmd_expire(Store& store, const Args& a, bool seconds,
                       bool absolute, std::string_view name) {
  if (a.size() != 3) return reply::wrong_arity(name);
  std::int64_t n = 0;
  if (!to_int64(a[2], n)) return reply::error(kNotInteger);
  if (seconds) {
    if (n > std::numeric_limits<std::int64_t>::max() / 1000 ||
        n < std::numeric_limits<std::int64_t>::min() / 1000) {
      return reply::error(kInvalidExpire);
    }
    n *= 1000;
  }
  const std::int64_t deadline = absolute ? n : store.clock() + n;
  return reply::integer(store.expire_at(a[1], deadline) ? 1 : 0);
}

std::string cmd_ttl(Store& store, const Args& a, bool seconds,
                    std::string_view name) {
  if (a.size() != 2) return reply::wrong_arity(name);
  const std::int64_t ms = store.pttl(a[1]);
  if (ms < 0) return reply::integer(ms);  // -1 no ttl, -2 no key
  // Round up, so a key with 1500ms left reports 2s rather than 1s.
  return reply::integer(seconds ? (ms + 999) / 1000 : ms);
}

std::string cmd_persist(Store& store, const Args& a) {
  if (a.size() != 2) return reply::wrong_arity("persist");
  return reply::integer(store.persist(a[1]) ? 1 : 0);
}

// Enough CONFIG GET for redis-cli and redis-benchmark, which probe `save` and
// `appendonly` on connect and fail noisily if the reply shape is wrong.
std::string cmd_config(const Args& a) {
  if (a.size() >= 2 && iequals(a[1], "GET")) {
    if (a.size() != 3) return reply::wrong_arity("config|get");
    std::string out;
    std::size_t count = 0;
    const std::pair<const char*, const char*> params[] = {
        {"save", ""}, {"appendonly", "no"}, {"maxmemory", "0"}};
    for (const auto& [name, value] : params) {
      if (glob_match(a[2], name)) {
        out += reply::bulk(name);
        out += reply::bulk(value);
        count += 2;
      }
    }
    return reply::array_header(count) + out;
  }
  if (a.size() >= 2 && iequals(a[1], "SET")) return reply::ok();
  return reply::error("ERR Unknown CONFIG subcommand");
}

std::string cmd_dbsize(Store& store, const Args& a) {
  if (a.size() != 1) return reply::wrong_arity("dbsize");
  return reply::integer(static_cast<std::int64_t>(store.size()));
}

std::string cmd_flushall(Store& store) {
  store.clear();
  return reply::ok();
}

std::string cmd_info() {
  const std::string info = Stats::instance().to_info();
  return reply::bulk(info);
}

}  // namespace

std::string CommandTable::dispatch(const Args& args) {
  if (args.empty()) return "";  // e.g. "*0\r\n" -- no reply, by design

  const std::string name = upper(args[0]);

  if (name == "PING") return cmd_ping(args);
  if (name == "ECHO") return cmd_echo(args);
  if (name == "SET") return cmd_set(store_, args);
  if (name == "GET") return cmd_get(store_, args);
  if (name == "DEL" || name == "UNLINK") return cmd_del(store_, args);
  if (name == "EXISTS") return cmd_exists(store_, args);
  if (name == "TYPE") return cmd_type(store_, args);
  if (name == "KEYS") return cmd_keys(store_, args);
  if (name == "EXPIRE") return cmd_expire(store_, args, true, false, "expire");
  if (name == "PEXPIRE") return cmd_expire(store_, args, false, false, "pexpire");
  if (name == "EXPIREAT") return cmd_expire(store_, args, true, true, "expireat");
  if (name == "PEXPIREAT")
    return cmd_expire(store_, args, false, true, "pexpireat");
  if (name == "TTL") return cmd_ttl(store_, args, true, "ttl");
  if (name == "PTTL") return cmd_ttl(store_, args, false, "pttl");
  if (name == "PERSIST") return cmd_persist(store_, args);
  if (name == "CONFIG") return cmd_config(args);
  if (name == "DBSIZE") return cmd_dbsize(store_, args);
  if (name == "FLUSHALL" || name == "FLUSHDB") return cmd_flushall(store_);
  if (name == "INFO") return cmd_info();
  if (name == "COMMAND") return reply::empty_array();
  if (name == "SELECT") return reply::ok();  // single-db; accept and ignore
  if (name == "QUIT") return reply::ok();

  std::string msg = "ERR unknown command '" + args[0] + "', with args beginning with: ";
  for (std::size_t i = 1; i < args.size() && i < 4; ++i) {
    msg += "'" + args[i] + "' ";
  }
  return reply::error(msg);
}

std::size_t RespHandler::on_data(Buffer& in, Buffer& out) {
  std::size_t handled = 0;

  for (;;) {
    ParseResult result = parse_request(in.readable());

    if (result.status == ParseStatus::kIncomplete) break;

    if (result.status == ParseStatus::kError) {
      // Protocol errors are unrecoverable: the stream framing is lost, so
      // reply once and stop parsing. The connection layer sees no further
      // progress and the client is expected to disconnect.
      out.append(reply::error(result.error));
      in.clear();
      break;
    }

    in.consume(result.consumed);
    if (result.args.empty()) continue;  // "*0\r\n" or a blank inline line

    out.append(table_.dispatch(result.args));
    ++handled;
  }

  return handled;
}

}  // namespace rp
