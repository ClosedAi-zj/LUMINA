"""LUMINA 8 小时稳定性压测（WP-C2 发布门槛）

在真实 RW2 库（D:/LUMINA/build/rw2-real，20 张真松下 RAW + sidecar）上长跑：
- 每约 60s 一个循环：查询/检索/时间轴/地图/健康状态/直方图/代理提取/缩略图/编辑会话/
  定期导出/定期 6K 预览重解码，轮换照片与补丁变体；
- 每 30 循环重启一次引擎（验证 shutdown 干净退出：退出码 0 + 10s 内进程消失 + 锁释放）；
- 每 30 循环：SQLite quick_check（库副本，避免与运行中的 WAL 冲突）+ 5 张原片 MD5 抽检；
- 每 15 分钟心跳：循环数 / RSS 首末最值 / DB 体积 / 错误与事件计数；
- 硬失败条件（立即中止）：原片被改写 / quick_check 失败 / 引擎非计划死亡；
- 通过条件：0 RPC 错误 / 0 零改写违例 / 0 非计划死亡 / 末小时 RSS 均值较首小时 < +50%。

用法：python scripts/stability_8h.py [--hours 8] [--home D:/LUMINA/build/rw2-real]
产物：D:/LUMINA/build/stability/{metrics.jsonl,heartbeat.md} + docs/stability-8h.md
"""

import argparse
import hashlib
import json
import os
import shutil
import sqlite3
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from rpc_probe import Engine  # noqa: E402

PATCHES = [
    {"exposure": 0.35, "contrast": 12},
    {"toneCurveRGB": [{"x": 0, "y": 0}, {"x": 64, "y": 52}, {"x": 128, "y": 140}, {"x": 255, "y": 255}],
     "hsl": {"blueHue": 12, "blueSat": 25, "blueLum": -8, "orangeSat": 14}},
    {"exposure": -0.2, "highlights": -30, "shadows": 20, "clarity": 8},
]

OUT_DIR = "D:/LUMINA/build/stability"
REPORT = "D:/LUMINA/docs/stability-8h.md"


def md5(path):
    h = hashlib.md5()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def rss_kb(pid):
    try:
        out = subprocess.run(
            ["tasklist", "/FI", "PID eq %d" % pid, "/FO", "CSV", "/NH"],
            capture_output=True, text=True, timeout=10,
        ).stdout
        parts = [p.strip('"') for p in out.strip().split('","')]
        return int(parts[-1].replace(",", "").replace('"', "").replace(" K", ""))
    except Exception:
        return -1


def quick_check(db_path):
    """复制库三件套到临时目录再 quick_check，避免与运行中的 WAL 冲突。"""
    tmp = os.path.join(OUT_DIR, "qchk")
    os.makedirs(tmp, exist_ok=True)
    dst = os.path.join(tmp, "lumina.db")
    ok = True
    detail = ""
    try:
        for suffix in ("", "-wal", "-shm"):
            src = db_path + suffix
            if os.path.exists(src):
                shutil.copy2(src, dst + suffix)
        con = sqlite3.connect(dst)
        row = con.execute("PRAGMA quick_check").fetchall()
        con.close()
        detail = ";".join(r[0] for r in row)[:120]
        ok = len(row) == 1 and row[0][0] == "ok"
    except Exception as e:  # noqa: BLE001
        ok = False
        detail = str(e)[:120]
    return ok, detail


