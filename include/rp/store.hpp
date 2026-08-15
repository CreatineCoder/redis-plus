#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace rp {

// Absolute expiry deadline in ms since the epoch. kNoExpiry == persistent.
inline constexpr std::int64_t kNoExpiry = -1;

std::int64_t now_ms();

// Tuning for the active expiry cycle. Defaults mirror real redis with hz=10:
// sample 20 keys per pass, and keep going while more than 25% of the sample
// turned out to be expired -- so a keyspace that is mostly dead gets cleaned
// aggressively while a healthy one costs almost nothing.
struct ExpiryConfig {
  std::size_t sample_size = 20;
  int continue_threshold_pct = 25;
  std::size_t max_passes = 16;  // bounds the time one cycle can steal
};

struct ExpiryStats {
  std::uint64_t expired_lazy = 0;    // reaped on access
  std::uint64_t expired_active = 0;  // reaped by the cycle
  std::uint64_t cycles = 0;
  std::uint64_t passes = 0;
};

// The keyspace.
//
// Values are strings for now; Phase 6 widens Entry to a variant. Keys carrying
// a TTL are additionally indexed in `expires_`, exactly as redis does, so the
// active cycle samples only expiry candidates instead of walking the whole
// keyspace -- that index is what makes Phase 3's memory gate reachable.
class Store {
 public:
  using Clock = std::function<std::int64_t()>;

  Store() : clock_(&now_ms) {}
  explicit Store(Clock clock) : clock_(std::move(clock)) {}

  // Returns nullopt if absent or logically expired (and reaps it).
  std::optional<std::string> get(const std::string& key);

  void set(const std::string& key, std::string value,
           std::int64_t expire_at = kNoExpiry);

  bool erase(const std::string& key);
  bool contains(const std::string& key);

  // -2 no key, -1 no ttl, else ms remaining.
  std::int64_t pttl(const std::string& key);
  bool expire_at(const std::string& key, std::int64_t deadline_ms);
  bool persist(const std::string& key);

  std::vector<std::string> keys(std::string_view pattern);

  // Physical entry count, including expired-but-unreaped keys. Phase 3's gate
  // is that this stays close to `size()` under churn instead of growing.
  std::size_t raw_size() const { return map_.size(); }
  std::size_t size();
  std::size_t expires_size() const { return expires_.size(); }

  void clear() {
    map_.clear();
    expires_.clear();
    cursor_ = 0;
  }
  std::int64_t clock() const { return clock_(); }
  const ExpiryStats& expiry_stats() const { return stats_; }

  // One active expiry cycle: repeated sampling passes over the expires index.
  // Returns the number of keys reaped. Cheap and bounded -- safe to call from
  // the event loop between reads.
  std::size_t active_expire_cycle(const ExpiryConfig& cfg = {});

  // Reap up to `limit` expired keys. Used by tests and by shutdown paths;
  // the event loop uses active_expire_cycle().
  std::size_t reap_expired(std::size_t limit);

 private:
  struct Entry {
    std::string value;
  };

  bool is_expired(const std::string& key) const {
    const auto it = expires_.find(key);
    return it != expires_.end() && clock_() >= it->second;
  }

  void remove(const std::string& key) {
    map_.erase(key);
    expires_.erase(key);
  }

  // Collect up to `n` keys from the expires index, resuming where the last
  // call stopped so the cycle sweeps the whole index over time rather than
  // re-examining the same keys forever.
  std::vector<const std::string*> sample_expiring(std::size_t n);

  std::unordered_map<std::string, Entry> map_;
  std::unordered_map<std::string, std::int64_t> expires_;
  std::size_t cursor_ = 0;  // bucket index into expires_
  ExpiryStats stats_;
  Clock clock_;
};

}  // namespace rp
