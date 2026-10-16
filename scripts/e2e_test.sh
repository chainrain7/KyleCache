#!/usr/bin/env bash
# 端到端测试：docker compose 启动 etcd + 3 个缓存节点 + HTTP 网关，
# 然后通过网关 REST API 验证 Set/Get/Delete/Stats 语义。
#
# 用法：bash scripts/e2e_test.sh
# 依赖：docker、docker compose、curl；首次运行需要构建镜像（耗时较长）

set -euo pipefail
cd "$(dirname "$0")/.."

GATEWAY=http://127.0.0.1:9000

cleanup() {
  docker compose down -v --remove-orphans >/dev/null 2>&1 || true
}
trap cleanup EXIT

echo "[1/6] building image and starting cluster (etcd + 3 nodes + gateway)..."
docker compose up -d --build

echo "[2/6] waiting for gateway to come up..."
code=000
for i in $(seq 1 60); do
  code=$(curl -s -o /dev/null -w '%{http_code}' "$GATEWAY/api/cache/default/e2e_health" || echo 000)
  # 网关活着时可能返回 200 或 404（key 不存在），只有连不上才是 000
  [ "$code" != "000" ] && break
  sleep 2
done
if [ "$code" = "000" ]; then
  echo "FAIL: gateway not reachable after 120s"
  docker compose logs --tail=50
  exit 1
fi
echo "      gateway is up (http code: $code)"

echo "[3/6] SET key via form-encoded body..."
resp=$(curl -sf -X POST "$GATEWAY/api/cache/default/e2e_key" -d 'value=e2e_value')
echo "      $resp"
echo "$resp" | grep -q '"success":true' || { echo "FAIL: Set response"; exit 1; }

echo "[4/6] GET key back..."
resp=$(curl -sf "$GATEWAY/api/cache/default/e2e_key")
echo "      $resp"
echo "$resp" | grep -q '"value":"e2e_value"' || { echo "FAIL: Get should return exactly e2e_value"; exit 1; }

echo "[5/6] DELETE key, then GET should be 404..."
resp=$(curl -sf -X DELETE "$GATEWAY/api/cache/default/e2e_key")
echo "$resp" | grep -q '"deleted":true' || { echo "FAIL: Delete response"; exit 1; }
code=$(curl -s -o /dev/null -w '%{http_code}' "$GATEWAY/api/cache/default/e2e_key")
[ "$code" = "404" ] || { echo "FAIL: expected 404 after delete, got $code"; exit 1; }

echo "[6/6] Stats endpoint returns per-node counters..."
resp=$(curl -sf "$GATEWAY/api/stats/default")
echo "      $resp"
echo "$resp" | grep -q '"local_hits"' || { echo "FAIL: stats missing local_hits"; exit 1; }
echo "$resp" | grep -q '"loader_hits"' || { echo "FAIL: stats missing loader_hits"; exit 1; }

echo
echo "e2e test PASSED"
