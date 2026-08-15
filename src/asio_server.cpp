#include <asio.hpp>

#include <chrono>
#include <memory>
#include <utility>
#include <vector>

#include "rp/buffer.hpp"
#include "rp/server.hpp"
#include "rp/stats.hpp"

namespace rp {
namespace {

using asio::ip::tcp;

// One connection. Reads are always outstanding; writes are serialised through
// a single in-flight async_write with everything else queued in `out_`, which
// is the piece the legacy server lacked -- it called send() inline and silently
// truncated whenever the kernel buffer was full.
class Session : public std::enable_shared_from_this<Session> {
 public:
  Session(tcp::socket socket, const ServerConfig& cfg,
          std::shared_ptr<CommandHandler> handler)
      : socket_(std::move(socket)),
        cfg_(cfg),
        handler_(std::move(handler)),
        scratch_(cfg.read_chunk) {}

  void start() {
    Stats::instance().on_connect();
    if (cfg_.tcp_nodelay) {
      asio::error_code ignored;
      socket_.set_option(tcp::no_delay(true), ignored);
    }
    read();
  }

 private:
  void read() {
    auto self = shared_from_this();
    socket_.async_read_some(
        asio::buffer(scratch_.data(), scratch_.size()),
        [this, self](const asio::error_code& ec, std::size_t n) {
          if (ec) return close();

          auto& stats = Stats::instance();
          stats.bytes_read.fetch_add(n, std::memory_order_relaxed);
          in_.append(scratch_.data(), n);

          // Replies always land in `pending_`: appending to `out_` while a
          // write is in flight could reallocate the vector that async_write
          // holds a raw pointer into.
          const std::size_t handled = handler_->on_data(in_, pending_);
          stats.commands_processed.fetch_add(handled,
                                             std::memory_order_relaxed);

          if (out_.size() + pending_.size() > cfg_.max_output_buffer) {
            stats.output_buffer_overflows.fetch_add(1,
                                                    std::memory_order_relaxed);
            return close();
          }

          flush();
          read();
        });
  }

  void flush() {
    if (writing_) return;
    if (!pending_.empty()) {
      out_.append(pending_.readable());
      pending_.clear();
    }
    if (out_.empty()) return;
    writing_ = true;

    // Hand the whole readable range to async_write. `out_` is not mutated
    // while the write is in flight -- new replies land in `pending_`.
    const std::string_view chunk = out_.readable();
    auto self = shared_from_this();
    asio::async_write(
        socket_, asio::buffer(chunk.data(), chunk.size()),
        [this, self, n = chunk.size()](const asio::error_code& ec,
                                       std::size_t written) {
          writing_ = false;
          Stats::instance().bytes_written.fetch_add(written,
                                                    std::memory_order_relaxed);
          if (ec) return close();
          out_.consume(n);
          flush();
        });
  }

  void close() {
    if (closed_) return;
    closed_ = true;
    asio::error_code ignored;
    socket_.shutdown(tcp::socket::shutdown_both, ignored);
    socket_.close(ignored);
    Stats::instance().on_disconnect();
  }

  tcp::socket socket_;
  const ServerConfig& cfg_;
  std::shared_ptr<CommandHandler> handler_;
  std::vector<char> scratch_;
  Buffer in_;
  Buffer out_;
  Buffer pending_;
  bool writing_ = false;
  bool closed_ = false;
};

class AsioServer : public Server {
 public:
  AsioServer(const ServerConfig& cfg, std::shared_ptr<CommandHandler> handler,
             CronTask cron)
      : cfg_(cfg),
        handler_(std::move(handler)),
        cron_(std::move(cron)),
        acceptor_(io_, tcp::endpoint(asio::ip::make_address(cfg.bind_address),
                                     cfg.port)),
        cron_timer_(io_) {
    acceptor_.listen(cfg_.backlog);
  }

  void run() override {
    accept();
    schedule_cron();
    io_.run();
  }

  void stop() override {
    asio::post(io_, [this] {
      asio::error_code ignored;
      acceptor_.close(ignored);
      // No error_code overload here: it is deprecated, and we build with
      // ASIO_NO_DEPRECATED.
      cron_timer_.cancel();
      io_.stop();
    });
  }

  std::uint16_t port() const override {
    return acceptor_.local_endpoint().port();
  }

 private:
  void accept() {
    acceptor_.async_accept([this](const asio::error_code& ec,
                                  tcp::socket socket) {
      if (!ec) {
        std::make_shared<Session>(std::move(socket), cfg_, handler_)->start();
      } else if (ec != asio::error::operation_aborted) {
        // Out of descriptors is the common case at high connection counts.
        // Count it rather than dying -- the scaler benchmark reads this to
        // find the real ceiling.
        Stats::instance().connections_rejected.fetch_add(
            1, std::memory_order_relaxed);
      }
      if (acceptor_.is_open()) accept();
    });
  }

  // Server cron: runs on the same thread as every connection, so the store
  // needs no locking and the cycle must stay cheap.
  void schedule_cron() {
    if (!cron_) return;
    cron_timer_.expires_after(std::chrono::milliseconds(cfg_.cron_interval_ms));
    cron_timer_.async_wait([this](const asio::error_code& ec) {
      if (ec) return;  // cancelled by stop()
      cron_();
      schedule_cron();
    });
  }

  ServerConfig cfg_;
  std::shared_ptr<CommandHandler> handler_;
  CronTask cron_;
  asio::io_context io_{1};
  tcp::acceptor acceptor_;
  asio::steady_timer cron_timer_;
};

}  // namespace

std::unique_ptr<Server> make_asio_server(const ServerConfig& cfg,
                                         std::shared_ptr<CommandHandler> h,
                                         CronTask cron) {
  return std::make_unique<AsioServer>(cfg, std::move(h), std::move(cron));
}

}  // namespace rp
