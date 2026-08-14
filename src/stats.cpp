#include "rp/stats.hpp"

#include <chrono>
#include <sstream>

namespace rp {
namespace {

std::uint64_t uptime_seconds() {
  static const auto start = std::chrono::steady_clock::now();
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::seconds>(
          std::chrono::steady_clock::now() - start)
          .count());
}

}  // namespace

std::string Stats::to_info() const {
  std::ostringstream os;
  const auto load = [](const std::atomic<std::uint64_t>& a) {
    return a.load(std::memory_order_relaxed);
  };

  os << "# Server\r\n"
     << "redis_version:7.0.0-rp\r\n"
     << "uptime_in_seconds:" << uptime_seconds() << "\r\n"
     << "\r\n"
     << "# Clients\r\n"
     << "connected_clients:" << load(connections_active) << "\r\n"
     << "peak_connected_clients:" << load(connections_peak) << "\r\n"
     << "rejected_connections:" << load(connections_rejected) << "\r\n"
     << "\r\n"
     << "# Stats\r\n"
     << "total_connections_received:" << load(connections_received) << "\r\n"
     << "total_commands_processed:" << load(commands_processed) << "\r\n"
     << "total_net_input_bytes:" << load(bytes_read) << "\r\n"
     << "total_net_output_bytes:" << load(bytes_written) << "\r\n"
     << "client_output_buffer_overflows:" << load(output_buffer_overflows)
     << "\r\n";
  return os.str();
}

}  // namespace rp
