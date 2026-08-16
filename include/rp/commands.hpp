#pragma once

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "rp/handler.hpp"
#include "rp/store.hpp"

namespace rp {

using Args = std::vector<std::string>;

struct CommandResult {
  std::string reply;
  bool dirty = false;  // did this command change the keyspace?

  // What to write to the AOF (and, in Phase 5, stream to replicas). Not
  // necessarily the command the client sent: relative expiries are rewritten
  // to absolute deadlines, because replaying `SET k v EX 60` from a file an
  // hour later would silently resurrect the key for another minute.
  Args propagate;
};

// Called with the propagated form of every command that changed the keyspace.
using PropagateFn = std::function<void(const Args&)>;

// What SAVE/BGSAVE/BGREWRITEAOF need, as an interface, so the command table
// does not depend on the persistence implementation (and Phase 5 can hand it
// something else entirely).
class PersistenceOps {
 public:
  virtual ~PersistenceOps() = default;
  virtual bool save(std::string* error) = 0;
  virtual bool background_save(std::string* error) = 0;
  virtual bool rewrite_aof(std::string* error) = 0;
  virtual std::string info_section() = 0;
};

// Dispatches one parsed command to a reply. Pure with respect to the network
// layer: no sockets, no buffers, fully unit-testable.
// What the command table needs from replication, as an interface, to avoid a
// dependency cycle between the two.
class ReplicationOps {
 public:
  virtual ~ReplicationOps() = default;
  virtual void start_full_resync(ClientLink* link) = 0;
  virtual void on_replica_ack(ClientLink* link, std::int64_t offset) = 0;
  virtual void set_replica_port(ClientLink* link, std::uint16_t port) = 0;
  virtual void on_disconnect(ClientLink* link) = 0;
  virtual bool replicaof(const std::string& host, std::uint16_t port,
                         std::string* error) = 0;
  virtual void stop_replication() = 0;
  virtual bool is_replica() const = 0;
  virtual std::string info_section() = 0;
};

class CommandTable {
 public:
  explicit CommandTable(Store& store) : store_(store) {}

  CommandResult dispatch(const Args& args, ClientLink* link = nullptr);

  // Optional: without it, SAVE and friends report that persistence is off.
  void set_persistence(PersistenceOps* ops) { persistence_ = ops; }
  void set_replication(ReplicationOps* ops) { replication_ = ops; }

 private:
  Store& store_;
  PersistenceOps* persistence_ = nullptr;
  ReplicationOps* replication_ = nullptr;
};

// The Phase 2 handler: drains complete requests from the read buffer via
// parse_request() and appends replies. Phase 4 added the propagation hook.
class RespHandler : public CommandHandler {
 public:
  explicit RespHandler(std::shared_ptr<Store> store)
      : store_(std::move(store)), table_(*store_) {}

  using CommandHandler::on_data;  // keep the 2-argument convenience overload
  std::size_t on_data(ClientLink* link, Buffer& in, Buffer& out) override;
  void on_disconnect(ClientLink* link) override;

  void set_propagate(PropagateFn fn) { propagate_ = std::move(fn); }
  void set_persistence(PersistenceOps* ops) { table_.set_persistence(ops); }
  void set_replication(ReplicationOps* ops) {
    replication_ = ops;
    table_.set_replication(ops);
  }
  Store& store() { return *store_; }

  // Apply a command without replying or propagating it. Used to replay an AOF
  // at boot and to apply a master's stream on a replica: neither may be
  // appended back to the file or stream it came from.
  void apply_silently(const Args& args) { table_.dispatch(args, nullptr); }

 private:
  std::shared_ptr<Store> store_;
  CommandTable table_;
  PropagateFn propagate_;
  ReplicationOps* replication_ = nullptr;
};

}  // namespace rp
