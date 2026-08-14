#include "rp/server.hpp"

#include <utility>

namespace rp {

std::unique_ptr<Server> make_asio_server(const ServerConfig&,
                                         std::shared_ptr<CommandHandler>);
std::unique_ptr<Server> make_poll_server(const ServerConfig&,
                                         std::shared_ptr<CommandHandler>);

std::unique_ptr<Server> make_server(Backend backend, const ServerConfig& cfg,
                                    std::shared_ptr<CommandHandler> handler) {
  switch (backend) {
    case Backend::kPoll:
      return make_poll_server(cfg, std::move(handler));
    case Backend::kAsio:
    default:
      return make_asio_server(cfg, std::move(handler));
  }
}

}  // namespace rp
