# http 网关示例
add_executable(http_gateway "${CMAKE_CURRENT_SOURCE_DIR}/http_gateway.cpp")
target_link_libraries(http_gateway PRIVATE
    kylecache_client_sdk
    gflags::gflags
    httplib::httplib
    nlohmann_json::nlohmann_json)

# 关闭 Nagle：cpp-httplib 默认不设 TCP_NODELAY，keepalive 长连接上的小响应包
# 会被 Nagle + 延迟 ACK 组合卡出 ~40ms 的固定延迟（实测把吞吐压到几百 RPS）
target_compile_definitions(http_gateway PRIVATE CPPHTTPLIB_TCP_NODELAY=1)
