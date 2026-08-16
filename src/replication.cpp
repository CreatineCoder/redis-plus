#include "rp/replication.hpp"

#include <asio.hpp>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <random>
#include <string_view>

#include "rp/aof.hpp"
#include "rp/rdb.hpp"
#include "rp/resp.hpp"

namespace rp {
namespace {

using asio::ip::tcp;

std::string random_replid() {
  static const char* kHex = "0123456789abcdef";
  std::random_device rd;
  std::mt19937_64 rng(rd());
  std::uniform_int_distribution<int> pick(0, 15);
  std::string out(40, '0');
  for (char& c : out) c = kHex[pick(rng)];
  return out;
}

// Blocking line/exact reads over a synchronous socket, with the leftover bytes
// kept so the command stream that follows the RDB is not lost. Getting this
// wrong silently drops the first propagated write.
class SyncReader {
 public:
  explicit SyncReader(tcp::socket& socket) : socket_(socket) {}

  bool read_line(std::string* out, asio::error_code* ec) {
    for (;;) {
      const std::size_t eol = buffer_.find("\r\n");
      if (eol != std::string::npos) {
        *out = buffer_.substr(0, eol);
        buffer_.erase(0, eol + 2);
        return true;
      }
      if (!fill(ec)) return false;
    }
  }

  bool read_exact(std::size_t n, std::string* out, asio::error_code* ec) {
    while (buffer_.size() < n) {
      if (!fill(ec)) return false;
    }
    *out = buffer_.substr(0, n);
    buffer_.erase(0, n);
    return true;
  }

  // Whatever has been read past the last consumed byte.
  std::string& leftover() { return buffer_; }

  bool fill(asio::error_code* ec) {
    char chunk[16 * 1024];
    const std::size_t n =
        socket_.read_some(asio::buffer(chunk, sizeof(chunk)), *ec);
    if (*ec) return false;
    buffer_.append(chunk, n);
    return true;
  }

