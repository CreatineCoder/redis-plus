#pragma once

#include <atomic>
#include <cstdint>
#include <string>

namespace rp {

// Process-wide counters. These are the M1 instrumentation surface: the
// benchmark harness reads them through INFO rather than guessing from the
// outside, and they are what every phase's "meaningful number" is derived from.
//
// Relaxed ordering throughout -- these are monotonic counters for reporting,
// never used to synchronise anything.
struct Stats {
  std::atomic<std::uint64_t> connections_received{0};
  std::atomic<std::uint64_t> connections_active{0};
  std::atomic<std::uint64_t> connections_peak{0};
  std::atomic<std::uint64_t> connections_rejected{0};
  std::atomic<std::uint64_t> commands_processed{0};
  std::atomic<std::uint64_t> bytes_read{0};
  std::atomic<std::uint64_t> bytes_written{0};
  std::atomic<std::uint64_t> output_buffer_overflows{0};

  void on_connect() {
    connections_received.fetch_add(1, std::memory_order_relaxed);
    const std::uint64_t now =
        connections_active.fetch_add(1, std::memory_order_relaxed) + 1;
    std::uint64_t peak = connections_peak.load(std::memory_order_relaxed);
    while (now > peak && !connections_peak.compare_exchange_weak(
                             peak, now, std::memory_order_relaxed)) {
    }
  }

  void on_disconnect() {
    connections_active.fetch_sub(1, std::memory_order_relaxed);
  }

  static Stats& instance() {
    static Stats s;
    return s;
  }

  // INFO-compatible text. Deliberately mirrors real Redis field names so that
  // redis-cli INFO and existing tooling read it without special casing.
  std::string to_info() const;
};

}  // namespace rp
