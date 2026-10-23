"""LUMINA 真实 RW2 样张复测门槛（WP-C2 收尾项）

用户提供 20 张真实松下 S5M2 RW2（~29MB/张，561MB）→ 建独立库实测：
  导入 → 缩略图 → 6K 预览基准（3 轮冷缓存，对标 bench-1.1.0 §1.1 DoD <1500ms）
  → 内嵌代理提取 → 全尺寸解码 → 编辑会话（真实 crs 补丁）→ 导出 → 原片零改写校验
  → 引擎内存采样。结论落盘 docs/rw2-real-bench.md。

用法：python scripts/gate_rw2_real.py [--home D:/LUMINA/build/rw2-real]
"""

import argparse
import hashlib
import json
import os
import shutil
import statistics
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from rpc_probe import Engine  # noqa: E402

SAMPLES = "D:/LUMINA/samples/rw2"
DEFAULT_HOME = "D:/LUMINA/build/rw2-real"
REPORT = "D:/LUMINA/docs/rw2-real-bench.md"

PATCH = {
    "toneCurveRGB": [{"x": 0, "y": 0}, {"x": 64, "y": 52}, {"x": 128, "y": 140}, {"x": 255, "y": 255}],
    "hsl": {"blueHue": 12, "blueSat": 25, "blueLum": -8, "orangeSat": 14},
}


def md5(path):
    h = hashlib.md5()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def md5_tree(root):
    out = {}
    for dirpath, _, files in os.walk(root):
        for name in files:
            p = os.path.join(dirpath, name)
            out[os.path.relpath(p, root)] = md5(p)
    return out


def pct(values, q):
    if not values:
        return 0.0
    s = sorted(values)
    k = max(0, min(len(s) - 1, int(round((q / 100.0) * (len(s) - 1)))))
    return s[k]


def rss_kb(pid):
    try:
        out = subprocess.run(
            ["tasklist", "/FI", "PID eq %d" % pid, "/FO", "CSV", "/NH"],
            capture_output=True, text=True, timeout=10,
        ).stdout
        # "映像名称","PID","会话名","会话#","内存使用"
        parts = [p.strip('"') for p in out.strip().split('","')]
        return int(parts[-1].replace(",", "").replace('"', "").replace(" K", ""))
    except Exception:
        return -1


