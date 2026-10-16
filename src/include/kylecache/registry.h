#ifndef REGISTRY_H_
#define REGISTRY_H_

#include <atomic>
#include <etcd/Client.hpp>
#include <memory>
#include <string>
#include <thread>

namespace kylecache {

// 基于 etcd 租约的服务注册：
// - 注册键格式 /services/{svc_name}/{addr}，绑定短租约，靠后台线程续约
// - 续约失败（网络分区、etcd 重启等）会自动用新租约重新注册，节点不会从服务发现中静默消失
// - Unregister 幂等且不抛异常（可能从信号处理/析构路径调用）
class EtcdRegistry {
public:
    explicit EtcdRegistry(const std::string& endpoints = "http://127.0.0.1:2379") {
        etcd_client_ = std::make_unique<etcd::Client>(endpoints);
    }

    ~EtcdRegistry();  // 保证续约线程被 join，不要求调用方显式 Unregister

    EtcdRegistry(const EtcdRegistry&) = delete;
    auto operator=(const EtcdRegistry&) -> EtcdRegistry& = delete;

    // 注册服务。addr 以 ':' 开头（如 ":8001"）时自动补上本机 IP，
    // 便于跨机器服务发现。
    bool Register(const std::string& svc_name, std::string addr);

    // 撤销租约并删除服务信息（etcd 删租约会连带删除键）。
    void Unregister();

    // 实际注册到 etcd 的地址（供节点标识使用），未注册时为空
    auto advertised_addr() const -> const std::string& { return advertised_addr_; }

private:
    auto GetLocalIP() -> std::string;

    // 创建租约并写入服务键（Register 与续约失败后的重新注册共用）
    bool DoRegister();

    void KeepAliveLoop();

    static constexpr int kLeaseTtlSec = 10;

    std::string svc_name_;
    std::string addr_;
    std::string key_;
    std::string advertised_addr_;

    int64_t lease_id_{0};
    std::unique_ptr<etcd::Client> etcd_client_;
    std::thread keepalive_thread_;
    std::atomic<bool> is_stop_{false};
};

}  // namespace kylecache

#endif
