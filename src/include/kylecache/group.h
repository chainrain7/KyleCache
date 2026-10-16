#ifndef CACHE_H_
#define CACHE_H_

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <utility>

#include "kylecache/cache.h"
#include "kylecache/singleflight.h"

namespace kylecache {

using DataGetter = std::function<CacheValueOptional(const std::string& key)>;

// 统计快照（普通值类型，通过 KyleCacheGroup::Stats() 获取）
struct GroupStats {
    int64_t loads = 0;             // 回源加载次数
    int64_t local_hits = 0;        // 本地缓存命中次数
    int64_t local_misses = 0;      // 本地缓存未命中次数
    int64_t loader_hits = 0;       // 回源成功次数
    int64_t loader_errors = 0;     // 回源失败次数
    int64_t load_duration_ns = 0;  // 回源累计耗时（纳秒）
};

enum class SyncFlag {
    SET,
    DELETE,
    INVALIDATE,  // 缓存失效，只删除本地缓存，不通过getter重新加载
};

class KyleCacheGroup {
public:
    KyleCacheGroup() = default;

    KyleCacheGroup(std::string name, int64_t bytes, DataGetter getter)
        : cache_(std::make_unique<LRUCache>(bytes)), name_(name), getter_(getter) {}

    KyleCacheGroup(const KyleCacheGroup&) = delete;

    auto operator=(const KyleCacheGroup& other) -> KyleCacheGroup& = delete;

    // 手写移动语义：SingleFlight 内含 mutex 不可移动，
    // 移动后的新对象使用全新的 SingleFlight 与计数器
    KyleCacheGroup(KyleCacheGroup&& other) {
        cache_ = std::move(other.cache_);
        name_ = std::move(other.name_);
        getter_ = std::move(other.getter_);
    }

    auto operator=(KyleCacheGroup&& other) -> KyleCacheGroup& {
        cache_ = std::move(other.cache_);
        name_ = std::move(other.name_);
        getter_ = std::move(other.getter_);
        return *this;
    }

    auto Get(const std::string& key) -> CacheValueOptional;

    bool Set(const std::string& key, CacheValue b);

    bool Delete(const std::string& key);

    // 处理来自其他节点的失效请求
    bool InvalidateFromPeer(const std::string& key);

    // 当前统计快照
    auto Stats() const -> GroupStats;

private:
    auto Load(const std::string& key) -> CacheValueOptional;
    auto LoadData(const std::string& key) -> CacheValueOptional;

private:
    std::unique_ptr<LRUCache> cache_;
    std::string name_;
    std::atomic<bool> is_close_{false};
    DataGetter getter_;
    SingleFlight loader_;

    // 内部原子计数器（C++17 中 std::atomic 整型默认构造不初始化，必须显式置 0）
    struct {
        std::atomic_int64_t loads{0};
        std::atomic_int64_t local_hits{0};
        std::atomic_int64_t local_misses{0};
        std::atomic_int64_t loader_hits{0};
        std::atomic_int64_t loader_errors{0};
        std::atomic_int64_t load_duration_ns{0};
    } status_;
};

// 创建（或覆盖同名）全局缓存组；getter 为空时抛出 std::invalid_argument
auto MakeCacheGroup(const std::string& name, int64_t bytes, DataGetter getter) -> KyleCacheGroup&;
auto GetCacheGroup(const std::string& name) -> KyleCacheGroup*;

}  // namespace kylecache

#endif /* CACHE_H_ */
