#include <gtest/gtest.h>

#include <optional>
#include <string>
#include <vector>

#include "kylecache/cache.h"

TEST(LRUCacheTest, TestGet) {
    kylecache::LRUCache cache{100, nullptr};
    auto ret = cache.Get("1");
    EXPECT_EQ(ret, std::nullopt);

    cache.Set("abcdefg", kylecache::CacheValue{"abcdefg"});
    ret = cache.Get("abcdefg");
    EXPECT_NE(ret, std::nullopt);
    EXPECT_EQ(ret.value().ToString(), "abcdefg");

    cache.Set("11", kylecache::CacheValue{"22"});
    ret = cache.Get("11");
    EXPECT_NE(ret, std::nullopt);
    EXPECT_EQ(ret.value().ToString(), "22");

    cache.Set("123456789", kylecache::CacheValue{"123456789"});
    ret = cache.Get("123456789");
    EXPECT_NE(ret, std::nullopt);
    EXPECT_EQ(ret.value().ToString(), "123456789");
}

TEST(LRUCacheTest, TestRemoveOldest) {
    kylecache::LRUCache cache{40, nullptr};
    cache.Set("12345", kylecache::CacheValue{"abcde"});
    cache.Set("67890", kylecache::CacheValue{"fghij"});
    cache.Set("xxxxx", kylecache::CacheValue{"11111"});
    cache.Set("yyyyy", kylecache::CacheValue{"22222"});

    // 这个时候应该已经满了
    // 再加入新的缓存，原来最旧的缓存 {"12345", "abcde"}会被淘汰
    cache.Set("zzzzz", kylecache::CacheValue{"33333"});

    auto ret = cache.Get("12345");
    EXPECT_EQ(ret, std::nullopt);

    ret = cache.Get("67890");
    EXPECT_EQ(ret.value().ToString(), "fghij");
}

TEST(LRUCacheTest, TestEvictedFunc) {
    std::vector<kylecache::Entry> kvs;
    auto evicted_func = [&](std::string key, const kylecache::CacheValue& value) {
        std::cout << "test evicted function...\n";
        kvs.emplace_back(kylecache::Entry{key, value});
    };
    kylecache::LRUCache cache{10, evicted_func};

    // 容量只有10，也就是在完成下面四次插入后，key1 和 k2 会被淘汰
    cache.Set("key1", kylecache::CacheValue{"123456"});
    cache.Set("k2", kylecache::CacheValue{"v2"});
    cache.Set("k3", kylecache::CacheValue{"v3"});
    cache.Set("k4", kylecache::CacheValue{"v4"});

    std::vector<kylecache::Entry> expected{{"key1", kylecache::CacheValue{"123456"}}, {"k2", kylecache::CacheValue{"v2"}}};
    EXPECT_EQ(kvs, expected);
}