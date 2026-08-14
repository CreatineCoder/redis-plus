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

// The keyspace.
//
// Values are strings for now; Phase 6 widens Entry to a variant over the other
// types. Expiry here is lazy (checked on access) exactly as in the reference
// implementation -- the difference is that this one is testable with an
// injected clock, and Phase 3 hangs the active expiry cycle off `sample_keys`
// so expired-but-never-touched keys stop leaking.
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

  // Physical entry count, including expired-but-unreaped keys. Phase 3's
  // gate is that this converges on `size()` under churn.
  std::size_t raw_size() const { return map_.size(); }
  std::size_t size();

  void clear() { map_.clear(); }
  std::int64_t clock() const { return clock_(); }

  // Reap up to `limit` expired keys from an arbitrary position. Phase 3's
  // active cycle drives this; exposed now so the data structure never needs
  // to change shape later.
  std::size_t reap_expired(std::size_t limit);

 private:
  struct Entry {
    std::string value;
    std::int64_t expire_at = kNoExpiry;
  };

  bool expired(const Entry& e) const {
    return e.expire_at != kNoExpiry && clock_() >= e.expire_at;
  }

  std::unordered_map<std::string, Entry> map_;
  Clock clock_;
};

}  // namespace rp
