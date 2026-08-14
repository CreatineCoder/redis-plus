#pragma once

#include <cstddef>

#include "rp/buffer.hpp"

namespace rp {

// The seam between the connection layer (Phase 1) and the protocol layer
// (Phase 2). The connection layer never inspects bytes; it hands the inbound
// buffer over and appends whatever comes back to the outbound buffer.
class CommandHandler {
 public:
  virtual ~CommandHandler() = default;

  // Consume as many complete requests as `in` currently holds, appending
  // replies to `out`. Leave incomplete trailing bytes in `in` for the next
  // read. Returns the number of complete requests handled.
  //
  // Implementations MUST NOT block and MUST NOT consume a partial request.
  virtual std::size_t on_data(Buffer& in, Buffer& out) = 0;
};

// Phase 1 placeholder: enough protocol to make the connection layer
// measurable with redis-benchmark/redis-cli PING, and nothing more. Replaced
// wholesale by the incremental RESP parser in Phase 2.
class PingPongHandler : public CommandHandler {
 public:
  std::size_t on_data(Buffer& in, Buffer& out) override;
};

}  // namespace rp
