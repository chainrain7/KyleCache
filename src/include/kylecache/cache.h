#ifndef LRU_H_
#define LRU_H_

#include <cstdint>
#include <functional>
#include <list>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>

namespace kylecache {

// 缓存值类型：值语义，持有数据副本（不是不拥有数据的 string_view 意义上的"视图"）
struct CacheValue {
    std::vector<char> data_{};

    CacheValue() = default;
    CacheValue(const std::string& str) {
        data_.assign(str.begin(), str.end());
    }

    auto Len() const -> int64_t { return static_cast<int64_t>(data_.size()); }
    auto ToString() const -> std::string { return std::string(data_.begin(), data_.end()); }
};

using CacheValueOptional = std::optional<CacheValue>;

struct Entry {
    std::string key_;
    CacheValue value_;

    Entry(std::string k, const CacheValue& v) : key_(std::move(k)), value_(v) {}

    auto operator==(const Entry& entry) const -> bool {
        return key_ == entry.key_ && value_.data_ == entry.value_.data_;
    }
};

// 线程安全的 LRU 缓存。max_bytes 为 0 表示容量不限。
// 淘汰回调 evicted_func 在锁外执行，回调内再次访问本缓存是安全的。
class LRUCache {
    using EvictedFunc = std::function<void(std::string, CacheValue)>;
    using ListElementIter = std::list<Entry>::iterator;

public:
    LRUCache(int64_t max_bytes, const EvictedFunc& evicted_func = nullptr)
        : max_bytes_(max_bytes), evicted_func_(evicted_func) {}

    auto Get(const std::string& key) -> CacheValueOptional;
    void Set(const std::string& key, const CacheValue&);
    void Delete(const std::string& key);
    void RemoveOldest();

private:
    // 以下两个函数要求调用方已持有 mtx_
    auto SetLocked(const std::string& key, const CacheValue& value) -> void;
    auto PopOldestLocked() -> std::pair<std::string, CacheValue>;

    void NotifyEvicted(std::pair<std::string, CacheValue> evicted);

private:
    int64_t bytes_ = 0;
    int64_t max_bytes_;
    EvictedFunc evicted_func_;

    std::unordered_map<std::string, ListElementIter> cache_;
    std::list<Entry> list_;
    std::mutex mtx_;
};

}  // namespace kylecache

#endif /* LRU_H_ */
