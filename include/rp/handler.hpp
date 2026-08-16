#pragma once

#include <cstddef>
#include <string>
#include <string_view>

#include "rp/buffer.hpp"

namespace rp {

// A handle on one connected client, implemented by the server backend.
//
// Phase 5 is the reason this exists. Until now every byte the server sent was
// a reply to something the client had just asked for, so the handler could
// simply return bytes. A replica is different: after PSYNC it says nothing
// more, and the master pushes writes at it indefinitely. That needs a way to
// address a connection outside its own read callback.
//
// All methods are called on the event-loop thread. Anything arriving from
// another thread must go through Server::post() first.
class ClientLink {
 public:
  virtual ~ClientLink() = default;

  // Queue bytes for this client. Never blocks.
  virtual void send(std::string_view data) = 0;

  // Close after the queued output drains.
  virtual void close() = 0;

  // Stable identifier, for logging and INFO.
  virtual std::string peer() const = 0;
};

class CommandHandler {
 public:
  virtual ~CommandHandler() = default;

  virtual void on_connect(ClientLink*) {}
  virtual void on_disconnect(ClientLink*) {}

  // Consume as many complete requests as `in` currently holds, appending
  // replies to `out`. Leave incomplete trailing bytes in `in` for the next
  // read. Returns the number of complete requests handled.
  //
  // Implementations MUST NOT block and MUST NOT consume a partial request.
  virtual std::size_t on_data(ClientLink* link, Buffer& in, Buffer& out) = 0;

  // Convenience for tests and callers with no connection context.
  std::size_t on_data(Buffer& in, Buffer& out) {
    return on_data(nullptr, in, out);
  }
};

// Phase 1 placeholder, kept because the connection-layer tests use it: enough
// protocol to be driven by redis-benchmark, and nothing more.
class PingPongHandler : public CommandHandler {
 public:
  using CommandHandler::on_data;  // keep the 2-argument convenience overload
  std::size_t on_data(ClientLink* link, Buffer& in, Buffer& out) override;
};

}  // namespace rp
