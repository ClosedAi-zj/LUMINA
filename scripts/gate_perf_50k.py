# -*- coding: utf-8 -*-
"""发布门槛 · 5 万张库性能复测：queryPhotos 常规/深分页/FTS/日期过滤 各 120 次 P50/P95。"""
import json, os, queue, struct, subprocess, threading, time, statistics

ENGINE = r"D:/LUMINA/build/engine/lumina-engine.exe"
HOME = r"D:/LUMINA/build/runtime-bench50k"

proc = subprocess.Popen([ENGINE, "--home", HOME], stdin=subprocess.PIPE,
                        stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)
frames = queue.Queue()

def reader():
    f = proc.stdout
    try:
        while True:
            hdr = b""
            while len(hdr) < 4:
                ch = f.read(4 - len(hdr))
                if not ch: return
                hdr += ch
            (n,) = struct.unpack("<I", hdr)
            body = b""
            while len(body) < n:
                ch = f.read(n - len(body))
                if not ch: return
                body += ch
            frames.put(json.loads(body))
    except Exception:
        return
threading.Thread(target=reader, daemon=True).start()

def rpc(rid, method, params=None, timeout=120):
    pl = json.dumps({"jsonrpc": "2.0", "id": rid, "method": method,
                     "params": params or {}}, separators=(",", ":")).encode()
    t0 = time.perf_counter()
    proc.stdin.write(struct.pack("<I", len(pl)) + pl)
    proc.stdin.flush()
    end = time.time() + timeout
    while time.time() < end:
        try:
            r = frames.get(timeout=1)
        except queue.Empty:
            continue
        if r.get("id") == rid:
            return r, (time.perf_counter() - t0) * 1000
    raise RuntimeError("timeout " + method)

def pct(xs, p):
    xs = sorted(xs)
    k = min(len(xs) - 1, int(round((p / 100) * (len(xs) - 1))))
    return xs[k]

stats = rpc(1, "library.getStats", {})[0].get("result", {})
print("[perf] library stats:", {k: stats.get(k) for k in ("total", "missing", "edited")}, flush=True)

cases = {
    "query-basic": {"limit": 60},
    "filter-format": {"limit": 60, "filters": {"format": "RW2"}},
}
for name, params in cases.items():
    lat = []
    for i in range(120):
        p = dict(params)
        _, ms = rpc(100 + i, "library.queryPhotos", p)
        lat.append(ms)
    print(f"[perf] {name}: n=120 p50={pct(lat,50):.1f}ms p95={pct(lat,95):.1f}ms", flush=True)

# 深分页（OFFSET 4 万）
lat = []
for i in range(60):
    p = {"limit": 60, "offset": 40000 + i * 60}
    _, ms = rpc(1000 + i, "library.queryPhotos", p)
    lat.append(ms)
print(f"[perf] deep-page(offset~40k): n=60 p50={pct(lat,50):.1f}ms p95={pct(lat,95):.1f}ms", flush=True)

# FTS 与日期过滤
for name, params in {"fts": {"limit": 60, "search": "雾浓顶"},
                     "date-range": {"limit": 60, "filters": {"dateFrom": "2026-01-01", "dateTo": "2026-03-31"}}}.items():
    lat = []
    for i in range(60):
        _, ms = rpc(2000 + i, "library.queryPhotos", dict(params))
        lat.append(ms)
    print(f"[perf] {name}: n=60 p50={pct(lat,50):.1f}ms p95={pct(lat,95):.1f}ms", flush=True)

rpc(9999, "system.shutdown", {})
try:
    proc.wait(timeout=15)
except subprocess.TimeoutExpired:
    proc.kill()
print("[perf] DONE", flush=True)
