#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

#include "rp/handler.hpp"

namespace rp {

struct ServerConfig {
  std::string bind_address = "0.0.0.0";
  std::uint16_t port = 6379;
  int backlog = 511;  // matches redis' default tcp-backlog

  // Hard cap on a single connection's pending output. A client that never
  // drains its socket must not be able to grow the server's memory without
  // bound -- past this we drop the connection. Real redis calls this
  // client-output-buffer-limit.
  std::size_t max_output_buffer = 64ull * 1024 * 1024;

  std::size_t read_chunk = 16 * 1024;

  bool tcp_nodelay = true;

  // Server cron period. 100ms == redis' default hz=10. The active expiry
  // cycle rides on this.
  int cron_interval_ms = 100;
};

// Periodic work run on the event loop thread, between client events. Must be
// cheap and must not block -- it shares the thread with every connection.
using CronTask = std::function<void()>;

// Backend-agnostic interface so the poll() implementation and the asio
// implementation are interchangeable, and so the benchmark harness can compare
// them directly on the same workload.
class Server {
 public:
  virtual ~Server() = default;
  virtual void run() = 0;   // blocks until stop()
  virtual void stop() = 0;
  virtual std::uint16_t port() const = 0;  // resolved port (useful when 0)

  // Run `task` on the event-loop thread. The only thread-safe entry point on
  // this interface, and the way work from other threads (a replica's link to
  // its master) reaches the store without any locking.
  virtual void post(std::function<void()> task) = 0;
};

enum class Backend {
  kAsio,  // cross-platform: epoll/kqueue/IOCP underneath
  kPoll,  // POSIX poll(2) -- the legacy design, kept for honest comparison
};

// Throws std::invalid_argument if the backend is unavailable on this platform.
std::unique_ptr<Server> make_server(Backend backend, const ServerConfig& cfg,
                                    std::shared_ptr<CommandHandler> handler,
                                    CronTask cron = nullptr);

}  // namespace rp
