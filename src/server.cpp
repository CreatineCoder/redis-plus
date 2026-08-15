#include "rp/server.hpp"

#include <utility>

namespace rp {

std::unique_ptr<Server> make_asio_server(const ServerConfig&,
                                         std::shared_ptr<CommandHandler>,
                                         CronTask);
std::unique_ptr<Server> make_poll_server(const ServerConfig&,
                                         std::shared_ptr<CommandHandler>,
                                         CronTask);

std::unique_ptr<Server> make_server(Backend backend, const ServerConfig& cfg,
                                    std::shared_ptr<CommandHandler> handler,
                                    CronTask cron) {
  switch (backend) {
    case Backend::kPoll:
      return make_poll_server(cfg, std::move(handler), std::move(cron));
    case Backend::kAsio:
    default:
      return make_asio_server(cfg, std::move(handler), std::move(cron));
  }
}

}  // namespace rp