 private:
  tcp::socket& socket_;
  std::string buffer_;
};

bool send_command(tcp::socket& socket, const Args& args,
                  asio::error_code* ec) {
  const std::string encoded = Aof::encode(args);
  asio::write(socket, asio::buffer(encoded), *ec);
  return !*ec;
}

bool expect_prefix(const std::string& line, std::string_view prefix) {
  return line.size() >= prefix.size() &&
         line.compare(0, prefix.size(), prefix) == 0;
}

}  // namespace

const char* link_state_name(LinkState state) {
  switch (state) {
    case LinkState::kConnecting: return "connect";
    case LinkState::kHandshaking: return "handshake";
    case LinkState::kSyncing: return "sync";
    case LinkState::kConnected: return "up";
    case LinkState::kNone:
    default: return "down";
  }
}

bool is_write_command(const std::string& name) {
  return name == "SET" || name == "DEL" || name == "UNLINK" ||
         name == "EXPIRE" || name == "PEXPIRE" || name == "EXPIREAT" ||
         name == "PEXPIREAT" || name == "PERSIST" || name == "FLUSHALL" ||
         name == "FLUSHDB";
}

Replication::Replication(std::shared_ptr<Store> store)
    : store_(std::move(store)), replid_(random_replid()) {}

Replication::~Replication() { stop_replication(); }

// ---------------------------------------------------------------------------
// master side
// ---------------------------------------------------------------------------

void Replication::start_full_resync(ClientLink* link) {
  if (link == nullptr) return;

  // The offset is captured before the snapshot is taken, so any write that
  // lands after this point is streamed rather than lost between the two.
  link->send("+FULLRESYNC " + replid_ + " " +
             std::to_string(master_offset_) + "\r\n");

  const std::string payload = rdb_serialize(store_->snapshot());
  // Length-prefixed like a bulk string but with NO trailing CRLF -- this is
  // the RDB transfer format, and appending one desynchronises the replica's
  // command stream by two bytes.
  link->send("$" + std::to_string(payload.size()) + "\r\n");
  link->send(payload);

  Replica replica;
  replica.link = link;
  replica.info.address = link->peer();
  replica.info.ack_time_ms = now_ms();
  const auto port = pending_ports_.find(link);
  if (port != pending_ports_.end()) replica.info.listening_port = port->second;
  replicas_.push_back(replica);
}

void Replication::set_replica_port(ClientLink* link, std::uint16_t port) {
  for (auto& replica : replicas_) {
    if (replica.link == link) replica.info.listening_port = port;
  }
  pending_ports_[link] = port;
}

void Replication::on_replica_ack(ClientLink* link, std::int64_t offset) {
  for (auto& replica : replicas_) {
    if (replica.link == link) {
      replica.info.ack_offset = offset;
      replica.info.ack_time_ms = now_ms();
    }
  }
}

void Replication::on_disconnect(ClientLink* link) {
  replicas_.erase(
      std::remove_if(replicas_.begin(), replicas_.end(),
                     [link](const Replica& r) { return r.link == link; }),
      replicas_.end());
  pending_ports_.erase(link);
}

void Replication::propagate(const Args& args) {
  if (replicas_.empty()) {
    // Offset still advances with no replicas attached, matching redis: it is
    // the position in the replication stream, not "bytes actually sent".
    master_offset_ += static_cast<std::int64_t>(Aof::encode(args).size());
    return;
  }
  const std::string encoded = Aof::encode(args);
  master_offset_ += static_cast<std::int64_t>(encoded.size());
  for (auto& replica : replicas_) replica.link->send(encoded);
}

// ---------------------------------------------------------------------------
// replica side
// ---------------------------------------------------------------------------

bool Replication::replicaof(const std::string& host, std::uint16_t port,
                            std::string* error) {
  if (server_ == nullptr) {
    if (error) *error = "replication is not attached to a server";
    return false;
  }
  stop_replication();

  replica_of_host_ = host;
  replica_of_port_ = port;
  link_stop_.store(false);
  link_state_.store(LinkState::kConnecting);
  replica_offset_.store(0);
  link_thread_ = std::thread([this, host, port] { replica_thread(host, port); });
  return true;
}

void Replication::stop_replication() {
  link_stop_.store(true);
  if (link_thread_.joinable()) link_thread_.join();
  replica_of_host_.clear();
  replica_of_port_ = 0;
  link_state_.store(LinkState::kNone);
}

void Replication::replica_thread(std::string host, std::uint16_t port) {
  const auto record_error = [this](const std::string& message) {
    std::lock_guard<std::mutex> lock(link_error_mutex_);
    link_error_ = message;
  };

  while (!link_stop_.load()) {
    asio::io_context io;
    tcp::socket socket(io);
    asio::error_code ec;

    link_state_.store(LinkState::kConnecting);
    socket.connect(tcp::endpoint(asio::ip::make_address(host, ec), port), ec);
    if (ec) {
      record_error("cannot connect to master: " + ec.message());
      std::this_thread::sleep_for(std::chrono::milliseconds(500));
      continue;
    }
    socket.set_option(tcp::no_delay(true), ec);

    SyncReader reader(socket);
    std::string line;

    // Handshake, in redis' order. Each step is a real round trip: a master
    // that does not answer is not a master we should stream from.
    link_state_.store(LinkState::kHandshaking);
    const auto step = [&](const Args& command, std::string_view expected) {
      if (!send_command(socket, command, &ec)) return false;
      if (!reader.read_line(&line, &ec)) return false;
      return expected.empty() || expect_prefix(line, expected);
    };

    if (!step({"PING"}, "+PONG") ||
        !step({"REPLCONF", "listening-port", std::to_string(listening_port_)},
              "+OK") ||
        !step({"REPLCONF", "capa", "psync2"}, "+OK")) {
      record_error("handshake rejected by master");
      std::this_thread::sleep_for(std::chrono::milliseconds(500));
      continue;
    }

    if (!send_command(socket, {"PSYNC", "?", "-1"}, &ec) ||
        !reader.read_line(&line, &ec) || !expect_prefix(line, "+FULLRESYNC")) {
      record_error("master refused PSYNC");
      std::this_thread::sleep_for(std::chrono::milliseconds(500));
      continue;
    }

    std::int64_t offset = 0;
    {
      const std::size_t last_space = line.rfind(' ');
      if (last_space != std::string::npos) {
        offset = std::strtoll(line.c_str() + last_space + 1, nullptr, 10);
      }
    }

    // RDB payload: "$<len>\r\n" then exactly len bytes, no trailing CRLF.
    link_state_.store(LinkState::kSyncing);
    if (!reader.read_line(&line, &ec) || line.empty() || line[0] != '$') {
      record_error("bad RDB header from master");
      continue;
    }
    const std::size_t rdb_len =
        static_cast<std::size_t>(std::strtoull(line.c_str() + 1, nullptr, 10));
    std::string payload;
    if (!reader.read_exact(rdb_len, &payload, &ec)) {
      record_error("truncated RDB from master");
      continue;
    }

    std::vector<Record> records;
    std::string parse_error;
    if (!rdb_parse(payload, &records, &parse_error)) {
      record_error("corrupt RDB from master: " + parse_error);
      std::this_thread::sleep_for(std::chrono::milliseconds(500));
      continue;
    }

    // Replace the keyspace wholesale on the event-loop thread: a full resync
    // means the master's state is authoritative, including deletions.
    auto store = store_;
    server_->post([store, records] {
      store->clear();
      for (const auto& record : records) store->load_record(record);
    });

    replica_offset_.store(offset);
    link_state_.store(LinkState::kConnected);
    record_error("");

    // Command stream.
    std::string stream = std::move(reader.leftover());
    std::int64_t last_ack_ms = now_ms();

    while (!link_stop_.load()) {
      std::string_view remaining(stream);
      std::size_t consumed_total = 0;

      for (;;) {
        const ParseResult result = parse_request(remaining);
        if (result.status != ParseStatus::kOk) break;

        remaining.remove_prefix(result.consumed);
        consumed_total += result.consumed;

        if (!result.args.empty()) {
          std::string name = result.args[0];
          std::transform(name.begin(), name.end(), name.begin(),
                         [](unsigned char c) { return std::toupper(c); });

          // Offset advances for every byte received, including GETACK itself,
          // before the acknowledgement is sent.
          replica_offset_.fetch_add(
              static_cast<std::int64_t>(result.consumed));

          if (name == "REPLCONF" && result.args.size() >= 2) {
            send_command(socket,
                         {"REPLCONF", "ACK",
                          std::to_string(replica_offset_.load())},
                         &ec);
          } else if (name == "PING") {
            // Keepalive: counts toward the offset, applies nothing.
          } else if (apply_) {
            const Args args = result.args;
            auto apply = apply_;
            server_->post([apply, args] { apply(args); });
          }
        } else {
          replica_offset_.fetch_add(
              static_cast<std::int64_t>(result.consumed));
        }
      }

      stream.erase(0, consumed_total);

      const std::int64_t now = now_ms();
      if (now - last_ack_ms >= 1000) {
        send_command(socket,
                     {"REPLCONF", "ACK", std::to_string(replica_offset_.load())},
                     &ec);
        last_ack_ms = now;
      }

      // Blocking read with a deadline would be better; a short poll keeps the
      // shutdown path simple and costs one syscall per interval.
      socket.non_blocking(true, ec);
      char chunk[16 * 1024];
      const std::size_t n =
          socket.read_some(asio::buffer(chunk, sizeof(chunk)), ec);
      if (!ec) {
        stream.append(chunk, n);
      } else if (ec == asio::error::would_block ||
                 ec == asio::error::try_again) {
        ec.clear();
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
      } else {
        record_error("lost connection to master: " + ec.message());
        break;
      }
    }

    link_state_.store(LinkState::kConnecting);
  }

  link_state_.store(LinkState::kNone);
}

std::string Replication::info_section() {
  std::string out;
  if (is_replica()) {
    out += "role:slave\r\n";
    out += "master_host:" + replica_of_host_ + "\r\n";
    out += "master_port:" + std::to_string(replica_of_port_) + "\r\n";
    out += "master_link_status:" +
           std::string(link_state_.load() == LinkState::kConnected ? "up"
                                                                  : "down") +
           "\r\n";
    out += "master_link_state:" +
           std::string(link_state_name(link_state_.load())) + "\r\n";
    out += "slave_read_only:1\r\n";
    out += "slave_repl_offset:" + std::to_string(replica_offset_.load()) +
           "\r\n";
    std::lock_guard<std::mutex> lock(link_error_mutex_);
    if (!link_error_.empty()) {
      out += "master_link_error:" + link_error_ + "\r\n";
    }
  } else {
    out += "role:master\r\n";
  }

  out += "connected_slaves:" + std::to_string(replicas_.size()) + "\r\n";
  for (std::size_t i = 0; i < replicas_.size(); ++i) {
    const auto& info = replicas_[i].info;
    out += "slave" + std::to_string(i) + ":addr=" + info.address +
           ",port=" + std::to_string(info.listening_port) +
           ",state=online,offset=" + std::to_string(info.ack_offset) +
           ",lag=" + std::to_string(master_offset_ - info.ack_offset) + "\r\n";
  }
  out += "master_replid:" + replid_ + "\r\n";
  out += "master_repl_offset:" + std::to_string(master_offset_) + "\r\n";
  return out;
}

}  // namespace rp
