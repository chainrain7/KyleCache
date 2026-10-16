#include <memory>
#include <string>

#include <gflags/gflags.h>
#include <httplib.h>
#include <nlohmann/json.hpp>
#include <spdlog/common.h>
#include <spdlog/spdlog.h>

#include "kylecache/client.h"

DEFINE_int32(http_port, 9000, "HTTP服务端口");
DEFINE_string(etcd_endpoints, "http://127.0.0.1:2379", "etcd地址");
DEFINE_string(service_name, "kylecache", "缓存服务名称");
DEFINE_string(log_level, "info", "日志级别，可选值：trace, debug, info, warn, error, critical");

using namespace kylecache;

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

class HttpGateway {
public:
    HttpGateway(int port, const std::string& etcd_addr, const std::string& svc_name) : port_(port) {
        kylecache_client_ = std::make_unique<KyleCacheClient>(etcd_addr, svc_name);
        server_.set_payload_max_length(4 << 20);
        SetupRoutes();
    }

    ~HttpGateway() { server_.stop(); }

    void Start() {
        spdlog::info("Starting HTTP Gateway on port {}", port_);
        server_.listen("0.0.0.0", port_);
    }

private:
    void SetupRoutes() {
        // GET /api/cache/{group}/{key}
        server_.Get(R"(/api/cache/([^/]+)/([^/]+))",
                    [this](const httplib::Request& req, httplib::Response& res) { HandleGet(req, res); });

        // POST /api/cache/{group}/{key}
        server_.Post(R"(/api/cache/([^/]+)/([^/]+))",
                     [this](const httplib::Request& req, httplib::Response& res) { HandleSet(req, res); });

        // DELETE /api/cache/{group}/{key}
        server_.Delete(R"(/api/cache/([^/]+)/([^/]+))",
                       [this](const httplib::Request& req, httplib::Response& res) { HandleDelete(req, res); });

        // GET /api/stats/{group}
        server_.Get(R"(/api/stats/([^/]+))",
                    [this](const httplib::Request& req, httplib::Response& res) { HandleStats(req, res); });
    }

    // 解析请求体中的 value，按优先级支持：
    // 1) 表单：curl -d 'value=xxx'（application/x-www-form-urlencoded）
    // 2) JSON：{"value": "xxx"}
    // 3) 原始 body
    static std::string ParseValue(const httplib::Request& req) {
        if (req.has_param("value")) {
            return req.get_param_value("value");
        }
        try {
            auto body = nlohmann::json::parse(req.body);
            if (body.is_object() && body.contains("value") && body["value"].is_string()) {
                return body.value("value", "");
            }
        } catch (...) {
        }
        return req.body;
    }

    void HandleGet(const httplib::Request& req, httplib::Response& res) {
        std::string group = req.matches[1];
        std::string key = req.matches[2];

        auto value = kylecache_client_->Get(group, key);
        if (value) {
            nlohmann::json json_resp = {{"key", key}, {"value", *value}, {"group", group}};
            res.set_content(json_resp.dump() + "\n", "application/json");
        } else {
            SendError(res, 404, "Key not found or service unavailable");
        }
    }

    void HandleSet(const httplib::Request& req, httplib::Response& res) {
        std::string group = req.matches[1];
        std::string key = req.matches[2];

        std::string value = ParseValue(req);
        if (value.empty()) {
            SendError(res, 400, "Value is required");
            return;
        }

        bool success = kylecache_client_->Set(group, key, value);
        if (success) {
            nlohmann::json json_resp = {{"key", key}, {"value", value}, {"group", group}, {"success", true}};
            res.set_content(json_resp.dump() + "\n", "application/json");
        } else {
            SendError(res, 500, "Failed to Set value");
        }
    }

    void HandleDelete(const httplib::Request& req, httplib::Response& res) {
        std::string group = req.matches[1];
        std::string key = req.matches[2];

        bool success = kylecache_client_->Delete(group, key);
        if (success) {
            nlohmann::json json_resp = {{"key", key}, {"group", group}, {"deleted", true}};
            res.set_content(json_resp.dump() + "\n", "application/json");
        } else {
            SendError(res, 500, "Failed to delete key");
        }
    }

    void HandleStats(const httplib::Request& req, httplib::Response& res) {
        std::string group = req.matches[1];

        auto stats = kylecache_client_->Stats(group);
        nlohmann::json arr = nlohmann::json::array();
        for (const auto& s : stats) {
            arr.push_back({{"node", s.node},
                           {"group", s.group},
                           {"loads", s.loads},
                           {"local_hits", s.local_hits},
                           {"local_misses", s.local_misses},
                           {"loader_hits", s.loader_hits},
                           {"loader_errors", s.loader_errors},
                           {"load_duration_ns", s.load_duration_ns}});
        }
        res.set_content(arr.dump() + "\n", "application/json");
    }

    void SendError(httplib::Response& res, int code, const std::string& message) {
        nlohmann::json error = {{"error", message}, {"code", code}};
        res.status = code;
        res.set_content(error.dump(), "application/json");
    }

private:
    int port_;
    httplib::Server server_;
    std::unique_ptr<KyleCacheClient> kylecache_client_;
};

int main(int argc, char** argv) {
    gflags::ParseCommandLineFlags(&argc, &argv, true);

    spdlog::set_level(ParseLogLevel(FLAGS_log_level));
    spdlog::set_pattern("[http-gateway][%^%l%$] %v");

    try {
        HttpGateway gateway(FLAGS_http_port, FLAGS_etcd_endpoints, FLAGS_service_name);
        gateway.Start();
    } catch (const std::exception& e) {
        spdlog::error("Gateway failed: {}", e.what());
        return 1;
    }

    return 0;
}
