#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "rp/commands.hpp"
#include "rp/handler.hpp"
#include "rp/server.hpp"
#include "rp/store.hpp"

namespace rp {

// Asynchronous master-replica replication, in the shape real redis uses.
//
// Master side: a client that sends PSYNC stops being a client and becomes a
// replica. It gets a FULLRESYNC header, an RDB payload of the whole keyspace
// (the Phase 4 serializer, unchanged), and from then on every write is
// streamed to it as a RESP command. A byte offset advances with the stream and
// replicas acknowledge it, which is how replication lag becomes measurable
// rather than a feeling.
//
// Replica side: a background thread performs the handshake, loads the RDB,
// then applies the stream. It never touches the store directly -- every apply
// is posted to the event-loop thread, so the store remains single-threaded and
// lock-free exactly as in Phase 1.
//
// This is ASYNCHRONOUS replication, like redis: the master does not wait for
// acknowledgement before replying to the client. Writes acknowledged by the
// master can therefore be lost if it dies before they reach a replica. That is
// eventual consistency, not high availability -- automatic failover would need
// a Sentinel-style component that does not exist here.

struct ReplicaInfo {
  std::string address;
  std::uint16_t listening_port = 0;
  std::int64_t ack_offset = 0;
  std::int64_t ack_time_ms = 0;
};

enum class LinkState { kNone, kConnecting, kHandshaking, kSyncing, kConnected };

const char* link_state_name(LinkState state);

class Replication : public ReplicationOps {
 public:
  explicit Replication(std::shared_ptr<Store> store);
  ~Replication() override;

  // The event loop this replication attaches to. Required before REPLICAOF.
  void attach(Server* server) { server_ = server; }
  void set_listening_port(std::uint16_t port) { listening_port_ = port; }

  // How a replica applies a command from its master. Wired to the command
  // table by the owner.
  void set_apply(std::function<void(const Args&)> apply) {
    apply_ = std::move(apply);
  }

  // ---- master side (event-loop thread) ----

  // Promote a connection to a replica: FULLRESYNC + RDB payload, then stream.
  void start_full_resync(ClientLink* link) override;
  void on_replica_ack(ClientLink* link, std::int64_t offset) override;
  void set_replica_port(ClientLink* link, std::uint16_t port) override;
  void on_disconnect(ClientLink* link) override;

  // Stream one write to every replica and advance the offset.
  void propagate(const Args& args);

  std::size_t replica_count() const { return replicas_.size(); }
  const std::string& replid() const { return replid_; }
  std::int64_t master_offset() const { return master_offset_; }

  // ---- replica side ----

  // REPLICAOF <host> <port>, or REPLICAOF NO ONE to stop.
  bool replicaof(const std::string& host, std::uint16_t port,
                 std::string* error) override;
  void stop_replication() override;
  bool is_replica() const override { return replica_of_port_ != 0; }
  LinkState link_state() const { return link_state_.load(); }
  std::int64_t replica_offset() const { return replica_offset_.load(); }

  std::string info_section() override;

 private:
  struct Replica {
    ClientLink* link = nullptr;
    ReplicaInfo info;
  };

  void replica_thread(std::string host, std::uint16_t port);

  std::shared_ptr<Store> store_;
  Server* server_ = nullptr;
  std::function<void(const Args&)> apply_;
  std::uint16_t listening_port_ = 0;

  // master side
  std::string replid_;
  std::int64_t master_offset_ = 0;
  std::vector<Replica> replicas_;

  // REPLCONF listening-port arrives before PSYNC, so the port is parked here
  // until the connection is actually promoted to a replica.
  std::unordered_map<ClientLink*, std::uint16_t> pending_ports_;

  // replica side
  std::string replica_of_host_;
  std::uint16_t replica_of_port_ = 0;
  std::thread link_thread_;
  std::atomic<bool> link_stop_{false};
  std::atomic<LinkState> link_state_{LinkState::kNone};
  std::atomic<std::int64_t> replica_offset_{0};
  std::string link_error_;
  std::mutex link_error_mutex_;
};

// Commands a read-only replica must refuse from ordinary clients.
bool is_write_command(const std::string& upper_name);

}  // namespace rp
