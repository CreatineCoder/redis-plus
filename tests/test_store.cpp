// Store tests drive a fake clock rather than sleeping, so expiry semantics are
// exercised exactly and the suite stays fast.

#include "rp/store.hpp"

#include <gtest/gtest.h>

namespace {

struct StoreTest : public ::testing::Test {
  std::int64_t now = 1'000'000;
  rp::Store store{[this] { return now; }};
};

TEST_F(StoreTest, SetGet) {
  store.set("k", "v");
  ASSERT_TRUE(store.get("k").has_value());
  EXPECT_EQ(*store.get("k"), "v");
}

TEST_F(StoreTest, MissingKey) { EXPECT_FALSE(store.get("nope").has_value()); }

TEST_F(StoreTest, Overwrite) {
  store.set("k", "a");
  store.set("k", "b");
  EXPECT_EQ(*store.get("k"), "b");
}

TEST_F(StoreTest, EraseReportsLiveness) {
  store.set("k", "v");
  EXPECT_TRUE(store.erase("k"));
  EXPECT_FALSE(store.erase("k"));
}

TEST_F(StoreTest, KeyIsLiveUpToDeadlineAndDeadAtIt) {
  store.set("k", "v", now + 100);
  now += 99;
  EXPECT_TRUE(store.get("k").has_value());
  now += 1;  // exactly at the deadline
  EXPECT_FALSE(store.get("k").has_value());
}

TEST_F(StoreTest, ExpiredKeyIsReapedOnAccess) {
  store.set("k", "v", now + 10);
  now += 20;
  EXPECT_FALSE(store.get("k").has_value());
  EXPECT_EQ(store.raw_size(), 0u);  // lazily reaped, not merely hidden
}

TEST_F(StoreTest, PttlReportsRemaining) {
  store.set("k", "v", now + 5000);
  EXPECT_EQ(store.pttl("k"), 5000);
  now += 1000;
  EXPECT_EQ(store.pttl("k"), 4000);
}

TEST_F(StoreTest, PttlSentinels) {
  EXPECT_EQ(store.pttl("absent"), -2);
  store.set("k", "v");
  EXPECT_EQ(store.pttl("k"), -1);
}

TEST_F(StoreTest, ExpireAndPersist) {
  store.set("k", "v");
  EXPECT_TRUE(store.expire_at("k", now + 1000));
  EXPECT_EQ(store.pttl("k"), 1000);
  EXPECT_TRUE(store.persist("k"));
  EXPECT_EQ(store.pttl("k"), -1);
  EXPECT_FALSE(store.persist("k"));  // already persistent
}

TEST_F(StoreTest, ExpireInThePastDeletesImmediately) {
  store.set("k", "v");
  EXPECT_TRUE(store.expire_at("k", now - 1));
  EXPECT_FALSE(store.get("k").has_value());
}

TEST_F(StoreTest, ExpireOnMissingKeyFails) {
  EXPECT_FALSE(store.expire_at("absent", now + 1000));
}

TEST_F(StoreTest, KeysFiltersExpired) {
  store.set("live", "v");
  store.set("dead", "v", now + 10);
  now += 20;
  const auto found = store.keys("*");
  ASSERT_EQ(found.size(), 1u);
  EXPECT_EQ(found[0], "live");
}

TEST_F(StoreTest, KeysGlob) {
  store.set("user:1", "a");
  store.set("user:2", "b");
  store.set("admin:1", "c");
  EXPECT_EQ(store.keys("user:*").size(), 2u);
  EXPECT_EQ(store.keys("*").size(), 3u);
}

TEST_F(StoreTest, SizeCountsOnlyLiveKeys) {
  store.set("a", "1");
  store.set("b", "2", now + 10);
  now += 20;
  EXPECT_EQ(store.size(), 1u);
  EXPECT_EQ(store.raw_size(), 2u);  // Phase 3 closes this gap
}

// Keys that expire but are never touched again accumulate. This documents the
// leak Phase 3's active cycle exists to fix, and proves the reaper primitive
// it will be built on already works.
TEST_F(StoreTest, UntouchedExpiredKeysAccumulateUntilReaped) {
  for (int i = 0; i < 1000; ++i) {
    store.set("k" + std::to_string(i), "v", now + 10);
  }
  now += 100;
  EXPECT_EQ(store.raw_size(), 1000u);
  EXPECT_EQ(store.size(), 0u);

  EXPECT_EQ(store.reap_expired(400), 400u);
  EXPECT_EQ(store.raw_size(), 600u);
  store.reap_expired(1000);
  EXPECT_EQ(store.raw_size(), 0u);
}

}  // namespace
