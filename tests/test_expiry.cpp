// Phase 3: the active expiry cycle.
//
// Lazy expiry alone leaks: a key with a TTL that is never read again stays
// allocated forever. These tests pin down that the cycle reclaims it, that it
// costs almost nothing when the keyspace is healthy, and that it never touches
// a live key.

#include <gtest/gtest.h>

#include <algorithm>
#include <string>

#include "rp/store.hpp"

namespace {

struct ExpiryTest : public ::testing::Test {
  std::int64_t now = 1'000'000;
  rp::Store store{[this] { return now; }};

  void fill(int count, const char* prefix, std::int64_t ttl_ms) {
    for (int i = 0; i < count; ++i) {
      store.set(std::string(prefix) + std::to_string(i), "v",
                ttl_ms < 0 ? rp::kNoExpiry : now + ttl_ms);
    }
  }
};

TEST_F(ExpiryTest, ReapsExpiredKeysNobodyTouched) {
  fill(500, "k", 100);
  now += 1000;

  ASSERT_EQ(store.raw_size(), 500u);
  while (store.raw_size() > 0) store.active_expire_cycle();
  EXPECT_EQ(store.raw_size(), 0u);
  EXPECT_EQ(store.expires_size(), 0u);
}

TEST_F(ExpiryTest, NeverReapsLiveKeys) {
  fill(200, "live", 100000);
  fill(200, "dead", 10);
  now += 1000;

  for (int i = 0; i < 200; ++i) store.active_expire_cycle();

  EXPECT_EQ(store.size(), 200u);
  for (int i = 0; i < 200; ++i) {
    EXPECT_TRUE(store.get("live" + std::to_string(i)).has_value());
  }
}

TEST_F(ExpiryTest, LeavesPersistentKeysAlone) {
  fill(100, "forever", -1);
  now += 1'000'000;
  for (int i = 0; i < 50; ++i) store.active_expire_cycle();
  EXPECT_EQ(store.size(), 100u);
}

// A healthy keyspace must not pay for the cycle: one sample pass finds nothing
// expired and stops immediately rather than sweeping the index.
TEST_F(ExpiryTest, StopsEarlyWhenKeyspaceIsHealthy) {
  fill(10000, "live", 100000);
  const auto before = store.expiry_stats().passes;
  store.active_expire_cycle();
  EXPECT_LE(store.expiry_stats().passes - before, 1u);
}

// A rotting keyspace must be cleaned aggressively: >25% dead keeps passing.
TEST_F(ExpiryTest, KeepsPassingWhileKeyspaceIsRotten) {
  fill(2000, "dead", 10);
  now += 1000;
  const auto before = store.expiry_stats().passes;
  store.active_expire_cycle();
  EXPECT_GT(store.expiry_stats().passes - before, 1u);
}

TEST_F(ExpiryTest, OneCycleIsBounded) {
  fill(100000, "dead", 10);
  now += 1000;

  rp::ExpiryConfig cfg;  // sample 20, at most 16 passes
  const std::size_t reaped = store.active_expire_cycle(cfg);
  EXPECT_LE(reaped, cfg.sample_size * cfg.max_passes)
      << "a single cycle must not stall the event loop";
  EXPECT_GT(reaped, 0u);
}

// The cursor must advance, or the cycle re-samples the same buckets forever
// and keys at the far end of the index are never reclaimed.
TEST_F(ExpiryTest, CursorSweepsTheWholeIndex) {
  fill(5000, "dead", 10);
  now += 1000;

  for (int i = 0; i < 2000 && store.raw_size() > 0; ++i) {
    store.active_expire_cycle();
  }
  EXPECT_EQ(store.raw_size(), 0u) << "cursor failed to reach some buckets";
}

// The Phase 3 gate, in miniature: writes with short TTLs arriving continuously
// while the cron runs. Tracked keys must plateau near the live set rather than
// growing with total writes.
TEST_F(ExpiryTest, MemoryStaysBoundedUnderChurn) {
  std::size_t peak_tracked = 0;

  for (int round = 0; round < 500; ++round) {
    for (int i = 0; i < 20; ++i) {
      store.set("churn:" + std::to_string(round) + ":" + std::to_string(i), "v",
                now + 50);
    }
    now += 10;
    store.active_expire_cycle();
    peak_tracked = std::max(peak_tracked, store.raw_size());
  }

  // 10,000 keys were written; without the cycle all 10,000 would be resident.
  EXPECT_LT(peak_tracked, 1000u) << "tracked keys grew with total writes";
  EXPECT_GT(store.expiry_stats().expired_active, 5000u);
}

TEST_F(ExpiryTest, StatsDistinguishLazyFromActive) {
  store.set("touched", "v", now + 10);
  store.set("untouched", "v", now + 10);
  now += 100;

  EXPECT_FALSE(store.get("touched").has_value());  // lazy path
  EXPECT_EQ(store.expiry_stats().expired_lazy, 1u);

  while (store.raw_size() > 0) store.active_expire_cycle();
  EXPECT_EQ(store.expiry_stats().expired_active, 1u);
}

TEST_F(ExpiryTest, CycleOnEmptyStoreIsHarmless) {
  EXPECT_EQ(store.active_expire_cycle(), 0u);
  store.set("k", "v");  // no TTL: index stays empty
  EXPECT_EQ(store.active_expire_cycle(), 0u);
  EXPECT_TRUE(store.get("k").has_value());
}

}  // namespace
