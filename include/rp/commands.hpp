#pragma once

#include <memory>
#include <string>
#include <vector>

#include "rp/handler.hpp"
#include "rp/store.hpp"

namespace rp {

using Args = std::vector<std::string>;

// Dispatches one parsed command to a reply. Pure with respect to the network
// layer: no sockets, no buffers, fully unit-testable.
class CommandTable {
 public:
  explicit CommandTable(Store& store) : store_(store) {}

  std::string dispatch(const Args& args);

 private:
  Store& store_;
};

// The real Phase 2 handler: drains complete requests from the read buffer via
// parse_request() and appends replies. Replaces PingPongHandler.
class RespHandler : public CommandHandler {
 public:
  explicit RespHandler(std::shared_ptr<Store> store)
      : store_(std::move(store)), table_(*store_) {}

  std::size_t on_data(Buffer& in, Buffer& out) override;

  Store& store() { return *store_; }

 private:
  std::shared_ptr<Store> store_;
  CommandTable table_;
};

}  // namespace rp
