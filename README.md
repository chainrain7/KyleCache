# KyleCache

[![CI](https://github.com/chainrain7/KyleCazche/actions/workflows/ci.yml/badge.svg)](https://github.com/chainrain7/KyleCazche/actions/workflows/ci.yml)

KyleCache 是一个类似 Memcached 的分布式缓存系统，采用客户端-服务器架构。系统基于一致性哈希算法实现数据分片和负载均衡，使用 LRU（最近最少使用）缓存淘汰算法。节点间通过 gRPC 进行高效通信，基于 etcd 实现服务注册与发现。

与 Memcached 类似，KyleCache 提供了独立的客户端 SDK，应用程序通过 SDK 直接与缓存集群交互。同时提供了 HTTP 网关作为示例，展示如何基于 SDK 构建 REST API 服务。该项目使用 Conan 作为包管理工具，使用 CMake 作为构建工具。

## 运行环境

- Ubuntu 22.04 (Docker)
- GCC 11.4 (use C++ 17)
- CMake 3.22.1
- Conan 2.16.1

## 项目依赖

- gflags / 2.2.2
- gtest / 1.16.0
- protobuf / 3.21.12
- grpc / 1.54.3
- etcd-cpp-apiv3 / 0.15.4
- fmt / 11.1.3
- spdlog / 1.15.1
- cpp-httplib / 0.20.1
- nlohmann_json / 3.12.0

该项目使用 [conan](https://conan.io/) 作为依赖管理工具，在配置该项目时需确保当前系统上安装了 conan。

> 项目 v2 版本不使用 vcpkg 了是因为其无法下载 etcd-cpp-apiv3，为了方便管理第三方库就只使用了 conan。

## 架构设计

KyleCache 采用客户端-服务器架构，类似于 Memcached：

```
┌─────────────┐     ┌─────────────┐     ┌─────────────┐
│   客户端1    │     │   客户端2    │     │  HTTP网关   │
│ (使用 SDK)  │     │ (使用 SDK)  │     │ (基于 SDK)  │
└──────┬──────┘     └──────┬──────┘     └──────┬──────┘
       │                   │                   │
       │ ┌─────────────────┴───────────────────┘
       │ │          一致性哈希路由 + 服务发现
       │ │                   │
       └─┴───────────────────┴─────────────────┐
         │                                     │
    ┌────▼────┐         ┌────────┐       ┌────▼────┐
    │ Node A  │◄────────┤  etcd  ├──────►│ Node C  │
    │ (gRPC)  │         │(注册中心)│       │ (gRPC)  │
    └────┬────┘         └────────┘       └────┬────┘
         │                                     │
         └──────────────►┌────────┐◄───────────┘
                        │ Node B  │
                        │ (gRPC)  │
                        └─────────┘
```

### 数据流程

**GET 请求：**
1. 客户端/网关通过 SDK 使用一致性哈希确定目标节点
2. 向目标节点发送 gRPC Get 请求
3. 节点首先检查本地 LRU 缓存
4. 若缓存未命中，从数据源加载（通过 Getter 回调）
5. 将数据存入本地缓存并返回给客户端

**SET 请求（写失效，最终一致）：**
1. 客户端/网关通过一致性哈希确定目标节点
2. 向目标节点发送 gRPC Set 请求写入数据（失败则沿哈希环故障转移到下一节点）
3. 向其余所有节点并行发送 Invalidate 请求（缓存失效，单节点失败自动重试）
4. 所有失效广播完成后返回成功；任一节点最终失败则整体返回失败

> 注意：Set 采用"写失效"而非强一致协议——在失效广播完成前，其他节点可能短暂读到旧值；若某节点的失效广播彻底失败，该节点会残留旧值，直至该 key 被再次写入或删除。

**DELETE 请求：**
1. 客户端/网关向所有节点广播 Delete 请求
2. 各节点删除本地缓存
3. 全部完成后返回成功

## 设计理念

KyleCache 的设计灵感来源于：
- **Memcached**：采用客户端-服务器架构，提供独立的客户端 SDK
- **GroupCache**：借鉴了 Group、SingleFlight 等概念，实现缓存穿透防护

与 GroupCache 的主要区别：
- GroupCache 是嵌入式缓存库，节点间自动协调；KyleCache 是独立服务，通过 SDK 访问
- GroupCache 只读缓存；KyleCache 支持完整的 CRUD 操作
- KyleCache 提供了更灵活的客户端集成方式

## 项目结构

```
.
├── include/kylecache          # 公共头文件（客户端 SDK）
│   └── client.h           # KyleCacheClient SDK 接口
├── src/
│   ├── cache/             # LRU 缓存实现
│   ├── client/            # 客户端 SDK 实现
│   │   └── client_sdk.cpp
│   ├── consistent_hash/   # 一致性哈希
│   ├── group/             # 缓存组（逻辑命名空间）
│   ├── server/            # gRPC 服务器
│   ├── registry/          # etcd 服务注册
│   ├── proto/             # Protobuf 定义
│   │   └── kylecache.proto
│   ├── include/kylecache/    # 内部头文件
│   └── main.cpp           # 缓存节点入口
├── example/
│   └── http_gateway/      # HTTP 网关示例（基于 SDK）
├── test/                  # 单元测试与基准测试
│   ├── test_lru.cpp
│   ├── test_consistent_hash.cpp
│   ├── test_group.cpp
│   └── bench_cache.cpp    # 简易基准（LRU / 一致性哈希）
├── scripts/
│   └── e2e_test.sh        # docker compose 端到端测试
├── .github/workflows/ci.yml  # CI：构建 + 单元测试 + 基准冒烟
├── CMakeLists.txt
├── conanfile.txt
└── README.md
```

### 核心模块

- **kylecache_core**：核心业务逻辑库（缓存、哈希、组管理、服务器等）
- **kylecache_client_sdk**：客户端 SDK 库，封装服务发现、路由、通信
- **node_server**：缓存节点可执行程序
- **http_gateway**：HTTP 网关示例（展示如何使用 SDK）

## 本地运行

项目中使用的第三方库 `etcd-cpp-apiv3` 中依赖的 `libsystemd/255`，而这个版本的 libsystemd 在高版本的 Linux Kernel 上有一个 bug：

```plain
Unknown filesystems defined in kernel headers:

Filesystem found in kernel header but not in filesystems-gperf.gperf: BCACHEFS_SUPER_MAGIC
Filesystem found in kernel header but not in filesystems-gperf.gperf: PID_FS_MAGIC
```

不过后面这个 bug 修复了，可以看[这里](https://lore.kernel.org/buildroot/ZmGjGvRCN3GwWFhp@landeda/T/)。而我们使用的 Conan 仓库中 etcd-cpp-apiv3 的最新版本还是依赖了 libsystemd/255。

因此如果继续使用 Conan 作为包管理器的话，就需要使用低版本的内核（6.8及以下），这里我使用的是 Ubuntu22.04，内核版本为 5.15，可以使用虚拟机或者 Docker。项目给出了 Dockerfile，能比较方便的构建出镜像，这里在下一章会有说明。

### 项目构建

1. 使用 conan 的 CMake 配置:

    在项目根目录下执行：

    ```sh
    conan install . --build=missing -s build_type=<Debug|Release>
    ```

    Debug 和 Release 取决于你的选择。

2. 配置项目：

    ```sh
    # cmake >= 3.23 
    cmake --preset conan-debug
    # cmake < 3.23
    cmake -DCMAKE_TOOLCHAIN_FILE=build/Release/generators/conan_toolchain.cmake -DCMAKE_BUILD_TYPE=Debug -S . -B build -G Ninja
    ```

    可按照你的需求将命令中的 debug(Debug) 换成 release(Release)

3. 构建项目：

    ```sh
    cmake --build build -j
    ```

完成后，在 `src/proto` 目录下会生成 Protobuf 相关文件，在 `bin/` 目录下会生成可执行程序：

- `node_server`：缓存节点程序
- `http_gateway`：HTTP 网关程序（示例）
- `test_*`：单元测试程序

### 启动 etcd

```sh
docker run -d --name etcd \ 
        -p 2379:2379 \
        quay.io/coreos/etcd:v3.5.0 \
        etcd --advertise-client-urls http://0.0.0.0:2379 \
        --listen-client-urls http://0.0.0.0:2379
```

### 运行

在不同终端启动缓存节点：

```sh
./bin/node_server --port=8001 --node=A
./bin/node_server --port=8002 --node=B
./bin/node_server --port=8003 --node=C
```

启动 HTTP 网关（可选，也可以直接使用 SDK）：

```sh
./bin/http_gateway --http_port=9000
```

## Docker 运行

### 构建镜像  

```sh
docker build -t kylecache:latest .  
```

> PS：如果构建时间长或者失败，可以考虑使用本地网络和代理：  
> `docker build --network host --build-arg HTTP_PROXY=http://your-proxy:port --build-arg HTTPS_PROXY=http://your-proxy:port -t kylecache:latest .`

构建镜像时需要安装依赖，编译第三方库，可以喝杯☕慢慢等待~

### 单节点运行  

**启动 etcd**： 

启动 etcd：  

```sh
docker run -d --name etcd   
  -p 2379:2379   
  quay.io/coreos/etcd:v3.5.0   
  etcd --advertise-client-urls http://0.0.0.0:2379   
  --listen-client-urls http://0.0.0.0:2379  
```

也可以在自己电脑上安装 etcd 来启动。  

**启动一个 kylecache node**：

```sh
docker run -d   
  --name kylecache-node   
  -p 8001:8001   
  --network host   
  kylecache:latest   
  /app/bin/node_server --port=8001 --node=A
```

### 多节点集群  

使用 Docker Compose 一键启动集群。其中有三个节点，还有一个使用 client sdk 构建的示例网关。

```sh
# 启动整个集群（包含 etcd + 3个节点 + 网关）  
docker compose up -d  
```

可以通过 ps 查看服务状态， log 命令查看节点和网关的日志：  

```sh
# 查看服务状态  
docker compose ps  

# 查看日志  
docker compose logs -f  
```

结束任务，删除容器：  

```sh
# 停止服务  
docker compose down  
```

## 核心组件

### 客户端 SDK (KyleCacheClient)

独立的客户端库，提供与缓存集群交互的统一接口：

```cpp
// 初始化客户端（第三个参数可选，用于调整超时/重试策略）
KyleCacheClient client("http://127.0.0.1:2379", "kylecache");

// 基本操作
auto value = client.Get("group_name", "key");  // 获取缓存
client.Set("group_name", "key", "value");      // 设置缓存（写失效）
client.Delete("group_name", "key");            // 删除缓存（全节点广播）

// 拉取各节点统计（回源次数、命中率、耗时等）
auto stats = client.Stats("group_name");
```

**核心功能：**
- 自动服务发现（etcd watch 实时更新节点列表，断线自动重连并全量同步）
- 一致性哈希路由，目标节点不可用时沿哈希环故障转移
- gRPC 连接按节点复用（连接池），所有 RPC 带超时
- 写失效策略：Set 写入目标节点后并行向其余节点广播失效（单节点失败重试），Delete 并行广播所有节点

### 缓存节点 (Node Server)

每个节点是独立的缓存服务器：

**功能：**
- 提供 gRPC 服务端接口（Get/Set/Delete/Invalidate/Stats）
- 管理本地 LRU 缓存
- 启动时以短租约注册到 etcd，靠后台线程续约；续约失败自动用新租约重新注册，节点不会从服务发现中静默消失
- 响应客户端请求并执行缓存操作

**内部组件：**
- **Group**：缓存的逻辑命名空间，支持多租户隔离，内置原子统计计数器
- **LRU Cache**：线程安全的本地缓存，自动淘汰最少使用数据（淘汰回调在锁外执行）
- **SingleFlight**：防止缓存击穿，同一 key 的并发请求合并为一次加载；回源异常安全传播，不会留下卡死的等待者

### 一致性哈希

客户端使用一致性哈希算法将 key 映射到节点：

- 虚拟节点机制确保负载均衡
- 节点变化时最小化数据迁移
- 支持按负载动态调整虚拟节点数，但默认关闭（动态调整会改变 key 到节点的映射，破坏缓存路由稳定性，仅在特殊场景开启）

### 服务注册与发现

基于 etcd 实现：

- 节点启动时注册自己的地址（自动补全本机 IP）到 etcd
- 客户端通过 etcd watch 实时感知节点变化，watch 断开后指数退避重连并全量同步节点列表
- 节点下线（租约过期）时自动从集群中移除

### HTTP 网关（可选）

基于 SDK 实现的 REST API 网关，展示如何集成客户端库：

- 将 HTTP 请求转换为 SDK 调用
- 提供标准的 REST API 接口
- 可作为参考实现集成到现有应用


## 使用 curl 访问示例网关

启动服务后，可以通过本机的 9000 端口来访问服务：  

1. Get  

```sh
curl http://127.0.0.1:9000/api/cache/default/Kevin  
# 输出：{"group":"test","key":"Kevin","value":"370"}⏎  
```

2. Set（支持表单 `-d 'value=xxx'`、JSON body `{"value":"xxx"}` 与原始 body 三种形式）

```sh
$ curl -X POST http://127.0.0.1:9000/api/cache/default/Kevin -d 'value=1219'     
# 输出：{"group":"test","key":"Kevin","success":true,"value":"1219"}⏎  
```

3. Delete

```sh
$ curl -X DELETE http://localhost:9000/api/cache/default/Kevin  
# 输出：{"deleted":true,"group":"test","key":"Kevin"}⏎  
```

4. Stats（查看各节点上指定 group 的统计：回源次数、本地命中/未命中、回源耗时等）

```sh
$ curl http://127.0.0.1:9000/api/stats/default  
# 输出：[{"group":"default","load_duration_ns":151500,"loader_errors":0,"loader_hits":1,"loads":1,"local_hits":2,"local_misses":1,"node":"192.168.1.10:8001"}, ...]⏎  
```

## 开发

### 单元测试

构建后通过 ctest 运行（覆盖 LRU、一致性哈希、缓存组/SingleFlight）：

```sh
ctest --test-dir build --output-on-failure
```

### 基准测试

```sh
./bin/bench_cache
```

输出 LRU Set/Get（命中与未命中）与一致性哈希路由的吞吐（ops/s）。

实测（WSL2 Ubuntu-22.04，i7-14650HX 24 核，Release 构建）：

- LRU 引擎（单线程）：Get 命中 **1644 万 ops/s**、Get 未命中 1869 万 ops/s、Set 1147 万 ops/s（value 128B，见 `test/bench_cache.cpp`）
- 一致性哈希路由：1146 万 ops/s（10 节点 × 10 虚拟节点）

### 负载测试

对完整集群（etcd + 3 缓存节点 + HTTP 网关，同机部署）压测 REST API：

```sh
pip3 install aiohttp
python3 scripts/load_test.py
```

实测（同上环境，aiohttp 客户端，每场景 15s）：

| 场景 | 吞吐 | P50 | P95 |
|---|---|---|---|
| GET 命中（c=50） | 8131 req/s | 2.0ms | 3.1ms |
| GET 命中（c=200） | 7605 req/s | 2.1ms | 3.2ms |
| GET 未命中+回源（c=50） | 7884 req/s | 2.1ms | 3.3ms |
| SET+并行广播失效（c=50） | 5181 req/s | 3.3ms | 4.5ms |

> 排障记录：网关最初只有约 400 req/s（P50 44ms）。排查发现 cpp-httplib 默认不设置 `TCP_NODELAY`，keepalive 长连接上的小响应包会被 Nagle 算法与延迟 ACK 组合卡出约 40ms 的固定延迟；对网关开启 `CPPHTTPLIB_TCP_NODELAY` 后吞吐提升约 20 倍。

### 端到端测试

基于 docker compose 拉起 etcd + 3 节点 + 网关，走 REST API 验证 Set/Get/Delete/Stats 全链路：

```sh
bash scripts/e2e_test.sh
```

### CI

GitHub Actions（`.github/workflows/ci.yml`）：Ubuntu 22.04 上 conan 安装依赖、构建、跑单元测试与基准冒烟。


## 许可证

该项目使用 MIT 许可证，详情见 [LICENSE](LICENSE) 文件。
