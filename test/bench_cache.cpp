// 简易基准测试：LRU 缓存与一致性哈希的热路径性能（手动计时，无额外依赖）
//
// 构建：随项目正常构建，产物为 bin/bench_cache
// 运行：./bin/bench_cache

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "kylecache/cache.h"
#include "kylecache/consistent_hash.h"

using namespace kylecache;
using namespace std::chrono;

namespace {

template <typename F>
double RunForSeconds(double seconds, F&& f) {
    // 反复执行 f（f 返回本轮循环次数），返回每秒操作数
    int64_t total_ops = 0;
    auto start = steady_clock::now();
    auto deadline = start + duration_cast<steady_clock::duration>(duration<double>(seconds));
    while (steady_clock::now() < deadline) {
        total_ops += f();
    }
    double elapsed = duration<double>(steady_clock::now() - start).count();
    return static_cast<double>(total_ops) / elapsed;
}

void BenchLru() {
    constexpr int kKeys = 10000;
    constexpr int kValueSize = 128;
    LRUCache cache{64 << 20};  // 足够大，不触发淘汰
    const CacheValue value{std::string(kValueSize, 'x')};

    for (int i = 0; i < kKeys; ++i) {
        cache.Set("key_" + std::to_string(i), value);
    }

    double set_ops = RunForSeconds(1.0, [&] {
        for (int i = 0; i < 10000; ++i) {
            cache.Set("key_" + std::to_string(i % kKeys), value);
        }
        return 10000;
    });

    double get_hit_ops = RunForSeconds(1.0, [&] {
        int64_t hits = 0;
        for (int i = 0; i < 10000; ++i) {
            if (cache.Get("key_" + std::to_string(i % kKeys))) ++hits;
        }
        return 10000;
    });

    double get_miss_ops = RunForSeconds(1.0, [&] {
        for (int i = 0; i < 10000; ++i) {
            cache.Get("missing_" + std::to_string(i));
        }
        return 10000;
    });

    std::printf("[LRU]      value=%dB keys=%d  Set: %.0f ops/s | Get(hit): %.0f ops/s | Get(miss): %.0f ops/s\n",
                kValueSize, kKeys, set_ops, get_hit_ops, get_miss_ops);
}

void BenchConsistentHash() {
    std::vector<std::string> nodes;
    for (int i = 0; i < 10; ++i) {
        nodes.push_back("192.168.1." + std::to_string(i + 1) + ":8000");
    }

    ConsistentHashMap hash_map;
    hash_map.Add(nodes);

    double get_ops = RunForSeconds(1.0, [&] {
        for (int i = 0; i < 100000; ++i) {
            volatile auto node = hash_map.Get("key_" + std::to_string(i));
            (void)node;
        }
        return 100000;
    });

    std::printf("[HashRing] nodes=%d replicas=10  Get: %.0f ops/s\n", static_cast<int>(nodes.size()), get_ops);
}

}  // namespace

int main() {
    std::printf("running benchmarks (~6s)...\n");
    BenchLru();
    BenchConsistentHash();
    std::printf("done.\n");
    return 0;
}
