"""迷你 mock 库快速验证：确认当前代码写出的 sidecar 不含裸文本污染。

用一个独立的 home（不碰主库），只生成 60 张 / 12 张已调整，
然后逐字节检查每个 sidecar 的 rdf:Description 内容层有没有裸文本。
"""

import json
import os
import shutil
import struct
import subprocess
import sys
import time

EXE = "D:/LUMINA/build/engine/lumina-engine.exe"
MINGW_BIN = "D:/work/tools/mingw_posix/mingw64/bin"
HOME = "D:/LUMINA/build/runtime-mini"
LIB = os.path.join(HOME, "mocklib")


class Eng:
    def __init__(self, home):
        env = dict(os.environ)
        env["PATH"] = MINGW_BIN + ";" + env.get("PATH", "")
        os.makedirs(os.path.join(home, "_probe"), exist_ok=True)
        self.p = subprocess.Popen(
            [EXE, "--home", home],
            stdin=subprocess.PIPE, stdout=subprocess.PIPE,
            stderr=open(os.path.join(home, "_probe", "stderr.log"), "ab"),
            env=env, bufsize=0)
        self.n = 1

    def _write(self, payload):
        frame = struct.pack("<I", len(payload)) + payload
        v = memoryview(frame)
        while v:
            w = self.p.stdin.write(v)
            if not w:
                raise RuntimeError("stdin closed")
            v = v[w:]
        self.p.stdin.flush()

    def _readn(self, n):
        buf = b""
        while len(buf) < n:
            c = self.p.stdout.read(n - len(buf))
            if not c:
                raise RuntimeError("eof")
            buf += c
        return buf

    def _msg(self):
        (ln,) = struct.unpack("<I", self._readn(4))
        return json.loads(self._readn(ln).decode("utf-8"))

    def call(self, method, params=None, timeout=1200):
        mid = self.n
        self.n += 1
        self._write(json.dumps({"jsonrpc": "2.0", "id": mid, "method": method,
                                "params": params or {}}, ensure_ascii=False).encode("utf-8"))
        deadline = time.time() + timeout
        while time.time() < deadline:
            m = self._msg()
            if "method" in m and "id" not in m:
                if m["method"] == "event.job.completed":
                    self.last_job = m.get("params") or {}
                continue
            if m.get("id") == mid:
                return m
        raise RuntimeError("timeout " + method)

    def close(self):
        try:
            self.p.stdin.close()
        except Exception:
            pass
        try:
            self.p.wait(timeout=10)
        except Exception:
            self.p.kill()

    def wait_job(self, timeout=1200):
        """mock.generate 是异步 job：必须等 event.job.completed，否则进程被关掉会中断生成。"""
        deadline = time.time() + timeout
        while time.time() < deadline:
            m = self._msg()
            if m.get("method") == "event.job.completed":
                return m.get("params") or {}
        raise RuntimeError("timeout waiting job completion")


def bare_text(path):
    s = open(path, "rb").read().decode("utf-8", "replace")
    m = s.find("<rdf:Description")
    if m < 0:
        return "no-description"
    st = s.find(">", m)
    en = s.find("</rdf:Description>", st)
    if st < 0 or en < 0:
        return "unterminated"
    body = s[st + 1:en]
    depth, pending, i = 0, [], 0
    while i < len(body):
        c = body[i]
        if c == "<":
            if depth == 0 and "".join(pending).strip():
                return "".join(pending).strip()[:60]
            pending = []
            e = body.find(">", i)
            if e < 0:
                break
            closing = i + 1 < len(body) and body[i + 1] == "/"
            selfc = e > i and body[e - 1] == "/"
            if closing:
                depth = max(0, depth - 1)
            elif not selfc:
                depth += 1
            i = e + 1
        else:
            pending.append(c)
            i += 1
    if depth == 0 and "".join(pending).strip():
        return "".join(pending).strip()[:60]
    return None


def main():
    if os.path.isdir(HOME):
        shutil.rmtree(HOME)
    os.makedirs(HOME, exist_ok=True)

    eng = Eng(HOME)
    try:
        eng.call("initialize", {"clientName": "mini-mock", "clientVersion": "1.0.0",
                                "protocolVersion": "1.0"})
        r = eng.call("mock.generate", {"total": 60, "edited": 12, "missing": 2,
                                       "gps": 8, "pairCount": 4, "rootCount": 2,
                                       "folderCount": 6, "libraryRoot": LIB})
        if "error" in r:
            print("generate error:", json.dumps(r["error"], ensure_ascii=False))
            return 1
        job = eng.wait_job()
        print("job:", json.dumps(job.get("result"), ensure_ascii=False))
    finally:
        eng.close()

    xmps = []
    for dp, _, fs in os.walk(LIB):
        for f in fs:
            if f.lower().endswith(".xmp"):
                xmps.append(os.path.join(dp, f))
    print("\nsidecar count:", len(xmps))

    bad = []
    for p in xmps:
        b = bare_text(p)
        if b:
            bad.append((p, b))
    print("裸文本污染:", len(bad))
    for p, b in bad[:6]:
        print("  ", os.path.basename(p), "|", b)

    if xmps:
        sample = xmps[0]
        print("\n--- 样本 %s ---" % os.path.basename(sample))
        print(open(sample, "rb").read().decode("utf-8", "replace"))
    return 0 if (xmps and not bad) else 1


if __name__ == "__main__":
    sys.exit(main())
