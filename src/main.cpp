#include <cstdlib>
#include <exception>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>

#include "rp/commands.hpp"
#include "rp/server.hpp"
#include "rp/store.hpp"

namespace {

void usage() {
  std::cerr << "usage: redis-plus [--port N] [--bind ADDR] "
               "[--backend asio|poll]\n";
}

}  // namespace

int main(int argc, char** argv) {
  std::cout << std::unitbuf;
  std::cerr << std::unitbuf;

  rp::ServerConfig cfg;
  rp::Backend backend = rp::Backend::kAsio;

  for (int i = 1; i < argc; ++i) {
    const std::string_view arg = argv[i];
    const auto next = [&]() -> std::string {
      if (i + 1 >= argc) {
        usage();
        std::exit(1);
      }
      return argv[++i];
    };

    if (arg == "--port") {
      cfg.port = static_cast<std::uint16_t>(std::stoi(next()));
    } else if (arg == "--bind") {
      cfg.bind_address = next();
    } else if (arg == "--backend") {
      const std::string b = next();
      if (b == "poll") {
        backend = rp::Backend::kPoll;
      } else if (b == "asio") {
        backend = rp::Backend::kAsio;
      } else {
        usage();
        return 1;
      }
    } else {
      usage();
      return 1;
    }
  }

  try {
    auto store = std::make_shared<rp::Store>();
    // Active expiry rides the server cron on the event-loop thread, so keys
    // that are set with a TTL and never read again are still reclaimed.
    auto server = rp::make_server(
        backend, cfg, std::make_shared<rp::RespHandler>(store),
        [store] { store->active_expire_cycle(); });
    std::cout << "redis-plus listening on " << cfg.bind_address << ":"
              << server->port() << " (backend="
              << (backend == rp::Backend::kPoll ? "poll" : "asio") << ")\n";
    server->run();
  } catch (const std::exception& e) {
    std::cerr << "fatal: " << e.what() << "\n";
    return 1;
  }
  return 0;
}
