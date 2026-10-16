#include "kylecache/server.h"

#include <fstream>
#include <iterator>
#include <memory>
#include <optional>

#include <fmt/base.h>
#include <grpcpp/health_check_service_interface.h>
#include <grpcpp/security/server_credentials.h>
#include <grpcpp/server_builder.h>
#include <spdlog/spdlog.h>

#include "kylecache.pb.h"
#include "kylecache/group.h"

namespace kylecache {

namespace {

// 读取 PEM 文件内容；失败返回空
auto ReadFile(const std::string& path) -> std::optional<std::string> {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        return std::nullopt;
    }
    return std::string{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}

}  // namespace

KyleCacheServer::KyleCacheServer(const std::string& addr, const std::string& svc_name, ServerOptions opts)
    : addr_(addr), svc_name_(svc_name), opts_(opts) {
    // etcd 客户端支持逗号分隔的多地址，全部端点都传给注册中心
    std::string endpoints;
    for (size_t i = 0; i < opts_.etcd_endpoints.size(); ++i) {
        if (i > 0) endpoints += ",";
        endpoints += opts_.etcd_endpoints[i];
    }
    // 创建etcd注册器
    etcd_register_ = std::make_unique<EtcdRegistry>(endpoints);
    if (!etcd_register_->Register(svc_name_, addr_)) {
        throw std::runtime_error("[kylecache] Failed to register service with etcd");
    }
    node_id_ = etcd_register_->advertised_addr();
}

auto KyleCacheServer::Get(grpc::ServerContext* context, const pb::Request* request, pb::GetResponse* response)
    -> grpc::Status {
    auto group = GetCacheGroup(request->group());
    if (!group) {
        return grpc::Status(grpc::StatusCode::NOT_FOUND, "Group not found");
    }
    auto value = group->Get(request->key());
    if (!value) {
        return grpc::Status(grpc::StatusCode::NOT_FOUND, "Key not found");
    }
    response->set_value(value->ToString());
    return grpc::Status::OK;
}

auto KyleCacheServer::Set(grpc::ServerContext* context, const pb::Request* request, pb::SetResponse* response)
    -> grpc::Status {
    auto group = GetCacheGroup(request->group());
    if (!group) {
        return grpc::Status(grpc::StatusCode::NOT_FOUND, "Group not found");
    }
    bool is_set = group->Set(request->key(), request->value());
    response->set_value(is_set);
    return grpc::Status::OK;
}

auto KyleCacheServer::Delete(grpc::ServerContext* context, const pb::Request* request,
                             pb::DeleteResponse* response) -> grpc::Status {
    auto group = GetCacheGroup(request->group());
    if (!group) {
        return grpc::Status(grpc::StatusCode::NOT_FOUND, "Group not found");
    }
    bool is_delete = group->Delete(request->key());
    response->set_value(is_delete);
    return grpc::Status::OK;
}

auto KyleCacheServer::Invalidate(grpc::ServerContext* context, const pb::Request* request,
                                 pb::InvalidateResponse* response) -> grpc::Status {
    auto group = GetCacheGroup(request->group());
    if (!group) {
        return grpc::Status(grpc::StatusCode::NOT_FOUND, "Group not found");
    }
    // Invalidate RPC 调用总是来自其他节点，调用 InvalidateFromPeer 避免循环传播
    bool is_invalidate = group->InvalidateFromPeer(request->key());
    response->set_value(is_invalidate);
    return grpc::Status::OK;
}

auto KyleCacheServer::Stats(grpc::ServerContext* context, const pb::StatsRequest* request,
                            pb::StatsResponse* response) -> grpc::Status {
    auto group = GetCacheGroup(request->group());
    if (!group) {
        return grpc::Status(grpc::StatusCode::NOT_FOUND, "Group not found");
    }
    auto stats = group->Stats();
    response->set_group(request->group());
    response->set_node(node_id_);
    response->set_loads(stats.loads);
    response->set_local_hits(stats.local_hits);
    response->set_local_misses(stats.local_misses);
    response->set_loader_hits(stats.loader_hits);
    response->set_loader_errors(stats.loader_errors);
    response->set_load_duration_ns(stats.load_duration_ns);
    return grpc::Status::OK;
}

// 启动 gRPC 服务器
void KyleCacheServer::Start() {
    try {
        // 配置gRPC服务器选项
        grpc::ServerBuilder builder;

        // 设置最大消息大小
        builder.SetMaxReceiveMessageSize(opts_.max_msg_size);
        builder.SetMaxSendMessageSize(opts_.max_msg_size);

        // 配置TLS或非安全连接
        if (opts_.tls) {
            auto creds = LoadTLSCredentials(opts_.cert_file, opts_.key_file);
            if (!creds) {
                throw std::runtime_error("Failed to load TLS credentials");
            }
            builder.AddListeningPort(addr_, creds);
        } else {
            builder.AddListeningPort(addr_, grpc::InsecureServerCredentials());
        }

        // 启用默认健康检查服务
        grpc::EnableDefaultHealthCheckService(true);
        builder.SetOption(grpc::MakeChannelArgumentOption(GRPC_ARG_KEEPALIVE_TIME_MS, 30000));
        builder.SetOption(grpc::MakeChannelArgumentOption(GRPC_ARG_KEEPALIVE_TIMEOUT_MS, 5000));

        // 注册服务
        builder.RegisterService(this);

        // 构建并启动服务器
        grpc_server_ = builder.BuildAndStart();
        if (!grpc_server_) {
            throw std::runtime_error("Failed to build and start gRPC server");
        }

        // 设置健康检查状态
        auto health_service = grpc_server_->GetHealthCheckService();
        if (health_service) {
            health_service->SetServingStatus(svc_name_, true);
        }

        spdlog::info("gRPC Server start success at {}!", addr_);

        grpc_server_->Wait();

    } catch (const std::exception& e) {
        spdlog::error("Failed to start gRPC Server: {}", e.what());
        throw;
    }
}

// 关闭 gRPC 服务器
void KyleCacheServer::Stop() {
    if (etcd_register_) {
        etcd_register_->Unregister();
        etcd_register_.reset();
    }
    if (grpc_server_) {
        grpc_server_->Shutdown();
        grpc_server_.reset();
    }
    spdlog::info("gRPC Server {} stopped.", addr_);
}

auto KyleCacheServer::LoadTLSCredentials(const std::string& cert_file, const std::string& key_file)
    -> std::shared_ptr<grpc::ServerCredentials> {
    auto cert = ReadFile(cert_file);
    auto key = ReadFile(key_file);
    if (!cert || cert->empty() || !key || key->empty()) {
        spdlog::error("Failed to read TLS cert/key files: [{}] [{}]", cert_file, key_file);
        return nullptr;
    }
    grpc::SslServerCredentialsOptions ssl_opts;
    ssl_opts.pem_key_cert_pairs.push_back({*key, *cert});
    ssl_opts.client_certificate_request = GRPC_SSL_DONT_REQUEST_CLIENT_CERTIFICATE;
    return grpc::SslServerCredentials(ssl_opts);
}

}  // namespace kylecache
