#!/usr/bin/env python3
"""KyleCache 集群负载测试。

对 HTTP 网关压测 REST API（需要集群已启动）：
    pip3 install aiohttp
    python3 scripts/load_test.py

为什么不用 ab：ab 不设置 TCP_NODELAY，其与延迟 ACK 的交互会把
keepalive 长连接的延迟虚高约 40ms，吞吐严重失真；aiohttp 客户端
默认启用 TCP_NODELAY，测得的是服务端真实性能。
"""
import asyncio
import time

import aiohttp

GW = "http://127.0.0.1:9000"
DURATION = 15.0  # 每个场景持续秒数


async def worker(session: aiohttp.ClientSession, method: str, url: str, data, lat: list,
                 errors: list, stop_at: float):
    while time.perf_counter() < stop_at:
        t0 = time.perf_counter()
        try:
            if method == "GET":
                async with session.get(url) as resp:
                    await resp.read()
                    if resp.status not in (200, 404):
                        errors.append(resp.status)
            else:
                async with session.post(url, data=data) as resp:
                    await resp.read()
                    if resp.status != 200:
                        errors.append(resp.status)
            lat.append(time.perf_counter() - t0)
        except Exception as e:
            errors.append(str(e)[:60])


async def run_scenario(name, concurrency, method, url, data=None):
    conn = aiohttp.TCPConnector(limit=concurrency + 10, force_close=False)
    timeout = aiohttp.ClientTimeout(total=5)
    async with aiohttp.ClientSession(connector=conn, timeout=timeout) as session:
        lat: list = []
        errors: list = []
        stop_at = time.perf_counter() + DURATION
        t0 = time.perf_counter()
        await asyncio.gather(*[
            worker(session, method, url, data, lat, errors, stop_at)
            for _ in range(concurrency)
        ])
        elapsed = time.perf_counter() - t0

        lat.sort()
        n = len(lat)
        if n == 0:
            print(f"[{name}] ALL FAILED: {errors[:3]}")
            return
        p = lambda q: lat[min(n - 1, int(n * q))] * 1000
        rps = n / elapsed
        print(f"[{name:22s}] {rps:9.0f} req/s | "
              f"P50 {p(0.50):7.2f}ms P95 {p(0.95):7.2f}ms P99 {p(0.99):8.2f}ms | "
              f"avg {sum(lat)/n*1000:7.2f}ms | errors {len(errors)}")


async def main():
    # 预热
    conn = aiohttp.TCPConnector(limit=10)
    async with aiohttp.ClientSession(connector=conn) as session:
        for _ in range(20):
            async with session.get(f"{GW}/api/cache/default/Tom") as r:
                await r.read()

    print(f"环境: WSL2 localhost, 每场景 {DURATION:.0f}s")
    await run_scenario("GET 命中 c=50", 50, "GET", f"{GW}/api/cache/default/Tom")
    await run_scenario("GET 命中 c=200", 200, "GET", f"{GW}/api/cache/default/Tom")
    await run_scenario("GET 未命中 c=50", 50, "GET", f"{GW}/api/cache/default/missing_key_zzz")
    await run_scenario("SET+广播失效 c=50", 50, "POST", f"{GW}/api/cache/default/Tom",
                       data="value=400")

    async with aiohttp.ClientSession() as session:
        async with session.get(f"{GW}/api/stats/default") as r:
            print("stats:", (await r.text())[:400])


if __name__ == "__main__":
    asyncio.run(main())
