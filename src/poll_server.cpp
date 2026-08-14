// POSIX poll(2) backend.
//
// This is the design the reference implementation used, kept deliberately so
// the "poll multiplexing" claim stays literally true and so the benchmark
// harness can put poll() and epoll/kqueue side by side on identical workloads.
// Unlike the reference it uses non-blocking sockets, per-connection buffers,
// deferred writes via POLLOUT, and safe removal during iteration.

#include "rp/server.hpp"

#if defined(__unix__) || defined(__APPLE__)

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

#include "rp/buffer.hpp"
#include "rp/stats.hpp"

namespace rp {
namespace {

void set_nonblocking(int fd) {
  const int flags = fcntl(fd, F_GETFL, 0);
  if (flags >= 0) fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

struct Conn {
  Buffer in;
  Buffer out;
  bool close_after_flush = false;
};

class PollServer : public Server {
 public:
  PollServer(const ServerConfig& cfg, std::shared_ptr<CommandHandler> handler)
      : cfg_(cfg), handler_(std::move(handler)), scratch_(cfg.read_chunk) {
    listen_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd_ < 0) throw std::runtime_error("socket() failed");

    int reuse = 1;
    ::setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(cfg.port);
    if (::inet_pton(AF_INET, cfg.bind_address.c_str(), &addr.sin_addr) != 1) {
      throw std::runtime_error("bad bind address: " + cfg.bind_address);
    }
    if (::bind(listen_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) !=
        0) {
      throw std::runtime_error("bind() failed on port " +
                               std::to_string(cfg.port));
    }
    if (::listen(listen_fd_, cfg.backlog) != 0) {
      throw std::runtime_error("listen() failed");
    }

    socklen_t len = sizeof(addr);
    if (::getsockname(listen_fd_, reinterpret_cast<sockaddr*>(&addr), &len) ==
        0) {
      port_ = ntohs(addr.sin_port);
    }
    set_nonblocking(listen_fd_);
  }

  ~PollServer() override {
    if (listen_fd_ >= 0) ::close(listen_fd_);
  }

  void run() override {
    std::vector<pollfd> fds;
    while (!stopping_.load(std::memory_order_relaxed)) {
      fds.clear();
      fds.push_back({listen_fd_, POLLIN, 0});
      for (auto& [fd, conn] : conns_) {
        short events = POLLIN;
        if (!conn->out.empty()) events |= POLLOUT;
        fds.push_back({fd, events, 0});
      }

      const int ready = ::poll(fds.data(), fds.size(), kPollTimeoutMs);
      if (ready < 0) {
        if (errno == EINTR) continue;
        break;
      }

      // Collect closures and apply them after the sweep -- the reference
      // implementation erased from the pollfd vector mid-loop and decremented
      // the index to compensate, which skipped events under load.
      std::vector<int> dead;
      for (const auto& p : fds) {
        if (p.revents == 0) continue;
        if (p.fd == listen_fd_) {
          accept_ready();
          continue;
        }
        if (p.revents & (POLLHUP | POLLERR | POLLNVAL)) {
          dead.push_back(p.fd);
          continue;
        }
        if ((p.revents & POLLIN) && !handle_read(p.fd)) {
          dead.push_back(p.fd);
          continue;
        }
        if ((p.revents & POLLOUT) && !handle_write(p.fd)) {
          dead.push_back(p.fd);
        }
      }
      for (const int fd : dead) drop(fd);
    }
  }

  void stop() override { stopping_.store(true, std::memory_order_relaxed); }

  std::uint16_t port() const override { return port_; }

 private:
  void accept_ready() {
    for (;;) {  // drain the backlog; the socket is non-blocking
      const int fd = ::accept(listen_fd_, nullptr, nullptr);
      if (fd < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) return;
        if (errno == EMFILE || errno == ENFILE) {
          Stats::instance().connections_rejected.fetch_add(
              1, std::memory_order_relaxed);
        }
        return;
      }
      set_nonblocking(fd);
      if (cfg_.tcp_nodelay) {
        int one = 1;
        ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
      }
      conns_.emplace(fd, std::make_unique<Conn>());
      Stats::instance().on_connect();
    }
  }

  bool handle_read(int fd) {
    auto it = conns_.find(fd);
    if (it == conns_.end()) return false;
    Conn& c = *it->second;
    auto& stats = Stats::instance();

    for (;;) {
      const ssize_t n = ::recv(fd, scratch_.data(), scratch_.size(), 0);
      if (n > 0) {
        stats.bytes_read.fetch_add(static_cast<std::uint64_t>(n),
                                   std::memory_order_relaxed);
        c.in.append(scratch_.data(), static_cast<std::size_t>(n));
        if (static_cast<std::size_t>(n) < scratch_.size()) break;
        continue;  // full chunk: more may be queued
      }
      if (n == 0) return false;  // peer closed
      if (errno == EAGAIN || errno == EWOULDBLOCK) break;
      if (errno == EINTR) continue;
      return false;
    }

    stats.commands_processed.fetch_add(handler_->on_data(c.in, c.out),
                                       std::memory_order_relaxed);
    if (c.out.size() > cfg_.max_output_buffer) {
      stats.output_buffer_overflows.fetch_add(1, std::memory_order_relaxed);
      return false;
    }
    return handle_write(fd);
  }

  bool handle_write(int fd) {
    auto it = conns_.find(fd);
    if (it == conns_.end()) return false;
    Conn& c = *it->second;

    while (!c.out.empty()) {
      const std::string_view chunk = c.out.readable();
      const ssize_t n = ::send(fd, chunk.data(), chunk.size(), kSendFlags);
      if (n > 0) {
        Stats::instance().bytes_written.fetch_add(
            static_cast<std::uint64_t>(n), std::memory_order_relaxed);
        c.out.consume(static_cast<std::size_t>(n));
        continue;
      }
      if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
        // Socket is full. Leave the remainder queued; POLLOUT brings us back.
        return true;
      }
      if (n < 0 && errno == EINTR) continue;
      return false;
    }
    return !c.close_after_flush;
  }

  void drop(int fd) {
    if (conns_.erase(fd) == 0) return;
    ::close(fd);
    Stats::instance().on_disconnect();
  }

  static constexpr int kPollTimeoutMs = 100;  // bounds stop() latency
#if defined(MSG_NOSIGNAL)
  static constexpr int kSendFlags = MSG_NOSIGNAL;
#else
  static constexpr int kSendFlags = 0;
#endif

  ServerConfig cfg_;
  std::shared_ptr<CommandHandler> handler_;
  std::vector<char> scratch_;
  std::unordered_map<int, std::unique_ptr<Conn>> conns_;
  int listen_fd_ = -1;
  std::uint16_t port_ = 0;
  std::atomic<bool> stopping_{false};
};

}  // namespace

std::unique_ptr<Server> make_poll_server(const ServerConfig& cfg,
                                         std::shared_ptr<CommandHandler> h) {
  return std::make_unique<PollServer>(cfg, std::move(h));
}

}  // namespace rp

#else  // non-POSIX

#include <memory>
#include <stdexcept>

namespace rp {
std::unique_ptr<Server> make_poll_server(const ServerConfig&,
                                         std::shared_ptr<CommandHandler>) {
  throw std::invalid_argument(
      "poll backend is POSIX-only; use --backend=asio on this platform");
}
}  // namespace rp

#endif
