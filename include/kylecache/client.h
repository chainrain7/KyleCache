#ifndef KYLECACHE_CLIENT_H_
#define KYLECACHE_CLIENT_H_

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <etcd/Client.hpp>
#include <etcd/Watcher.hpp>

#include "kylecache/consistent_hash.h"

namespace grpc {
class Channel;
}

namespace kylecache {

// 客户端行为配置
struct ClientOptions {
    // 单次 RPC 超时
    std::chrono::milliseconds rpc_timeout{std::chrono::milliseconds(500)};
    // Get 失效时的最大候选节点数（目标节点 + 备选，沿哈希环取）
    int max_candidates = 3;
    // 广播失效 / 删除时，单个节点的重试次数
    int broadcast_retries = 2;
};

// 单个节点的统计信息（通过 Stats() 拉取）
struct NodeStats {
    std::string node;
    std::string group;
    int64_t loads = 0;
    int64_t local_hits = 0;
    int64_t local_misses = 0;
    int64_t loader_hits = 0;
    int64_t loader_errors = 0;
    int64_t load_duration_ns = 0;
};

// 分布式缓存客户端 SDK：
// - 基于 etcd watch 的服务发现（断线自动重连并全量同步）
// - 一致性哈希路由，节点失效时沿哈希环故障转移
// - gRPC channel 按节点复用（连接池）
// - 所有 RPC 带超时
class KyleCacheClient {
public:
    KyleCacheClient(const std::string& etcd_endpoints, const std::string& service_name = "kylecache",
                    ClientOptions options = ClientOptions{});
    ~KyleCacheClient();

    // 禁止拷贝和赋值
    KyleCacheClient(const KyleCacheClient&) = delete;
    KyleCacheClient& operator=(const KyleCacheClient&) = delete;

    // 获取缓存；节点故障时自动尝试哈希环上的下一个候选节点
    auto Get(const std::string& group, const std::string& key) -> std::optional<std::string>;

    // 设置缓存：写入目标节点后，向其余节点并行广播失效（单节点失败会重试）。
    // 注意：这是"写失效"而非强一致——广播完成前其他节点可能短暂返回旧值。
    bool Set(const std::string& group, const std::string& key, const std::string& value);

    // 删除缓存：向所有节点并行广播删除（单节点失败会重试）
    bool Delete(const std::string& group, const std::string& key);

    // 拉取所有节点上该 group 的本地统计
    auto Stats(const std::string& group) -> std::vector<NodeStats>;

private:
    // 服务发现相关
    bool StartServiceDiscovery();
    void DiscoveryLoop();
    void HandleWatchEvents(const etcd::Response& resp);
    bool FetchAllServices();
    auto ParseAddrFromKey(const std::string& key) -> std::string;

    // 哈希环上 key 的候选节点（目标节点在前，用于故障转移）
    auto GetCandidateNodes(const std::string& key) -> std::vector<std::string>;

    // 获取（并缓存复用）到指定节点的 gRPC channel
    auto GetChannel(const std::string& addr) -> std::shared_ptr<grpc::Channel>;

private:
    std::string service_name_;
    ClientOptions options_;
    std::shared_ptr<etcd::Client> etcd_client_;

    std::unordered_set<std::string> cache_nodes_;
    mutable std::mutex nodes_mutex_;

    std::thread discovery_thread_;
    std::mutex watcher_mutex_;                      // 保护 etcd_watcher_ 指针本身
    std::unique_ptr<etcd::Watcher> etcd_watcher_;   // 由 discovery 线程创建/销毁
    std::atomic<bool> is_stop_{false};

    std::mutex pool_mutex_;                                        // 保护 channel_pool_
    std::unordered_map<std::string, std::shared_ptr<grpc::Channel>> channel_pool_;

    ConsistentHashMap consistent_hash_;
};

}  // namespace kylecache

#endif  // KYLECACHE_CLIENT_H_
