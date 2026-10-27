# -*- coding: utf-8 -*-
"""M4 冒烟：真引擎 RPC 协议下验证 softproof.preview 与 psd.inspect 可用且返回真实数值。

用法：python scripts/smoke_m4_rpc.py
不依赖 UI / Electron，只走 4 字节 LE 长度前缀 + JSON 帧。
"""
import json
import os
import shutil
import struct
import subprocess
import sys
import tempfile
import time

ENGINE = os.environ.get("LUMINA_ENGINE", r"D:/LUMINA/build/engine/lumina-engine.exe")
# 用 M4-02 测试产出的一张带 ICC 的 JPEG 作为输入（存在即用，否则用任意 png）
CANDIDATES = [
    r"D:/LUMINA/build/engine/m4-embed-sRGB.jpg",
    r"D:/LUMINA/build/engine/m4-embed-AdobeRGB.jpg",
]


def send_frame(proc, obj):
    payload = json.dumps(obj, separators=(",", ":")).encode("utf-8")
    proc.stdin.write(struct.pack("<I", len(payload)) + payload)
    proc.stdin.flush()


def read_frame(proc, timeout=60):
    deadline = time.time() + timeout

    def read_n(n, dl):
        buf = b""
        while len(buf) < n:
            if time.time() > dl:
                return None
            ch = proc.stdout.read(n - len(buf))
            if not ch:
                return None if not buf else buf
            buf += ch
        return buf

    hdr = read_n(4, deadline)
    if not hdr or len(hdr) < 4:
        return None
    (n,) = struct.unpack("<I", hdr)
    body = read_n(n, deadline)
    return json.loads(body) if body else None


def call(proc, rid, method, params):
    send_frame(proc, {"jsonrpc": "2.0", "id": rid, "method": method, "params": params})
    return read_frame(proc)


def main():
    if not os.path.exists(ENGINE):
        print("FAIL: 引擎不存在", ENGINE)
        return 1
    src = next((p for p in CANDIDATES if os.path.exists(p)), None)
    if not src:
        print("FAIL: 缺少输入样本，先跑 build/engine/test_icc_embed.exe")
        return 1

    home = tempfile.mkdtemp(prefix="lumina-m4-smoke-")
    proc = subprocess.Popen(
        [ENGINE, "--home", home],
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )
    failures = []
    try:
        r = call(proc, 1, "system.initialize", {})
        print("initialize:", "ok" if r and "result" in r else r)
        caps = (r or {}).get("result", {}).get("capabilities", {})
        print("  engineVersion:", (r or {}).get("result", {}).get("engineVersion"))
        print("  codecs:", caps.get("codecs"), "icc:", caps.get("icc"), "softProof:", caps.get("softProof"))
        if "psd" not in (caps.get("codecs") or []):
            failures.append("capabilities.codecs 未声明 psd（与实际能力不符）")
        if not caps.get("icc"):
            failures.append("capabilities.icc 为 false（lcms2 未接入）")

        r = call(proc, 2, "softproof.preview", {
            "inputPath": src, "longEdge": 800, "intent": 1, "simulatePaper": True,
        })
        if not r or "result" not in r:
            failures.append("softproof.preview 失败: %s" % (r,))
        else:
            res = r["result"]
            de = res.get("deltaE", {})
            print("softproof.preview: path=%s size=%sx%s target=%s" % (
                res.get("path"), res.get("width"), res.get("height"), res.get("target")))
            print("  ΔE00 mean=%.4f median=%.4f p95=%.4f max=%.4f samples=%s" % (
                de.get("mean", -1), de.get("median", -1), de.get("p95", -1), de.get("max", -1),
                res.get("samples")))
            if not res.get("path") or not os.path.exists(res["path"]):
                failures.append("softproof.preview 未产出 PNG")
            if res.get("samples", 0) <= 0:
                failures.append("softproof.preview 样本数为 0")

        r = call(proc, 3, "psd.inspect", {"path": r"D:/LUMINA/build/engine/m4-embed-sRGB.psd"})
        if r and "result" in r:
            info = r["result"]
            print("psd.inspect: %sx%s depth=%s layers=%s foreign=%s" % (
                info.get("width"), info.get("height"), info.get("depth"),
                info.get("layerCount"), info.get("foreign")))
            if info.get("layerCount", 0) < 1:
                failures.append("psd.inspect 层数异常")
        else:
            print("psd.inspect 跳过（样本缺失或有误）:", r)

        r = call(proc, 4, "system.shutdown", {})
        print("shutdown:", "ok" if r else r)
    finally:
        try:
            proc.wait(timeout=15)
        except Exception:
            proc.kill()
        shutil.rmtree(home, ignore_errors=True)

    if failures:
        print("\nM4 SMOKE FAIL:")
        for f in failures:
            print(" -", f)
        return 1
    print("\nM4 SMOKE PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
