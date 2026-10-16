// 线程安全的并发去重机制：对同一个键（key）的多次并发请求只执行一次实际操作，
// 其他请求等待并复用第一次的结果。

#ifndef SINGLEFLIGHT_H_
#define SINGLEFLIGHT_H_

#include <exception>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>

#include "kylecache/cache.h"

namespace kylecache {

class SingleFlight {
    using Result = std::optional<CacheValue>;
    using Func = std::function<Result()>;

public:
    // 执行 func 并将结果共享给同一 key 的所有并发调用者。
    // - func 正常返回：所有调用者拿到同一份结果
    // - func 抛出异常：异常会传播给所有等待者，且 map 中不会留下残留条目，
    //   后续对该 key 的调用会重新执行 func
    Result Do(const std::string& key, Func func) {
        std::unique_lock<std::mutex> glock(mtx_);

        // 已有进行中的调用：等待其结果
        if (auto it = map_.find(key); it != map_.end()) {
            auto existing_call = it->second;
            glock.unlock();  // 释放锁，避免阻塞其他 key 的处理
            return existing_call->fut.get();
        }

        // 成为该 key 的"领导者"，负责执行 func
        auto new_call = std::make_shared<Call>();
        map_[key] = new_call;
        glock.unlock();

        try {
            Result val = func();
            new_call->prom.set_value(val);
            Finish(key);
            return val;
        } catch (...) {
            // 把异常传递给等待者，并保证条目被清理，否则该 key 将永久卡死
            new_call->prom.set_exception(std::current_exception());
            Finish(key);
            throw;
        }
    }

private:
    void Finish(const std::string& key) {
        std::lock_guard<std::mutex> lock(mtx_);
        map_.erase(key);
    }

    struct Call {
        std::promise<Result> prom;
        std::shared_future<Result> fut = prom.get_future().share();
    };

    std::mutex mtx_;
    std::unordered_map<std::string, std::shared_ptr<Call>> map_;
};

}  // namespace kylecache

#endif /* SINGLEFLIGHT_H_ */
