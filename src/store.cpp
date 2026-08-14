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
  if (expired(it->second)) {
    map_.erase(it);
    return std::nullopt;
  }
  return it->second.value;
}

void Store::set(const std::string& key, std::string value,
                std::int64_t expire_at) {
  auto& entry = map_[key];
  entry.value = std::move(value);
  entry.expire_at = expire_at;
}

bool Store::erase(const std::string& key) {
  auto it = map_.find(key);
  if (it == map_.end()) return false;
  const bool was_live = !expired(it->second);
  map_.erase(it);
  return was_live;  // deleting an already-expired key reports 0, as redis does
}

bool Store::contains(const std::string& key) { return get(key).has_value(); }

std::int64_t Store::pttl(const std::string& key) {
  auto it = map_.find(key);
  if (it == map_.end()) return -2;
  if (expired(it->second)) {
    map_.erase(it);
    return -2;
  }
  if (it->second.expire_at == kNoExpiry) return -1;
  return it->second.expire_at - clock_();
}

bool Store::expire_at(const std::string& key, std::int64_t deadline_ms) {
  auto it = map_.find(key);
  if (it == map_.end() || expired(it->second)) return false;
  if (deadline_ms <= clock_()) {
    map_.erase(it);  // a deadline in the past deletes immediately
    return true;
  }
  it->second.expire_at = deadline_ms;
  return true;
}

bool Store::persist(const std::string& key) {
  auto it = map_.find(key);
  if (it == map_.end() || expired(it->second)) return false;
  if (it->second.expire_at == kNoExpiry) return false;
  it->second.expire_at = kNoExpiry;
  return true;
}

std::vector<std::string> Store::keys(std::string_view pattern) {
  std::vector<std::string> out;
  for (auto it = map_.begin(); it != map_.end();) {
    if (expired(it->second)) {
      it = map_.erase(it);
      continue;
    }
    if (glob_match(pattern, it->first)) out.push_back(it->first);
    ++it;
  }
  return out;
}

std::size_t Store::size() {
  std::size_t live = 0;
  for (const auto& [key, entry] : map_) {
    if (!expired(entry)) ++live;
  }
  return live;
}

std::size_t Store::reap_expired(std::size_t limit) {
  std::size_t reaped = 0;
  for (auto it = map_.begin(); it != map_.end() && reaped < limit;) {
    if (expired(it->second)) {
      it = map_.erase(it);
      ++reaped;
    } else {
      ++it;
    }
  }
  return reaped;
}

}  // namespace rp
