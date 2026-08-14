// Connection scaler (M0).
//
// redis-benchmark measures throughput but will not push a server into the
// thousands of *concurrent* connections, which is the specific claim Phase 1
// has to support. This tool opens N connections, keeps them all alive, sends a
// PING on each, and reports how many completed plus the latency distribution.
//
//   ./conn_scale --host 127.0.0.1 --port 6379 --conns 5000 --rounds 3
//
// Report the largest N where failed == 0 and p99 stays flat. That number, and
// the ratio against the same run on the legacy server, is what goes on paper.

#include <asio.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

namespace {

using asio::ip::tcp;
using Clock = std::chrono::steady_clock;

struct Options {
  std::string host = "127.0.0.1";
  std::uint16_t port = 6379;
  int conns = 1000;
  int rounds = 1;
};

double percentile(std::vector<double>& v, double p) {
  if (v.empty()) return 0.0;
  const std::size_t idx = static_cast<std::size_t>(p / 100.0 * (v.size() - 1));
  std::nth_element(v.begin(), v.begin() + idx, v.end());
  return v[idx];
}

}  // namespace

int main(int argc, char** argv) {
  Options opt;
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    if (i + 1 >= argc) break;
    if (a == "--host") opt.host = argv[++i];
    else if (a == "--port") opt.port = static_cast<std::uint16_t>(std::stoi(argv[++i]));
    else if (a == "--conns") opt.conns = std::stoi(argv[++i]);
    else if (a == "--rounds") opt.rounds = std::stoi(argv[++i]);
  }

  asio::io_context io;
  const tcp::endpoint ep(asio::ip::make_address(opt.host), opt.port);

  std::vector<std::unique_ptr<tcp::socket>> sockets;
  sockets.reserve(opt.conns);

  int failed_connect = 0;
  const auto connect_start = Clock::now();
  for (int i = 0; i < opt.conns; ++i) {
    auto s = std::make_unique<tcp::socket>(io);
    asio::error_code ec;
    s->connect(ep, ec);
    if (ec) {
      ++failed_connect;
      continue;
    }
    s->set_option(tcp::no_delay(true), ec);
    sockets.push_back(std::move(s));
  }
  const double connect_ms =
      std::chrono::duration<double, std::milli>(Clock::now() - connect_start)
          .count();

  std::cout << "established: " << sockets.size() << "/" << opt.conns
            << "  failed: " << failed_connect << "  connect_time: "
            << connect_ms << " ms\n";

  const std::string ping = "*1\r\n$4\r\nPING\r\n";
  std::vector<double> latencies;
  latencies.reserve(sockets.size() * static_cast<std::size_t>(opt.rounds));
  int io_errors = 0;

  for (int round = 0; round < opt.rounds; ++round) {
    for (auto& s : sockets) {
      char reply[7];
      asio::error_code ec;
      const auto t0 = Clock::now();
      asio::write(*s, asio::buffer(ping), ec);
      if (!ec) asio::read(*s, asio::buffer(reply, sizeof(reply)), ec);
      if (ec) {
        ++io_errors;
        continue;
      }
      latencies.push_back(
          std::chrono::duration<double, std::milli>(Clock::now() - t0).count());
    }
  }

  std::cout << "round-trips: " << latencies.size() << "  io_errors: "
            << io_errors << "\n"
            << "latency_ms p50=" << percentile(latencies, 50)
            << " p95=" << percentile(latencies, 95)
            << " p99=" << percentile(latencies, 99)
            << " max=" << percentile(latencies, 100) << "\n";

  return (failed_connect == 0 && io_errors == 0) ? 0 : 1;
}