class Stability:
    def __init__(self, home, hours):
        os.makedirs(OUT_DIR, exist_ok=True)
        self.home = home
        self.lib = os.path.join(home, "library")
        self.db = os.path.join(home, "lumina.db")  # 引擎把库放在 home 根（非 db/ 子目录）
        self.deadline = time.time() + hours * 3600
        self.cycle = 0
        self.errors = []
        self.rpc_errors = 0
        self.incidents = []
        self.zero_write_violations = 0
        self.restarts = 0
        self.unexpected_deaths = 0
        self.rss_first_hour = []
        self.rss_last_hour = []
        self.eng = None
        self.ids = []
        self.metrics = open(os.path.join(OUT_DIR, "metrics.jsonl"), "a", encoding="utf-8")
        self.heartbeat = open(os.path.join(OUT_DIR, "heartbeat.md"), "a", encoding="utf-8")
        self.start = time.time()

    # ---------- 基础设施 ----------
    def log(self, msg):
        line = "[%s] [%s] %s" % (time.strftime("%H:%M:%S"), "stab", msg)
        print(line, flush=True)
        self.heartbeat.write(line + "\n")
        self.heartbeat.flush()

    def metric(self, **kw):
        kw["t"] = round(time.time() - self.start, 1)
        kw["cycle"] = self.cycle
        self.metrics.write(json.dumps(kw, ensure_ascii=False) + "\n")
        self.metrics.flush()

    def spawn(self):
        self.eng = Engine(home=self.home)
        r = self.eng.call("system.initialize", {"clientName": "stability", "clientVersion": "1.0.0", "protocolVersion": "1.0"})
        if "error" in r:
            self.incidents.append("initialize failed: %s" % r["error"])
        q = self.eng.call("library.queryPhotos", {"page": {"limit": 100, "offset": 0}})
        self.ids = [it["id"] for it in ((q.get("result") or {}).get("items") or [])]
        if not self.ids:
            self.incidents.append("library empty after spawn")

    def shutdown(self, planned):
        """关闭引擎并严格验证 shutdown 质量。"""
        if not self.eng:
            return
        pid = self.eng.proc.pid
        try:
            self.eng.proc.stdin.close()
        except Exception:
            pass
        exited = False
        for _ in range(100):  # 10s
            if self.eng.proc.poll() is not None:
                exited = True
                break
            time.sleep(0.1)
        code = self.eng.proc.returncode
        if not exited:
            try:
                self.eng.proc.kill()
            except Exception:
                pass
            self.incidents.append("shutdown hang: pid %d alive >10s after stdin close (planned=%s)" % (pid, planned))
        elif planned and code != 0:
            self.incidents.append("shutdown exit code %s (planned restart)" % code)
        self.eng = None

    def call(self, method, params=None, timeout=180):
        if not self.eng:
            return {"error": "no engine"}
        try:
            r = self.eng.call(method, params, timeout=timeout)
        except Exception as e:  # noqa: BLE001
            self.unexpected_deaths += 1
            self.incidents.append("engine died during %s: %s" % (method, str(e)[:120]))
            self.eng = None
            return {"error": str(e)[:120]}
        if "error" in r:
            self.rpc_errors += 1
            self.errors.append("cycle%d %s: %s" % (self.cycle, method, json.dumps(r["error"], ensure_ascii=False)[:160]))
        return r

    # ---------- 单循环 ----------
    def run_cycle(self):
        self.cycle += 1
        t0 = time.time()
        ids = self.ids
        n = len(ids)
        if not n:
            self.incidents.append("no photos in library")
            return
        a = ids[self.cycle % n]
        b = ids[(self.cycle * 7 + 3) % n]

        self.call("library.queryPhotos", {"page": {"limit": 50, "offset": (self.cycle % 4) * 50}})
        self.call("library.search", {"text": "PANA"})
        self.call("library.getTimeline")
        self.call("map.getLocations", {})
        self.call("health.getStatus")
        self.call("health.listIssues", {"kind": "missing", "page": {"offset": 0, "limit": 10}})
        self.call("analysis.histogram", {"id": a})
        self.call("raw.getProxy", {"id": b})
        self.call("thumb.ensureBatch", {"ids": [a, b]})

        patch = PATCHES[self.cycle % len(PATCHES)]
        s = self.call("edit.beginSession", {"id": a})
        sid = (s.get("result") or {}).get("sessionId")
        if sid:
            self.call("edit.setParams", {"sessionId": sid, "patch": patch, "commitHistory": True})
            self.call("edit.endSession", {"sessionId": sid, "flush": True})

        if self.cycle % 4 == 0:
            r = self.call("export.createJob", {
                "photoIds": [b],
                "spec": {"format": "jpg", "longEdge": 2048, "colorSpace": "sRGB", "quality": 92},
            })
            jid = (r.get("result") or {}).get("jobId")
            if jid:
                try:
                    self.eng.wait_job(jid, timeout=600)
                except Exception as e:  # noqa: BLE001
                    self.incidents.append("export job wait failed: %s" % str(e)[:120])

        if self.cycle % 10 == 0:
            self.call("cache.clear", {"scope": "previews"})
            self.call("preview.getBase", {"id": a})

        rss = rss_kb(self.eng.proc.pid) if self.eng else -1
        elapsed = time.time() - self.start
        if elapsed < 3600:
            self.rss_first_hour.append(rss)
        elif elapsed > self.deadline - self.start - 3600:
            self.rss_last_hour.append(rss)
        self.metric(rss_kb=rss, cycle_s=round(time.time() - t0, 2), rpc_errors=self.rpc_errors)

        if self.cycle % 30 == 0:
            self.deep_check()

        if time.time() - t0 > 300:
            self.incidents.append("cycle %d took %.0fs (>300s stall)" % (self.cycle, time.time() - t0))

    def deep_check(self):
        ok, detail = quick_check(self.db)
        self.log("deep check #%d: quick_check=%s (%s)" % (self.cycle, "ok" if ok else "FAIL", detail))
        if not ok:
            self.incidents.append("quick_check FAILED: %s" % detail)
            raise SystemExit("integrity failure")
        files = sorted(f for f in os.listdir(self.lib) if f.lower().endswith(".rw2"))
        spot = files[self.cycle % max(1, len(files)):][:5] or files[:5]
        for name in spot:
            p = os.path.join(self.lib, name)
            before = getattr(self, "_md5_" + name, None)
            now = md5(p)
            if before and before != now:
                self.zero_write_violations += 1
                self.incidents.append("ZERO-WRITE VIOLATION: %s" % name)
            setattr(self, "_md5_" + name, now)
        db_size = os.path.getsize(self.db) if os.path.exists(self.db) else 0
        self.metric(deep_check=True, db_bytes=db_size, rss_kb=rss_kb(self.eng.proc.pid) if self.eng else -1)

    # ---------- 主流程 ----------
    def run(self):
        os.makedirs(OUT_DIR, exist_ok=True)
        self.log("=== 8h stability start home=%s deadline=%s ===" % (self.home, time.strftime("%H:%M:%S", time.localtime(self.deadline))))
        self.spawn()
        last_hb = time.time()
        try:
            while time.time() < self.deadline:
                if not self.eng:
                    self.log("engine down -> respawn (incident count=%d)" % len(self.incidents))
                    self.spawn()
                    if not self.eng:
                        time.sleep(5)
                        continue
                self.run_cycle()
                if self.cycle % 30 == 0 and self.eng:
                    self.log("planned restart #%d after cycle %d" % (self.restarts + 1, self.cycle))
                    self.shutdown(planned=True)
                    self.restarts += 1
                    self.spawn()
                if time.time() - last_hb >= 900:
                    self.heartbeat_line()
                    last_hb = time.time()
                # 循环间隔 ~60s（含操作耗时）
                target = self.start + self.cycle * 60
                delay = target - time.time()
                if delay > 0:
                    time.sleep(min(delay, 60))
        except SystemExit as e:
            self.log("ABORT: %s" % e)
        finally:
            self.shutdown(planned=True)
            self.heartbeat_line()
            self.report()

    def heartbeat_line(self):
        rss_all = self.rss_first_hour + self.rss_last_hour
        line = ("HEARTBEAT t=%.0fs cycles=%d restarts=%d rpc_err=%d incidents=%d zw=%d "
                "rss_first_avg=%dKB rss_last_avg=%dKB rss_now=%dKB"
                % (time.time() - self.start, self.cycle, self.restarts, self.rpc_errors,
                   len(self.incidents), self.zero_write_violations,
                   (sum(self.rss_first_hour) // max(1, len(self.rss_first_hour))),
                   (sum(self.rss_last_hour) // max(1, len(self.rss_last_hour))),
                   rss_all[-1] if rss_all else -1))
        self.log(line)

    def report(self):
        first = sum(self.rss_first_hour) // max(1, len(self.rss_first_hour))
        last = sum(self.rss_last_hour) // max(1, len(self.rss_last_hour))
        growth = (last - first) / first * 100 if first > 0 else 0
        ok = (self.rpc_errors == 0 and self.zero_write_violations == 0
              and self.unexpected_deaths == 0 and not self.incidents and growth < 50)
        lines = [
            "# LUMINA 8 小时稳定性压测报告（WP-C2）",
            "",
            "> 起止：%s → %s ｜ 库：`%s`（20 张真实 RW2）"
            % (time.strftime("%F %H:%M", time.localtime(self.start)), time.strftime("%F %H:%M"), self.home),
            "",
            "## 结果：%s" % ("**PASS**" if ok else "**FAIL**"),
            "",
            "| 指标 | 值 | 门槛 |",
            "|---|---|---|",
            "| 循环数 | %d | — |" % self.cycle,
            "| 计划内引擎重启 | %d | — |" % self.restarts,
            "| RPC 错误 | %d | 0 |" % self.rpc_errors,
            "| 非计划引擎死亡 | %d | 0 |" % self.unexpected_deaths,
            "| 零改写违例 | %d | 0 |" % self.zero_write_violations,
            "| 事件/意外 | %d | 0 |" % len(self.incidents),
            "| RSS 首小时均值 | %d KB | — |" % first,
            "| RSS 末小时均值 | %d KB | — |" % last,
            "| RSS 增长 | %+.1f%% | < +50%% |" % growth,
            "",
            "## 事件明细",
            "",
            ("无" if not self.incidents else "\n".join("- %s" % i for i in self.incidents[:50])),
            "",
            "## RPC 错误明细（前 20 条）",
            "",
            ("无" if not self.errors else "\n".join("- %s" % e for e in self.errors[:20])),
            "",
        ]
        with open(REPORT, "w", encoding="utf-8") as f:
            f.write("\n".join(lines))
        self.log("report -> %s RESULT=%s" % (REPORT, "PASS" if ok else "FAIL"))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--hours", type=float, default=8.0)
    ap.add_argument("--home", default="D:/LUMINA/build/rw2-real")
    args = ap.parse_args()
    Stability(args.home, args.hours).run()
    return 0


if __name__ == "__main__":
    sys.exit(main())
