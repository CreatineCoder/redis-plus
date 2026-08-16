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

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <functional>
#include <memory>
#include <mutex>
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

class PollServer;

// One client. Doubles as its own ClientLink so the master can push replication
// traffic at it long after its last request.
struct Conn : public ClientLink {
  Conn(int fd, std::string peer) : fd(fd), peer_name(std::move(peer)) {}

  void send(std::string_view data) override { out.append(data); }
  void close() override { close_after_flush = true; }
  std::string peer() const override { return peer_name; }

  int fd;
  std::string peer_name;
  Buffer in;
  Buffer out;
  bool close_after_flush = false;
};

class PollServer : public Server {
 public:
  PollServer(const ServerConfig& cfg, std::shared_ptr<CommandHandler> handler,
             CronTask cron)
      : cfg_(cfg),
        handler_(std::move(handler)),
        cron_(std::move(cron)),
        scratch_(cfg.read_chunk) {
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

      // poll() already wakes on a timeout, so the cron rides on it directly
      // rather than needing a timerfd.
      const int timeout =
          cron_ ? std::min(kPollTimeoutMs, cfg_.cron_interval_ms)
                : kPollTimeoutMs;
      const int ready = ::poll(fds.data(), fds.size(), timeout);
      drain_posted();
      run_cron_if_due();
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

  void post(std::function<void()> task) override {
    std::lock_guard<std::mutex> lock(posted_mutex_);
    posted_.push_back(std::move(task));
  }

 private:
  void accept_ready() {
    for (;;) {  // drain the backlog; the socket is non-blocking
      sockaddr_in peer{};
      socklen_t peer_len = sizeof(peer);
      const int fd =
          ::accept(listen_fd_, reinterpret_cast<sockaddr*>(&peer), &peer_len);
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
      char address[INET_ADDRSTRLEN] = {0};
      ::inet_ntop(AF_INET, &peer.sin_addr, address, sizeof(address));
      auto conn = std::make_unique<Conn>(
          fd, std::string(address) + ":" + std::to_string(ntohs(peer.sin_port)));
      handler_->on_connect(conn.get());
      conns_.emplace(fd, std::move(conn));
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

    stats.commands_processed.fetch_add(handler_->on_data(&c, c.in, c.out),
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

  void run_cron_if_due() {
    if (!cron_) return;
    const auto now = std::chrono::steady_clock::now();
    if (now - last_cron_ <
        std::chrono::milliseconds(cfg_.cron_interval_ms)) {
      return;
    }
    last_cron_ = now;
    cron_();
  }

  void drop(int fd) {
    auto it = conns_.find(fd);
    if (it == conns_.end()) return;
    handler_->on_disconnect(it->second.get());
    conns_.erase(it);
    ::close(fd);
    Stats::instance().on_disconnect();
  }

  // Drain work posted from other threads. poll() already wakes on its timeout,
  // so no self-pipe is needed; the cost is up to one cron interval of latency
  // before a posted task runs.
  void drain_posted() {
    std::vector<std::function<void()>> tasks;
    {
      std::lock_guard<std::mutex> lock(posted_mutex_);
      tasks.swap(posted_);
    }
    for (auto& task : tasks) task();
  }

  static constexpr int kPollTimeoutMs = 100;  // bounds stop() latency
#if defined(MSG_NOSIGNAL)
  static constexpr int kSendFlags = MSG_NOSIGNAL;
#else
  static constexpr int kSendFlags = 0;
#endif

  ServerConfig cfg_;
  std::shared_ptr<CommandHandler> handler_;
  CronTask cron_;
  std::chrono::steady_clock::time_point last_cron_ =
      std::chrono::steady_clock::now();
  std::vector<char> scratch_;
  std::unordered_map<int, std::unique_ptr<Conn>> conns_;
  std::mutex posted_mutex_;
  std::vector<std::function<void()>> posted_;
  int listen_fd_ = -1;
  std::uint16_t port_ = 0;
  std::atomic<bool> stopping_{false};
};

}  // namespace

std::unique_ptr<Server> make_poll_server(const ServerConfig& cfg,
                                         std::shared_ptr<CommandHandler> h,
                                         CronTask cron) {
  return std::make_unique<PollServer>(cfg, std::move(h), std::move(cron));
}

}  // namespace rp

#else  // non-POSIX

#include <memory>
#include <stdexcept>

namespace rp {
std::unique_ptr<Server> make_poll_server(const ServerConfig&,
                                         std::shared_ptr<CommandHandler>,
                                         CronTask) {
  throw std::invalid_argument(
      "poll backend is POSIX-only; use --backend=asio on this platform");
}
}  // namespace rp

#endif
