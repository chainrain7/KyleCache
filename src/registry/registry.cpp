#include "kylecache/registry.h"

#include <arpa/inet.h>
#include <ifaddrs.h>
#include <netinet/in.h>
#include <unistd.h>

#include <chrono>
#include <fstream>
#include <string>

#include <fmt/core.h>
#include <spdlog/spdlog.h>
#include <etcd/KeepAlive.hpp>

namespace kylecache {

EtcdRegistry::~EtcdRegistry() {
    is_stop_ = true;
    if (keepalive_thread_.joinable()) {
        keepalive_thread_.join();
    }
    // 不在这里撤销租约：租约到期后 etcd 会自动删除服务键。
    // 需要立即下线的场景应由调用方显式 Unregister()。
}

bool EtcdRegistry::Register(const std::string& svc_name, std::string addr) {
    svc_name_ = svc_name;
    addr_ = std::move(addr);
    if (!DoRegister()) {
        return false;
    }
    keepalive_thread_ = std::thread{[this] { this->KeepAliveLoop(); }};
    return true;
}

bool EtcdRegistry::DoRegister() {
    // 地址以 ':' 开头（如 ":8001"）时补上本机 IP；拿不到本机 IP 则退回环回地址
    std::string advertise = addr_;
    if (!advertise.empty() && advertise[0] == ':') {
        std::string local_ip = GetLocalIP();
        if (local_ip.empty()) {
            local_ip = "127.0.0.1";
        }
        advertise = local_ip + advertise;
    }
    if (advertise.empty()) {
        spdlog::error("Failed to build advertise address for service [{}]", svc_name_);
        return false;
    }
    key_ = "/services/" + svc_name_ + "/" + advertise;
    advertised_addr_ = advertise;

    // 创建租约
    auto lease_resp = etcd_client_->leasegrant(kLeaseTtlSec).get();
    if (!lease_resp.is_ok()) {
        spdlog::error("Failed to create lease: {}", lease_resp.error_message());
        return false;
    }
    lease_id_ = lease_resp.value().lease();

    // 注册服务
    auto put_resp = etcd_client_->put(key_, advertise, lease_id_).get();
    if (!put_resp.is_ok()) {
        spdlog::error("Failed to register [{}] to etcd: {}", key_, put_resp.error_message());
        lease_id_ = 0;
        return false;
    }

    spdlog::info("Etcd service registered: {} (lease {})", key_, lease_id_);
    return true;
}

void EtcdRegistry::Unregister() {
    is_stop_ = true;
    if (keepalive_thread_.joinable()) {
        keepalive_thread_.join();
    }
    if (lease_id_ != 0) {
        // 注意：这里必须检查 etcd 响应的状态，且不能抛异常——
        // 本函数会从信号处理与析构路径被调用
        auto resp = etcd_client_->leaserevoke(lease_id_).get();
        if (!resp.is_ok()) {
            spdlog::error("Failed to revoke lease {}: {}", lease_id_, resp.error_message());
        } else {
            spdlog::debug("Lease {} revoked successfully", lease_id_);
        }
        lease_id_ = 0;
    }
    spdlog::info("Service unregistered: {}", key_);
}

auto EtcdRegistry::GetLocalIP() -> std::string {
    // 多网卡/容器环境下"第一个非回环地址"常是虚拟网卡（如 WSL 的 NAT 网关），
    // 对外不可达。优先取默认路由所在网卡的 IP。
    std::string default_iface;
    {
        std::ifstream route("/proc/net/route");
        std::string header;
        std::getline(route, header);  // 表头
        std::string iface;
        uint32_t dest, gateway, flags, refcnt, use, metric, mask;
        while (route >> iface >> std::hex >> dest >> gateway >> flags >> refcnt >> use >> metric >> mask) {
            if (dest == 0) {  // 默认路由
                default_iface = iface;
                break;
            }
        }
    }

    struct ifaddrs* ifaddr;
    if (getifaddrs(&ifaddr) == -1) {
        return "";
    }
    std::string ip;
    std::string fallback;
    for (struct ifaddrs* ifa = ifaddr; ifa; ifa = ifa->ifa_next) {
        if (!ifa->ifa_addr || ifa->ifa_addr->sa_family != AF_INET) continue;
        char buf[INET_ADDRSTRLEN];
        void* addr_ptr = &((struct sockaddr_in*)ifa->ifa_addr)->sin_addr;
        inet_ntop(AF_INET, addr_ptr, buf, INET_ADDRSTRLEN);
        std::string candidate{buf};
        if (candidate == "127.0.0.1") continue;
        if (!default_iface.empty() && default_iface == ifa->ifa_name) {
            ip = candidate;
            break;
        }
        if (fallback.empty()) {
            fallback = candidate;
        }
    }
    freeifaddrs(ifaddr);
    return ip.empty() ? fallback : ip;
}

void EtcdRegistry::KeepAliveLoop() {
    while (!is_stop_) {
        try {
            // KeepAlive 对象存活期间自动续约；Check() 在续约失败时抛出异常
            etcd::KeepAlive keepalive{*etcd_client_, kLeaseTtlSec, lease_id_};
            while (!is_stop_) {
                keepalive.Check();
                std::this_thread::sleep_for(std::chrono::seconds(1));
            }
        } catch (const std::exception& e) {
            spdlog::error("KeepAlive failed for lease {}: {}", lease_id_, e.what());
            if (!is_stop_) {
                // 原租约已不可用，服务键即将（或已经）从 etcd 消失，
                // 必须用新租约重新注册，否则节点会静默掉线
                std::this_thread::sleep_for(std::chrono::seconds(1));
                if (!is_stop_ && DoRegister()) {
                    spdlog::info("Re-registered with new lease {}", lease_id_);
                }
            }
        }
    }
    spdlog::debug("KeepAlive loop exited for lease {}", lease_id_);
}

}  // namespace kylecache
