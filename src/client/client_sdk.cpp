#include "kylecache/client.h"

#include <algorithm>
#include <chrono>
#include <future>
#include <utility>
#include <vector>

#include <grpcpp/grpcpp.h>
#include <spdlog/spdlog.h>

#include "kylecache.grpc.pb.h"

namespace kylecache {

namespace {

// 给 RPC 上下文设置超时
void SetupContext(grpc::ClientContext& ctx, std::chrono::milliseconds timeout) {
    ctx.set_deadline(std::chrono::system_clock::now() + timeout);
}

// 对一组节点并行执行 op（每个节点一个异步任务），全部成功返回 true
template <typename Op>
auto ForEachNodeParallel(const std::vector<std::string>& nodes, Op&& op) -> bool {
    if (nodes.empty()) {
        return true;
    }
    std::vector<std::future<bool>> futs;
    futs.reserve(nodes.size());
    for (const auto& addr : nodes) {
        futs.push_back(std::async(std::launch::async, [addr, &op] { return op(addr); }));
    }
    bool all_ok = true;
    for (auto& f : futs) {
        try {
            if (!f.get()) {
                all_ok = false;
            }
        } catch (const std::exception& e) {
            spdlog::error("Node operation threw an exception: {}", e.what());
            all_ok = false;
        }
    }
    return all_ok;
}

}  // namespace

KyleCacheClient::KyleCacheClient(const std::string& etcd_endpoints, const std::string& service_name,
                                 ClientOptions options)
    : service_name_(service_name), options_(options) {
    etcd_client_ = std::make_shared<etcd::Client>(etcd_endpoints);
    StartServiceDiscovery();
}

KyleCacheClient::~KyleCacheClient() {
    is_stop_ = true;
    {
        std::lock_guard<std::mutex> lock(watcher_mutex_);
        if (etcd_watcher_) {
            etcd_watcher_->Cancel();
        }
    }
    if (discovery_thread_.joinable()) {
        discovery_thread_.join();
    }
}

auto KyleCacheClient::GetChannel(const std::string& addr) -> std::shared_ptr<grpc::Channel> {
    {
        std::lock_guard<std::mutex> lock(pool_mutex_);
        auto it = channel_pool_.find(addr);
        if (it != channel_pool_.end()) {
            return it->second;
        }
    }
    auto channel = grpc::CreateChannel(addr, grpc::InsecureChannelCredentials());
    std::lock_guard<std::mutex> lock(pool_mutex_);
    // emplace 幂等：并发创建时保留先入池的 channel
    auto [it, inserted] = channel_pool_.emplace(addr, std::move(channel));
    return it->second;
}

auto KyleCacheClient::Get(const std::string& group, const std::string& key) -> std::optional<std::string> {
    auto candidates = GetCandidateNodes(key);
    if (candidates.empty()) {
        spdlog::warn("No cache service available for key: {}", key);
        return std::nullopt;
    }

    pb::Request request;
    request.set_group(group);
    request.set_key(key);

    for (const auto& addr : candidates) {
        auto client = pb::KyleCache::NewStub(GetChannel(addr));

        pb::GetResponse response;
        grpc::ClientContext context;
        SetupContext(context, options_.rpc_timeout);

        auto status = client->Get(&context, request, &response);
        if (status.ok()) {
            return response.value();
        }
        if (status.error_code() == grpc::StatusCode::NOT_FOUND) {
            // 节点正常应答"不存在"，说明 key 确实不存在，无需故障转移
            return std::nullopt;
        }
        // UNAVAILABLE / DEADLINE_EXCEEDED 等：该节点当前不可用，尝试下一个候选节点
        spdlog::warn("Get failed on node {}: {} ({})", addr, status.error_message(),
                     static_cast<int>(status.error_code()));
    }
    spdlog::error("Get exhausted {} candidate nodes for key: {}", candidates.size(), key);
    return std::nullopt;
}

bool KyleCacheClient::Set(const std::string& group, const std::string& key, const std::string& value) {
    auto candidates = GetCandidateNodes(key);
    if (candidates.empty()) {
        spdlog::warn("No cache service available for Set");
        return false;
    }

    pb::Request request;
    request.set_group(group);
    request.set_key(key);
    request.set_value(value);

    // 写入目标节点；失败则沿候选节点故障转移
    std::string primary;
    for (const auto& addr : candidates) {
        auto client = pb::KyleCache::NewStub(GetChannel(addr));

        pb::SetResponse response;
        grpc::ClientContext context;
        SetupContext(context, options_.rpc_timeout);

        auto status = client->Set(&context, request, &response);
        if (status.ok() && response.value()) {
            primary = addr;
            break;
        }
        spdlog::warn("Set failed on node {}: {}", addr, status.error_message());
    }
    if (primary.empty()) {
        spdlog::error("Set failed on all {} candidate nodes for key: {}", candidates.size(), key);
        return false;
    }

    // 快照其余节点后放锁，并行广播失效（单节点失败重试），不阻塞路由
    std::vector<std::string> others;
    {
        std::lock_guard<std::mutex> lock(nodes_mutex_);
        for (const auto& addr : cache_nodes_) {
            if (addr != primary) {
                others.push_back(addr);
            }
        }
    }

    return ForEachNodeParallel(others, [&](const std::string& addr) {
        auto client = pb::KyleCache::NewStub(GetChannel(addr));
        for (int attempt = 1; attempt <= options_.broadcast_retries; ++attempt) {
            pb::InvalidateResponse response;
            grpc::ClientContext ctx;
            SetupContext(ctx, options_.rpc_timeout);
            auto status = client->Invalidate(&ctx, request, &response);
            if (status.ok() && response.value()) {
                return true;
            }
            spdlog::warn("Invalidate failed on node {} (attempt {}/{}): {}", addr, attempt,
                         options_.broadcast_retries, status.error_message());
        }
        return false;
    });
}

bool KyleCacheClient::Delete(const std::string& group, const std::string& key) {
    pb::Request request;
    request.set_group(group);
    request.set_key(key);

    // 快照全部节点后放锁，并行广播删除（单节点失败重试）
    std::vector<std::string> nodes;
    {
        std::lock_guard<std::mutex> lock(nodes_mutex_);
        if (cache_nodes_.empty()) {
            spdlog::warn("No cache service available for Delete");
            return false;
        }
        nodes.assign(cache_nodes_.begin(), cache_nodes_.end());
    }

    return ForEachNodeParallel(nodes, [&](const std::string& addr) {
        auto client = pb::KyleCache::NewStub(GetChannel(addr));
        for (int attempt = 1; attempt <= options_.broadcast_retries; ++attempt) {
            pb::DeleteResponse response;
            grpc::ClientContext ctx;
            SetupContext(ctx, options_.rpc_timeout);
            auto status = client->Delete(&ctx, request, &response);
            if (status.ok() && response.value()) {
                return true;
            }
            spdlog::warn("Delete failed on node {} (attempt {}/{}): {}", addr, attempt,
                         options_.broadcast_retries, status.error_message());
        }
        return false;
    });
}

auto KyleCacheClient::Stats(const std::string& group) -> std::vector<NodeStats> {
    std::vector<std::string> nodes;
    {
        std::lock_guard<std::mutex> lock(nodes_mutex_);
        nodes.assign(cache_nodes_.begin(), cache_nodes_.end());
    }
    if (nodes.empty()) {
        return {};
    }

    std::vector<NodeStats> result;
    std::mutex result_mtx;

    pb::StatsRequest request;
    request.set_group(group);

    ForEachNodeParallel(nodes, [&](const std::string& addr) {
        auto client = pb::KyleCache::NewStub(GetChannel(addr));

        pb::StatsResponse response;
        grpc::ClientContext ctx;
        SetupContext(ctx, options_.rpc_timeout);

        auto status = client->Stats(&ctx, request, &response);
        if (!status.ok()) {
            spdlog::warn("Stats failed on node {}: {}", addr, status.error_message());
            return false;
        }
        NodeStats s;
        s.node = response.node().empty() ? addr : response.node();
        s.group = response.group();
        s.loads = response.loads();
        s.local_hits = response.local_hits();
        s.local_misses = response.local_misses();
        s.loader_hits = response.loader_hits();
        s.loader_errors = response.loader_errors();
        s.load_duration_ns = response.load_duration_ns();
        {
            std::lock_guard<std::mutex> lock(result_mtx);
            result.push_back(std::move(s));
        }
        return true;
    });

    // 结果顺序不稳定，按节点名排序便于展示
    std::sort(result.begin(), result.end(), [](const NodeStats& a, const NodeStats& b) { return a.node < b.node; });
    return result;
}

bool KyleCacheClient::StartServiceDiscovery() {
    // 首次全量拉取失败不阻止构造：后台线程会持续重试
    bool ok = FetchAllServices();
    if (!ok) {
        spdlog::warn("Initial service discovery failed, will keep retrying in background");
    }
    discovery_thread_ = std::thread{[this] { DiscoveryLoop(); }};
    return ok;
}

void KyleCacheClient::DiscoveryLoop() {
    const std::string prefix = "/services/" + service_name_ + "/";
    int backoff_sec = 1;

    while (!is_stop_.load()) {
        try {
            // (重)建立 watcher 前先全量同步一次，避免断线期间漏掉事件
            FetchAllServices();
            {
                std::lock_guard<std::mutex> lock(watcher_mutex_);
                if (is_stop_.load()) {
                    break;
                }
                etcd_watcher_ = std::make_unique<etcd::Watcher>(
                    *etcd_client_, prefix, [this](etcd::Response resp) { HandleWatchEvents(resp); }, true);
            }
            backoff_sec = 1;
            etcd_watcher_->Wait();

            {
                std::lock_guard<std::mutex> lock(watcher_mutex_);
                etcd_watcher_.reset();
            }
        } catch (const std::exception& e) {
            spdlog::error("Service discovery error: {}", e.what());
            std::lock_guard<std::mutex> lock(watcher_mutex_);
            etcd_watcher_.reset();
        }
        if (is_stop_.load()) {
            break;
        }
        spdlog::warn("etcd watcher exited, reconnecting in {}s...", backoff_sec);
        std::this_thread::sleep_for(std::chrono::seconds(backoff_sec));
        backoff_sec = std::min(backoff_sec * 2, 10);
    }
}

void KyleCacheClient::HandleWatchEvents(const etcd::Response& resp) {
    std::lock_guard<std::mutex> lock{nodes_mutex_};
    if (!resp.is_ok()) {
        spdlog::error("Failed to watch etcd: {}", resp.error_message());
        return;
    }

    for (const auto& event : resp.events()) {
        std::string key = event.kv().key();
        std::string addr = ParseAddrFromKey(key);
        if (addr.empty()) {
            continue;
        }
        switch (event.event_type()) {
            case etcd::Event::EventType::PUT: {
                // insert().second 为 true 才是新增，避免重复 Add 虚拟节点
                if (cache_nodes_.insert(addr).second) {
                    consistent_hash_.Add({addr});
                    spdlog::debug("Service added: {} (key: {})", addr, key);
                }
                break;
            }
            case etcd::Event::EventType::DELETE_: {
                if (cache_nodes_.erase(addr) > 0) {
                    consistent_hash_.Remove(addr);
                    spdlog::debug("Service removed: {} (key: {})", addr, key);
                }
                // 节点下线，释放其缓存 channel
                {
                    std::lock_guard<std::mutex> pool_lock(pool_mutex_);
                    channel_pool_.erase(addr);
                }
                break;
            }
            default:
                spdlog::debug("Unknown event type: {} for key: {}", static_cast<int>(event.event_type()), key);
                break;
        }
    }
}

bool KyleCacheClient::FetchAllServices() {
    std::string prefix_key = "/services/" + service_name_ + "/";
    etcd::Response resp = etcd_client_->ls(prefix_key).get();

    if (!resp.is_ok()) {
        spdlog::error("Failed to get all services from etcd now: {}", resp.error_message());
        return false;
    }
    std::lock_guard<std::mutex> lock(nodes_mutex_);
    for (const auto& key : resp.keys()) {
        std::string addr = ParseAddrFromKey(key);
        if (!addr.empty() && cache_nodes_.insert(addr).second) {
            consistent_hash_.Add({addr});
            spdlog::debug("Discovered service at {}", addr);
        }
    }
    return true;
}

auto KyleCacheClient::ParseAddrFromKey(const std::string& key) -> std::string {
    std::string prefix = "/services/" + service_name_ + "/";
    if (key.rfind(prefix, 0) == 0) {
        return key.substr(prefix.length());
    }
    return "";
}

auto KyleCacheClient::GetCandidateNodes(const std::string& key) -> std::vector<std::string> {
    std::lock_guard<std::mutex> lock(nodes_mutex_);
    if (cache_nodes_.empty()) {
        return {};
    }
    auto nodes = consistent_hash_.GetN(key, std::max(1, options_.max_candidates));
    if (nodes.empty()) {
        // 哈希环异常时兜底：任取一个存活节点
        nodes.assign(1, *cache_nodes_.begin());
    }
    return nodes;
}

}  // namespace kylecache
