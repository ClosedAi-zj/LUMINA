#!/usr/bin/env python3
"""打包版端到端冒烟：启动 LUMINA.exe（临时 home）→ 等 mcp-port → 真 RPC → 收尾。

只用标准库。默认针对 build/pkg/LUMINA/LUMINA.exe；临时 home 用后即删，
绝不触碰真实图库（{home}/library 或 %APPDATA%/LUMINA）。

用法：
  python scripts/smoke-packaged.py [--exe <path>] [--keep-home] [--timeout 90]

退出码：0 = 冒烟通过；1 = 失败（打印失败原因）。
"""

import argparse
import json
import os
import shutil
import socket
import subprocess
import sys
import time

DEFAULT_EXE = os.path.join("build", "pkg", "LUMINA", "LUMINA.exe")


def log(msg: str) -> None:
    print(msg, flush=True)


def strip_host_leaks(env: dict) -> None:
    """宿主会泄漏 ELECTRON_RUN_AS_NODE=1，导致打包版以纯 node 运行即启即退。"""
    for key in ("ELECTRON_RUN_AS_NODE", "ELECTRON_NO_ATTACH_CONSOLE"):
        env.pop(key, None)


def rpc(port: int, payload: dict, timeout: float = 10.0) -> str:
    """控制口是行分隔 JSON（非 HTTP、非长度前缀）。"""
    s = socket.create_connection(("127.0.0.1", port), timeout=timeout)
    try:
        s.sendall((json.dumps(payload) + "\n").encode("utf-8"))
        s.settimeout(timeout)
        buf = b""
        while b"\n" not in buf:
            chunk = s.recv(65536)
            if not chunk:
                break
            buf += chunk
        return buf.decode("utf-8", "replace").strip()
    finally:
        s.close()


def wait_port_file(home: str, proc: subprocess.Popen, timeout: float):
    port_file = os.path.join(home, "mcp-port")
    deadline = time.time() + timeout
    while time.time() < deadline:
        time.sleep(0.5)
        if proc.poll() is not None:
            return None, f"进程提前退出（code={proc.returncode}）"
        if os.path.exists(port_file):
            try:
                text = open(port_file, encoding="utf-8").read().strip()
                if text.isdigit():
                    return int(text), None
            except OSError:
                pass
    return None, f"等待 mcp-port 超时（{timeout}s）"


def kill_tree(pid: int) -> None:
    try:
        subprocess.run(["taskkill", "/T", "/F", "/PID", str(pid)],
                       capture_output=True, timeout=30)
    except Exception as exc:  # noqa: BLE001 - 收尾失败不应掩盖冒烟结论
        log(f"[warn] taskkill 失败: {exc}")


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--exe", default=DEFAULT_EXE)
    ap.add_argument("--home", default=None, help="临时 home（默认 build/smoke-home）")
    ap.add_argument("--keep-home", action="store_true", help="保留临时 home 便于排查")
    ap.add_argument("--timeout", type=float, default=90.0)
    args = ap.parse_args()

    exe = os.path.abspath(args.exe)
    if not os.path.isfile(exe):
        log(f"[FAIL] 找不到可执行文件：{exe}")
        return 1

    home = os.path.abspath(args.home or os.path.join("build", "smoke-home"))
    if os.path.isdir(home):
        shutil.rmtree(home, ignore_errors=True)
    os.makedirs(home, exist_ok=True)

    env = dict(os.environ)
    strip_host_leaks(env)
    env["LUMINA_SMOKE"] = "1"
    env["LUMINA_HOME"] = home

    log_path = os.path.join(home, "smoke-stdout.log")
    log(f"[1/4] 启动 {exe}")
    log(f"      home={home}")
    with open(log_path, "w", encoding="utf-8", errors="replace") as logf:
        proc = subprocess.Popen([exe], env=env, cwd=os.path.dirname(exe),
                                stdout=logf, stderr=subprocess.STDOUT)
        failures = []
        try:
            port, err = wait_port_file(home, proc, args.timeout)
            if port is None:
                failures.append(f"控制口未就绪：{err}")
            else:
                log(f"[2/4] 控制口 {port} 就绪")

                status_raw = ""
                ok_status = False
                for attempt in range(8):
                    time.sleep(1.0)
                    try:
                        status_raw = rpc(port, {"id": 1, "op": "lumina_status"})
                    except OSError as exc:
                        status_raw = f"<连接失败 {exc}>"
                        continue
                    if '"ok":true' in status_raw.replace(" ", ""):
                        ok_status = True
                        break
                if ok_status:
                    compact = json.loads(status_raw).get("result", {})
                    engine = compact.get("engine", {}) if isinstance(compact, dict) else {}
                    log(f"[3/4] lumina_status ok · engine.mode={engine.get('mode')} "
                        f"version={engine.get('engineVersion')}")
                else:
                    failures.append(f"lumina_status 未成功：{status_raw[:300]}")

                try:
                    photos_raw = rpc(port, {"id": 2, "op": "list_photos", "params": {}})
                    if '"ok":true' in photos_raw.replace(" ", ""):
                        total = json.loads(photos_raw).get("result", {}).get("total")
                        log(f"      list_photos ok · total={total}")
                    else:
                        failures.append(f"list_photos 失败：{photos_raw[:300]}")
                except Exception as exc:  # noqa: BLE001
                    failures.append(f"list_photos 异常：{exc}")

                try:
                    unknown = rpc(port, {"id": 3, "op": "no_such_op"})
                    if '"ok":false' in unknown.replace(" ", ""):
                        log("      未知 op 返回标准错误信封（符合预期）")
                    else:
                        failures.append(f"未知 op 未按约定报错：{unknown[:300]}")
                except Exception as exc:  # noqa: BLE001
                    failures.append(f"未知 op 异常：{exc}")
        finally:
            log("[4/4] 收尾：结束进程树")
            kill_tree(proc.pid)
            time.sleep(1.0)

    tail = []
    try:
        with open(log_path, encoding="utf-8", errors="replace") as f:
            tail = f.read().splitlines()[-15:]
    except OSError:
        pass
    if tail:
        log("--- 应用输出尾部 ---")
        for line in tail:
            log("  " + line)

    if not args.keep_home:
        shutil.rmtree(home, ignore_errors=True)

    if failures:
        log("SMOKE FAIL")
        for item in failures:
            log("  - " + item)
        return 1

    log("SMOKE PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