def log(msg):
    print("[%s] %s" % (time.strftime("%H:%M:%S"), msg), flush=True)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--home", default=DEFAULT_HOME)
    args = ap.parse_args()
    home = args.home
    lib = os.path.join(home, "library")

    if os.path.isdir(home):
        shutil.rmtree(home)
    os.makedirs(lib, exist_ok=True)
    log("home=%s (fresh) samples=%s" % (home, SAMPLES))

    samples_before = md5_tree(SAMPLES)
    log("samples md5 baseline: %d files" % len(samples_before))

    eng = Engine(home=home)
    rss = {}
    t = {}
    errors = []

    def call(method, params=None, timeout=300):
        r = eng.call(method, params, timeout=timeout)
        if "error" in r:
            errors.append("%s: %s" % (method, json.dumps(r["error"], ensure_ascii=False)[:200]))
            log("  RPC ERROR %s -> %s" % (method, r["error"]))
        return r

    try:
        call("system.initialize", {"clientName": "rw2-gate", "clientVersion": "1.0.0", "protocolVersion": "1.0"})
        rss["boot"] = rss_kb(eng.proc.pid)

        # ---------- P1 导入 ----------
        t0 = time.time()
        r = call("import.start", {
            "source": {"path": SAMPLES},
            "target": lib,
            "renameTemplate": "{YYYY}-{MM}-{DD}_{camera}_{name}",
            "verifyMd5": True,
            "pairRawJpg": False,
        })
        job_id = (r.get("result") or {}).get("jobId")
        done = eng.wait_job(job_id, timeout=900) if job_id else {}
        t["import_s"] = round(time.time() - t0, 2)
        imp = done.get("result") or {}
        log("import done in %.1fs: %s" % (t["import_s"], json.dumps(imp, ensure_ascii=False)[:200]))
        rss["after_import"] = rss_kb(eng.proc.pid)

        q = call("library.queryPhotos", {"page": {"limit": 100, "offset": 0}})
        items = (q.get("result") or {}).get("items") or []
        ids = [it["id"] for it in items]
        log("library: %d photos" % len(ids))
        if len(ids) < 20:
            errors.append("import: expected >=20 photos, got %d" % len(ids))

        lib_before = md5_tree(lib)

        # ---------- P2 缩略图（冷缓存） ----------
        # 注意：thumb.getStatus 返回 {items:[...]}（在途项），没有 pending/running 计数器；
        # 轮询 ensureBatch 返回的 state 直到全部 ready（修：早前误用 getStatus 字段白等 300s）。
        t0 = time.time()
        ready = False
        while time.time() - t0 < 300:
            r = call("thumb.ensureBatch", {"ids": ids})
            states = [(it.get("id"), it.get("state")) for it in ((r.get("result") or {}).get("items") or [])]
            if states and all(s == "ready" for _, s in states):
                ready = True
                break
            time.sleep(1.0)
        t["thumbs_s"] = round(time.time() - t0, 2)
        log("thumbnails ready=%s in %.1fs" % (ready, t["thumbs_s"]))
        rss["after_thumbs"] = rss_kb(eng.proc.pid)

        # ---------- P3 6K 预览基准（3 轮冷缓存） ----------
        rounds = []
        for rnd in range(3):
            call("cache.clear", {"scope": "previews"})
            times = []
            for pid in ids:
                t1 = time.time()
                r = call("preview.getBase", {"id": pid}, timeout=300)
                dt = (time.time() - t1) * 1000
                res = r.get("result") or {}
                if "error" in r or not res.get("path"):
                    errors.append("preview.getBase id=%s failed" % pid)
                else:
                    times.append(dt)
            rounds.append(times)
            log("preview round %d: n=%d p50=%.0fms p95=%.0fms max=%.0fms"
                % (rnd + 1, len(times), pct(times, 50), pct(times, 95), max(times) if times else 0))
        all_prev = [x for rnd in rounds for x in rnd]
        t["preview_p50"] = round(pct(all_prev, 50), 1)
        t["preview_p95"] = round(pct(all_prev, 95), 1)
        t["preview_max"] = round(max(all_prev) if all_prev else 0, 1)
        t["preview_n"] = len(all_prev)
        rss["after_preview"] = rss_kb(eng.proc.pid)

        # ---------- P4 内嵌代理提取 ----------
        proxy_times = []
        for pid in ids:
            t1 = time.time()
            r = call("raw.getProxy", {"id": pid}, timeout=120)
            if "error" not in r:
                proxy_times.append((time.time() - t1) * 1000)
        t["proxy_p50"] = round(pct(proxy_times, 50), 1)
        t["proxy_max"] = round(max(proxy_times) if proxy_times else 0, 1)
        log("proxy extract: p50=%.0fms max=%.0fms" % (t["proxy_p50"], t["proxy_max"]))

        # ---------- P5 全尺寸解码（3 张，最重路径） ----------
        full_times = []
        for pid in ids[:3]:
            t1 = time.time()
            r = call("raw.decode", {"id": pid, "scale": "full"}, timeout=600)
            if "error" not in r:
                full_times.append((time.time() - t1) * 1000)
        t["full_p50"] = round(pct(full_times, 50), 1)
        t["full_max"] = round(max(full_times) if full_times else 0, 1)
        log("full decode: n=%d p50=%.0fms max=%.0fms" % (len(full_times), t["full_p50"], t["full_max"]))
        rss["after_full"] = rss_kb(eng.proc.pid)

        # ---------- P6 编辑会话（真实补丁） ----------
        t0 = time.time()
        for pid in ids:
            b = call("edit.beginSession", {"id": pid})
            sid = (b.get("result") or {}).get("sessionId")
            if not sid:
                errors.append("beginSession id=%s failed" % pid)
                continue
            call("edit.setParams", {"sessionId": sid, "patch": PATCH, "commitHistory": True})
            e = call("edit.endSession", {"sessionId": sid, "flush": True})
            res = e.get("result") or {}
            if res and res.get("flushed") is False:
                errors.append("endSession id=%s not flushed" % pid)
        t["edit_s"] = round(time.time() - t0, 2)
        log("edit sessions: %d in %.1fs" % (len(ids), t["edit_s"]))

        # ---------- P7 导出（5 张 JPG） ----------
        t0 = time.time()
        r = call("export.createJob", {
            "photoIds": ids[:5],
            "spec": {"format": "jpg", "longEdge": 2048, "colorSpace": "sRGB", "quality": 92},
        })
        jid = (r.get("result") or {}).get("jobId")
        if jid:
            done = eng.wait_job(jid, timeout=900)
            res = done.get("result") or {}
            log("export done in %.1fs: %s" % (time.time() - t0, json.dumps(res, ensure_ascii=False)[:200]))
            if res.get("failed"):
                errors.append("export failures: %s" % res.get("failed"))
        else:
            errors.append("export.createJob returned no jobId")
        t["export_s"] = round(time.time() - t0, 2)
        rss["after_export"] = rss_kb(eng.proc.pid)

        # ---------- P8 原片零改写 ----------
        lib_after = md5_tree(lib)
        changed = [k for k in lib_before if k in lib_after and lib_before[k] != lib_after[k]]
        missing = [k for k in lib_before if k not in lib_after]
        log("zero-write: changed=%d missing=%d (of %d)" % (len(changed), len(missing), len(lib_before)))
        for k in changed[:5]:
            errors.append("ZERO-WRITE VIOLATION: %s" % k)
        samples_after = md5_tree(SAMPLES)
        s_changed = [k for k in samples_before if samples_before[k] != samples_after.get(k)]
        log("samples untouched: %s (%d changed)" % (not s_changed, len(s_changed)))
        for k in s_changed[:5]:
            errors.append("SAMPLE MODIFIED: %s" % k)

        sidecars = [k for k in lib_after if k.lower().endswith(".xmp")]
        log("sidecars in library: %d" % len(sidecars))

        rss["final"] = rss_kb(eng.proc.pid)
    finally:
        eng.close()

    # ---------- 报告 ----------
    dod = t.get("preview_max", 0) < 1500
    lines = [
        "# 真实 RW2 样张复测报告（WP-C2 收尾）",
        "",
        "> 日期：%s ｜ 执行：team-lead ｜ 库：`%s`（20 张真实 PANA*.RW2，561MB，verifyMd5 导入）"
        % (time.strftime("%F"), home),
        "> 基准对照：`docs/bench-1.1.0.md` §1.1（合成 Linear DNG，优化后 p95=642ms / max=743ms，DoD <1500ms）",
        "",
        "## 指标",
        "",
        "| 项 | 结果 | 门槛/对照 | 判定 |",
        "|---|---|---|---|",
        "| 导入 20 张（含 MD5 校验） | %ss | — | — |" % t.get("import_s"),
        "| 缩略图 20 张（冷缓存） | %ss | — | — |" % t.get("thumbs_s"),
        "| 6K 预览 p50 | %sms | 对照 571ms | — |" % t.get("preview_p50"),
        "| 6K 预览 p95 | %sms | 对照 642ms | — |" % t.get("preview_p95"),
        "| 6K 预览 max | %sms | **< 1500ms** | %s |" % (t.get("preview_max"), "✅" if dod else "❌"),
        "| 内嵌代理提取 p50/max | %s / %sms | — | — |" % (t.get("proxy_p50"), t.get("proxy_max")),
        "| 全尺寸解码 p50/max | %s / %sms | — | — |" % (t.get("full_p50"), t.get("full_max")),
        "| 编辑会话 20 个 | %ss | — | — |" % t.get("edit_s"),
        "| 导出 5 张 JPG | %ss | — | — |" % t.get("export_s"),
        "| 原片零改写 | changed=%d missing=%d | **0 / 0** | %s |"
        % (len(changed), len(missing), "✅" if not changed and not missing else "❌"),
        "| 样张原文件 | %d 改动 | **0** | %s |" % (len(s_changed), "✅" if not s_changed else "❌"),
        "| sidecar 落盘 | %d 个 | 20 | %s |" % (len(sidecars), "✅" if len(sidecars) >= 20 else "⚠️"),
        "",
        "## 引擎内存（RSS，tasklist 实测）",
        "",
        "```",
        json.dumps(rss, ensure_ascii=False, indent=2),
        "```",
        "",
        "## 错误",
        "",
        ("无" if not errors else "\n".join("- %s" % e for e in errors)),
        "",
        "## 结论",
        "",
        ("**PASS**：真实 RW2 全链路达标，6K 预览 max %sms < 1500ms 门槛，原片零改写成立。"
         % t.get("preview_max")) if (dod and not changed and not s_changed and not errors)
        else "**FAIL**：见错误清单与指标表。",
        "",
    ]
    with open(REPORT, "w", encoding="utf-8") as f:
        f.write("\n".join(lines))
    log("report -> %s" % REPORT)
    log("RESULT: %s (preview_max=%sms, errors=%d)" % ("PASS" if dod and not errors else "FAIL", t.get("preview_max"), len(errors)))
    return 0 if (dod and not errors) else 1


if __name__ == "__main__":
    sys.exit(main())
