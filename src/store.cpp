#include "rp/store.hpp"

#include <chrono>
#include <utility>

#include "rp/resp.hpp"

namespace rp {

std::int64_t now_ms() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

std::optional<std::string> Store::get(const std::string& key) {
  auto it = map_.find(key);
  if (it == map_.end()) return std::nullopt;
  if (is_expired(key)) {
    remove(key);
    ++stats_.expired_lazy;
    return std::nullopt;
  }
  return it->second.value;
}

void Store::set(const std::string& key, std::string value,
                std::int64_t expire_at) {
  map_[key].value = std::move(value);
  if (expire_at == kNoExpiry) {
    expires_.erase(key);
  } else {
    expires_[key] = expire_at;
  }
}

bool Store::erase(const std::string& key) {
  if (map_.find(key) == map_.end()) return false;
  const bool was_live = !is_expired(key);
  if (!was_live) ++stats_.expired_lazy;
  remove(key);
  return was_live;  // deleting an already-expired key reports 0, as redis does
}

bool Store::contains(const std::string& key) { return get(key).has_value(); }

std::int64_t Store::pttl(const std::string& key) {
  if (map_.find(key) == map_.end()) return -2;
  if (is_expired(key)) {
    remove(key);
    ++stats_.expired_lazy;
    return -2;
  }
  const auto it = expires_.find(key);
  if (it == expires_.end()) return -1;
  return it->second - clock_();
}

bool Store::expire_at(const std::string& key, std::int64_t deadline_ms) {
  if (map_.find(key) == map_.end()) return false;
  if (is_expired(key)) {
    remove(key);
    ++stats_.expired_lazy;
    return false;
  }
  if (deadline_ms <= clock_()) {
    remove(key);  // a deadline in the past deletes immediately
    return true;
  }
  expires_[key] = deadline_ms;
  return true;
}

bool Store::persist(const std::string& key) {
  if (map_.find(key) == map_.end()) return false;
  if (is_expired(key)) {
    remove(key);
    ++stats_.expired_lazy;
    return false;
  }
  return expires_.erase(key) > 0;
}

std::vector<std::string> Store::keys(std::string_view pattern) {
  std::vector<std::string> out;
  std::vector<std::string> dead;
  for (const auto& [key, entry] : map_) {
    if (is_expired(key)) {
      dead.push_back(key);
      continue;
    }
    if (glob_match(pattern, key)) out.push_back(key);
  }
  for (const auto& key : dead) {
    remove(key);
    ++stats_.expired_lazy;
  }
  return out;
}

std::size_t Store::size() {
  std::size_t expired = 0;
  const std::int64_t now = clock_();
  for (const auto& [key, deadline] : expires_) {
    if (now >= deadline) ++expired;
  }
  return map_.size() - expired;
}

std::vector<const std::string*> Store::sample_expiring(std::size_t n) {
  std::vector<const std::string*> out;
  if (expires_.empty() || n == 0) return out;

  const std::size_t buckets = expires_.bucket_count();
  if (buckets == 0) return out;
  if (cursor_ >= buckets) cursor_ = 0;

  // Walk buckets from the persistent cursor, wrapping at most once so an
  // index full of live keys cannot spin here.
  for (std::size_t visited = 0; visited < buckets && out.size() < n;
       ++visited) {
    const std::size_t bucket = (cursor_ + visited) % buckets;
    for (auto it = expires_.begin(bucket);
         it != expires_.end(bucket) && out.size() < n; ++it) {
      out.push_back(&it->first);
    }
  }
  cursor_ = (cursor_ + 1) % buckets;
  return out;
}

std::size_t Store::active_expire_cycle(const ExpiryConfig& cfg) {
  ++stats_.cycles;
  std::size_t total = 0;

  for (std::size_t pass = 0; pass < cfg.max_passes; ++pass) {
    if (expires_.empty()) break;
    ++stats_.passes;

    const auto sampled = sample_expiring(cfg.sample_size);
    if (sampled.empty()) break;

    // Copy out the keys to delete: erasing invalidates the pointers into
    // expires_ that `sampled` holds.
    const std::int64_t now = clock_();
    std::vector<std::string> dead;
    for (const auto* key : sampled) {
      const auto it = expires_.find(*key);
      if (it != expires_.end() && now >= it->second) dead.push_back(*key);
    }
    for (const auto& key : dead) remove(key);

    total += dead.size();
    stats_.expired_active += dead.size();

    // Stop early once the sample comes back mostly alive -- the keyspace is
    // healthy and further passes would be wasted work.
    const std::size_t threshold =
        sampled.size() * static_cast<std::size_t>(cfg.continue_threshold_pct) /
        100;
    if (dead.size() <= threshold) break;
  }

  return total;
}

std::vector<Record> Store::snapshot() {
  std::vector<Record> out;
  out.reserve(map_.size());
  const std::int64_t now = clock_();
  for (const auto& [key, entry] : map_) {
    const auto it = expires_.find(key);
    const std::int64_t deadline = (it == expires_.end()) ? kNoExpiry : it->second;
    if (deadline != kNoExpiry && now >= deadline) continue;  // already dead
    out.push_back(Record{key, entry.value, deadline});
  }
  return out;
}

void Store::load_record(const Record& record) {
  if (record.expire_at != kNoExpiry && clock_() >= record.expire_at) {
    return;  // deadline already passed while the file sat on disk
  }
  set(record.key, record.value, record.expire_at);
}

std::size_t Store::reap_expired(std::size_t limit) {
  std::vector<std::string> dead;
  const std::int64_t now = clock_();
  for (const auto& [key, deadline] : expires_) {
    if (dead.size() >= limit) break;
    if (now >= deadline) dead.push_back(key);
  }
  for (const auto& key : dead) remove(key);
  stats_.expired_active += dead.size();
  return dead.size();
}

}  // namespace rp
