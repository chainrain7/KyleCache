#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>

#include <fmt/base.h>
#include <spdlog/spdlog.h>

#include "kylecache/cache.h"
#include "kylecache/group.h"

namespace kylecache {

namespace {
// 全局缓存组注册表：限制在本编译单元内，避免污染对外符号
std::unordered_map<std::string, KyleCacheGroup> cache_groups;
std::mutex groups_mtx;
}  // namespace

auto MakeCacheGroup(const std::string& name, int64_t bytes, DataGetter getter) -> KyleCacheGroup& {
    if (getter == nullptr) {
        throw std::invalid_argument("cache group [" + name + "] requires a non-null getter");
    }
    std::lock_guard lock{groups_mtx};
    cache_groups[name] = std::move(KyleCacheGroup{name, bytes, getter});
    return cache_groups[name];
}

auto GetCacheGroup(const std::string& name) -> KyleCacheGroup* {
    std::lock_guard lock{groups_mtx};
    auto it = cache_groups.find(name);
    if (it == cache_groups.end()) {
        return nullptr;
    }
    return &it->second;
}

auto KyleCacheGroup::Get(const std::string& key) -> CacheValueOptional {
    if (is_close_) {
        spdlog::error("Cache group [{}] is closed!!!", name_);
        return std::nullopt;
    }

    if (key.empty()) {
        spdlog::warn("The key [{}] is empty, you can't get a empty key from cache group", key);
        return std::nullopt;
    }

    // 先从本地缓存中获取
    auto ret = cache_->Get(key);
    if (ret) {
        ++status_.local_hits;  // 本地命中缓存次数+1
        return ret;
    }

    ++status_.local_misses;  // 本地未命中缓存次数+1
    return Load(key);
}

bool KyleCacheGroup::Set(const std::string& key, CacheValue b) {
    if (is_close_) {
        spdlog::error("Cache group [{}] is closed!!!", name_);
        return false;
    }
    if (key.empty()) {
        spdlog::warn("The key [{}] is empty, you can't set it into cache group", key);
        return false;
    }
    cache_->Set(key, b);
    spdlog::debug("key:{} is set value:{}", key, b.ToString());
    return true;
}

bool KyleCacheGroup::Delete(const std::string& key) {
    if (is_close_) {
        spdlog::error("Cache group [{}] is closed!!!", name_);
        return false;
    }
    if (key.empty()) {
        spdlog::warn("The key [{}] is empty, you can't delete it from cache group", key);
        return false;
    }
    cache_->Delete(key);
    spdlog::debug("key:{} is deleted", key);
    return true;
}

bool KyleCacheGroup::InvalidateFromPeer(const std::string& key) {
    if (is_close_) {
        spdlog::error("Cache group [{}] is closed!!!", name_);
        return false;
    }
    if (key.empty()) {
        spdlog::warn("The key [{}] is empty, you can't invalidate it from cache group", key);
        return false;
    }

    // 来自其他节点的失效请求，删除本地缓存
    cache_->Delete(key);
    spdlog::debug("Invalidated key [{}] from local cache (from peer)", key);
    return true;
}

auto KyleCacheGroup::Load(const std::string& key) -> CacheValueOptional {
    ++status_.loads;
    const auto start = std::chrono::steady_clock::now();
    auto record_duration = [&] {
        status_.load_duration_ns +=
            std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start)
                .count();
    };

    CacheValueOptional ret;
    try {
        ret = loader_.Do(key, [&] { return LoadData(key); });
    } catch (const std::exception& e) {
        record_duration();
        ++status_.loader_errors;
        // getter 抛出的异常在这里消化掉：不能让它沿 RPC 处理链传播导致进程退出
        spdlog::error("Load key [{}] threw an exception: {}", key, e.what());
        return std::nullopt;
    }
    record_duration();

    if (!ret) {
        ++status_.loader_errors;
        spdlog::error("Failed to load data for key: {}", key);
        return std::nullopt;
    }

    ++status_.loader_hits;
    cache_->Set(key, ret.value());
    return ret;
}

auto KyleCacheGroup::LoadData(const std::string& key) -> CacheValueOptional {
    spdlog::info("Try to load key [{}] from local", key);
    // 通过getter从数据源获取
    return getter_(key);
}

auto KyleCacheGroup::Stats() const -> GroupStats {
    GroupStats s;
    s.loads = status_.loads.load();
    s.local_hits = status_.local_hits.load();
    s.local_misses = status_.local_misses.load();
    s.loader_hits = status_.loader_hits.load();
    s.loader_errors = status_.loader_errors.load();
    s.load_duration_ns = status_.load_duration_ns.load();
    return s;
}

}  // namespace kylecache
