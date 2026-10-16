#include "kylecache/cache.h"

#include <mutex>
#include <optional>
#include <utility>
#include <vector>

namespace kylecache {

auto LRUCache::Get(const std::string& key) -> CacheValueOptional {
    std::lock_guard lock{mtx_};
    auto it = cache_.find(key);
    if (it == cache_.end()) {
        return std::nullopt;
    }
    // splice 只移动链表节点本身，不拷贝键值
    list_.splice(list_.begin(), list_, it->second);
    return it->second->value_;
}

void LRUCache::Set(const std::string& key, const CacheValue& value) {
    std::vector<std::pair<std::string, CacheValue>> evicted;
    {
        std::lock_guard lock{mtx_};
        SetLocked(key, value);
        // 容量超限时，从最久未使用的一端逐条淘汰
        while (max_bytes_ != 0 && bytes_ > max_bytes_ && !list_.empty()) {
            evicted.push_back(PopOldestLocked());
        }
    }
    for (auto& e : evicted) {
        NotifyEvicted(std::move(e));
    }
}

void LRUCache::Delete(const std::string& key) {
    std::optional<std::pair<std::string, CacheValue>> evicted;
    {
        std::lock_guard lock{mtx_};
        auto it = cache_.find(key);
        if (it == cache_.end()) {
            return;
        }
        bytes_ -= static_cast<int64_t>(key.size()) + it->second->value_.Len();
        evicted = std::make_pair(key, std::move(it->second->value_));
        list_.erase(it->second);
        cache_.erase(it);
    }
    if (evicted) {
        NotifyEvicted(std::move(*evicted));
    }
}

void LRUCache::RemoveOldest() {
    std::optional<std::pair<std::string, CacheValue>> evicted;
    {
        std::lock_guard lock{mtx_};
        if (!list_.empty()) {
            evicted = PopOldestLocked();
        }
    }
    if (evicted) {
        NotifyEvicted(std::move(*evicted));
    }
}

void LRUCache::SetLocked(const std::string& key, const CacheValue& value) {
    auto it = cache_.find(key);
    if (it != cache_.end()) {
        bytes_ += value.Len() - it->second->value_.Len();
        list_.erase(it->second);
    } else {
        bytes_ += static_cast<int64_t>(key.size()) + value.Len();
    }
    list_.emplace_front(key, value);
    cache_[key] = list_.begin();
}

auto LRUCache::PopOldestLocked() -> std::pair<std::string, CacheValue> {
    auto& back = list_.back();
    bytes_ -= static_cast<int64_t>(back.key_.size()) + back.value_.Len();
    // 先拷贝 key 再移出 value：cache_ 的擦除需要完整的 key
    std::string key = back.key_;
    CacheValue value = std::move(back.value_);
    cache_.erase(key);
    list_.pop_back();
    return {std::move(key), std::move(value)};
}

void LRUCache::NotifyEvicted(std::pair<std::string, CacheValue> evicted) {
    if (evicted_func_) {
        evicted_func_(std::move(evicted.first), std::move(evicted.second));
    }
}

}  // namespace kylecache
