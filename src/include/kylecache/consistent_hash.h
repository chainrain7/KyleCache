#ifndef CONSISTENT_HASH_H_
#define CONSISTENT_HASH_H_

#include <atomic>
#include <cstdint>
#include <functional>
#include <shared_mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include <fmt/core.h>

namespace kylecache {

// CRC32 IEEE 哈希函数，兼容 Go 的 crc32.ChecksumIEEE
uint32_t Crc32IEEE(const std::string& data);

// 一致性哈希配置
struct HashConfig {
    // 每个真实节点对应的虚拟节点数
    int replicas;
    // 最小虚拟节点数
    int min_replicas;
    // 最大虚拟节点数
    int max_replicas;
    // 哈希函数
    std::function<uint32_t(const std::string&)> hash_func;
    // 负载均衡阈值，超过此值触发虚拟节点调整
    double load_balance_threshold;
    // 是否启用负载自均衡。注意：动态调整虚拟节点会改变 key -> 节点的映射，
    // 对缓存这类依赖路由稳定性的场景通常应保持关闭（各客户端独立调整
    // 还会造成同一 key 被不同客户端路由到不同节点）。
    bool enable_load_balance;
};

// DefaultConfig 默认配置
const HashConfig kDefaultConfig = {
    10,   10, 200, Crc32IEEE,
    0.25,  // 25% 的负载不均衡度触发调整
    false  // 负载自均衡默认关闭
};

// Map 一致性哈希实现
class ConsistentHashMap {
public:
    // New 创建一致性哈希实例
    explicit ConsistentHashMap(HashConfig cfg = kDefaultConfig);

    // 析构函数，确保负载均衡器线程正确停止
    ~ConsistentHashMap();

    ConsistentHashMap(const ConsistentHashMap&) = delete;
    auto operator=(const ConsistentHashMap&) -> ConsistentHashMap& = delete;

    // Add 添加节点
    // 返回 true 表示成功，false 表示失败
    bool Add(const std::vector<std::string>& nodes);

    // Remove 移除节点
    // 返回 true 表示成功，false 表示失败
    bool Remove(const std::string& node);

    // Get 获取 key 所属的节点；环为空或 key 为空时返回 ""
    auto Get(const std::string& key) -> std::string;

    // GetN 沿哈希环顺时针返回 key 的前 n 个不同候选节点（用于故障转移），
    // 环为空或 key 为空时返回空 vector
    auto GetN(const std::string& key, int n) -> std::vector<std::string>;

    // GetStats 获取负载统计信息
    auto GetStats() -> std::unordered_map<std::string, double>;

private:
    // addNode 添加节点的虚拟节点
    void AddNode(const std::string& node, int replicas);

    // checkAndRebalance 检查并重新平衡虚拟节点
    void CheckAndRebalance();

    // rebalanceNodes 重新平衡节点
    void RebalanceNodes();

    // startBalancer 启动负载均衡器线程
    void StartBalancer();

private:
    mutable std::shared_mutex mtx_;  // 读写互斥量，类似于 Go 的 sync.RWMutex
    // 配置信息
    HashConfig config_;

    // 哈希环
    std::vector<uint32_t> keys_;
    // 哈希环到节点的映射
    std::unordered_map<uint32_t, std::string> hash_map_;
    // 节点到虚拟节点数量的映射
    std::unordered_map<std::string, int> node_replicas_;
    // 节点负载统计
    // 使用 std::atomic<long long> 保证对 nodeCounts 中每个节点计数的原子操作
    std::unordered_map<std::string, std::atomic<long long>> node_counts_;
    // 总请求数
    std::atomic<long long> total_requests_;

    std::thread balancer_thread_;         // 负载均衡器线程
    std::atomic<bool> is_balancer_stop_;  // 控制负载均衡器线程停止的标志
};

}  // namespace kylecache

#endif /* CONSISTENT_HASH_H_ */
