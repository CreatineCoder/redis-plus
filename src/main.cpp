#include <cstdlib>
#include <exception>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>

#include "rp/commands.hpp"
#include "rp/persistence.hpp"
#include "rp/replication.hpp"
#include "rp/server.hpp"
#include "rp/store.hpp"

namespace {

void usage() {
  std::cerr << "usage: redis-plus [options]\n"
               "  --port N              listen port (default 6379)\n"
               "  --bind ADDR           bind address (default 0.0.0.0)\n"
               "  --backend asio|poll   event loop (default asio)\n"
               "  --dir PATH            working directory for data files\n"
               "  --dbfilename NAME     RDB filename (default dump.rdb)\n"
               "  --save SEC CHANGES    snapshot policy; 0 0 disables\n"
               "  --appendonly yes|no   enable the AOF (default no)\n"
               "  --appendfsync always|everysec|no\n"
               "  --replicaof HOST PORT replicate from this master\n";
}

}  // namespace

int main(int argc, char** argv) {
  std::cout << std::unitbuf;
  std::cerr << std::unitbuf;

  rp::ServerConfig cfg;
  rp::PersistenceConfig persist_cfg;
  rp::Backend backend = rp::Backend::kAsio;
  std::string replicaof_host;
  std::uint16_t replicaof_port = 0;

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
      if (b == "poll") backend = rp::Backend::kPoll;
      else if (b == "asio") backend = rp::Backend::kAsio;
      else { usage(); return 1; }
    } else if (arg == "--dir") {
      persist_cfg.dir = next();
    } else if (arg == "--dbfilename") {
      persist_cfg.dbfilename = next();
    } else if (arg == "--save") {
      persist_cfg.save_seconds = std::stoi(next());
      persist_cfg.save_changes = std::stoll(next());
    } else if (arg == "--appendonly") {
      persist_cfg.aof_enabled = (next() == "yes");
    } else if (arg == "--replicaof") {
      replicaof_host = next();
      replicaof_port = static_cast<std::uint16_t>(std::stoi(next()));
    } else if (arg == "--appendfsync") {
      bool ok = false;
      persist_cfg.fsync_policy = rp::parse_fsync_policy(next(), &ok);
      if (!ok) { usage(); return 1; }
    } else {
      usage();
      return 1;
    }
  }

  try {
    auto store = std::make_shared<rp::Store>();
    auto handler = std::make_shared<rp::RespHandler>(store);
    auto persistence =
        std::make_shared<rp::Persistence>(store, persist_cfg);

    // Restore before accepting a single connection, so no client can observe
    // an empty keyspace that is about to be overwritten by the load.
    std::string error;
    if (!persistence->load(*handler, &error)) {
      std::cerr << "fatal: cannot restore state: " << error << "\n"
                << "Refusing to start on a corrupt data file. Move it aside "
                   "to start empty.\n";
      return 1;
    }
    std::cout << "loaded " << store->size() << " keys\n";

    auto replication = std::make_shared<rp::Replication>(store);
    replication->set_listening_port(cfg.port);
    replication->set_apply(
        [handler](const rp::Args& args) { handler->apply_silently(args); });

    handler->set_persistence(persistence.get());
    handler->set_replication(replication.get());

    // One hook, two consumers: every write that changed the keyspace goes to
    // the AOF and to every attached replica, in canonical (absolute-expiry)
    // form. Phase 4 built this seam; Phase 5 just hangs off it.
    handler->set_propagate([persistence, replication](const rp::Args& args) {
      persistence->on_write(args);
      replication->propagate(args);
    });

    auto server = rp::make_server(backend, cfg, handler, [store, persistence] {
      store->active_expire_cycle();
      persistence->cron();
    });

    replication->attach(server.get());
    if (!replicaof_host.empty()) {
      std::string replica_error;
      if (!replication->replicaof(replicaof_host, replicaof_port,
                                  &replica_error)) {
        std::cerr << "fatal: " << replica_error << "\n";
        return 1;
      }
      std::cout << "replicating from " << replicaof_host << ":"
                << replicaof_port << "\n";
    }

    std::cout << "redis-plus listening on " << cfg.bind_address << ":"
              << server->port() << " (backend="
              << (backend == rp::Backend::kPoll ? "poll" : "asio")
              << ", rdb=" << (persist_cfg.rdb_enabled ? "on" : "off")
              << ", aof=" << (persist_cfg.aof_enabled ? "on" : "off") << ")\n";
    server->run();

    replication->stop_replication();
    persistence->shutdown();
  } catch (const std::exception& e) {
    std::cerr << "fatal: " << e.what() << "\n";
    return 1;
  }
  return 0;
}
