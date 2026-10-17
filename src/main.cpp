#include <chrono>
#include <csignal>
#include <exception>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <unordered_map>

#include <fmt/base.h>
#include <fmt/core.h>
#include <gflags/gflags.h>
#include <gflags/gflags_declare.h>
#include <spdlog/spdlog.h>

#include "kylecache/cache.h"
#include "kylecache/group.h"
#include "kylecache/server.h"

using namespace kylecache;

DEFINE_int32(port, 8001, "节点端口");
DEFINE_string(node, "A", "节点标识符");
DEFINE_string(group, "default", "缓存组名称");
DEFINE_string(log_level, "info", "日志级别， 可选值：trace, debug, info, warn, error, critical");
DEFINE_string(etcd_endpoints, "http://127.0.0.1:2379", "etcd地址");

namespace {

spdlog::level::level_enum ParseLogLevel(const std::string& level) {
    if (level == "trace") return spdlog::level::trace;
    if (level == "debug") return spdlog::level::debug;
    if (level == "info") return spdlog::level::info;
    if (level == "warn" || level == "warning") return spdlog::level::warn;
    if (level == "error") return spdlog::level::err;
    if (level == "critical") return spdlog::level::critical;
    spdlog::warn("Unknown log level [{}], fallback to info", level);
    return spdlog::level::info;
}

}  // namespace

// 模拟数据库
std::unordered_map<std::string, std::string> db = {
    {"Tom", "400"},     {"Kevin", "370"}, {"Jack", "296"}, {"Alice", "320"}, {"Bob", "280"},
    {"Charlie", "410"}, {"Diana", "390"},  {"Eve", "310"},  {"abcde", "789"}, {"hello", "879"}};

std::function<void(int)> handler_wrapper;
void HandleCtrlC(int signum) { handler_wrapper(signum); }

int main(int argc, char** argv) {
    gflags::ParseCommandLineFlags(&argc, &argv, true);

    spdlog::set_level(ParseLogLevel(FLAGS_log_level));
    spdlog::set_pattern("[knode][%^%l%$] %v");

    // 先创建缓存组，再启动服务器：避免请求先于 group 到达而返回 Group not found
    MakeCacheGroup(FLAGS_group, 2 << 20, [&](const std::string& key) -> CacheValueOptional {
        if (db.find(key) != db.end()) {
            spdlog::info(">_< search [{}] from db\n", key);
            return CacheValue{db[key]};
        }
        spdlog::info(">_< Uh oh, there is not found [{}]\n", key);
        return std::nullopt;
    });

    // 绑定 0.0.0.0 接受任意来源连接；对外以 ":port" 注册，
    // EtcdRegistry 会自动补上默认路由网卡的 IP，保证跨机器可达
    const std::string bind_addr = "0.0.0.0:" + std::to_string(FLAGS_port);
    const std::string service_name = "kylecache";
    spdlog::info("[node{}] start at: {}", FLAGS_node, bind_addr);

    try {
        // 创建节点，同时注册到etcd
        ServerOptions opts;
        opts.etcd_endpoints = {FLAGS_etcd_endpoints};
        opts.advertise_addr = ":" + std::to_string(FLAGS_port);
        auto node = std::make_unique<KyleCacheServer>(bind_addr, service_name, opts);
        spdlog::info("[node{}] server created successfully", FLAGS_node);

        // 注册 Ctrl+C 信号处理器用来优雅关闭服务（在启动前注册，避免启动期间无法退出）
        handler_wrapper = [&](int signal) {
            if (signal == SIGINT) {
                spdlog::info("[node{}] received Ctrl+C signal, shutting down service...", FLAGS_node);
                if (node) {
                    node->Stop();
                }
                spdlog::info("[node{}] service stopped", FLAGS_node);
                // 不直接 exit，而是要等其他工作线程完成清理工作
            }
        };
        signal(SIGINT, HandleCtrlC);

        // 启动节点
        std::thread server_thread{[&] {
            spdlog::info("[node{}] starting service...", FLAGS_node);
            try {
                node->Start();
            } catch (const std::exception& e) {
                spdlog::info("[node{}] failed to start service: {}", FLAGS_node, e.what());
                std::exit(1);
            }
        }};

        spdlog::info("[node{}] service running, press Ctrl+C to exit...", FLAGS_node);

        // 等待服务器线程
        if (server_thread.joinable()) {
            server_thread.join();
        }

    } catch (const std::exception& e) {
        spdlog::error("[node{}] exception occurred: {}", FLAGS_node, e.what());
        std::exit(1);
    }

    return 0;
}
