# -*- coding: utf-8 -*-
"""Reproduce develop-page blank: call preview.getBase against the real app home."""
import json, os, queue, struct, subprocess, threading, time

ENGINE = r"D:/LUMINA/build/pkg/LUMINA/resources/build/engine/lumina-engine.exe"
HOME = r"C:/Users/zj/.lumina"

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

def rpc(rid, method, params, timeout=60):
    pl = json.dumps({"jsonrpc": "2.0", "id": rid, "method": method, "params": params},
                    separators=(",", ":")).encode()
    proc.stdin.write(struct.pack("<I", len(pl)) + pl)
    proc.stdin.flush()
    end = time.time() + timeout
    while time.time() < end:
        try:
            r = frames.get(timeout=1)
        except queue.Empty:
            continue
        if r.get("id") == rid:
            return r
    return None

print("ping:", bool(rpc(1, "system.ping", {})))
# 取几张不同类型（rw2/dng/jpg）的照片
r = rpc(2, "library.queryPhotos", {"limit": 5})
items = (r or {}).get("result", {}).get("items", [])
print("query ok:", bool(items), "count:", len(items))
for it in items[:4]:
    pid = it.get("id")
    print("photo", pid, it.get("fileName"), it.get("format"))
    g = rpc(100 + (pid or 0), "preview.getBase", {"id": pid})
    if g and "result" in g:
        res = g["result"]
        p = res.get("path", "")
        print("   preview OK:", res.get("width"), "x", res.get("height"),
              "source=", res.get("source"), "path=", p,
              "exists=", os.path.exists(p), "bytes=", os.path.getsize(p) if os.path.exists(p) else -1)
    else:
        print("   preview FAIL:", json.dumps(g, ensure_ascii=False)[:300])

rpc(999, "system.shutdown", {})
try:
    proc.wait(timeout=10)
    print("engine exit:", proc.returncode)
except subprocess.TimeoutExpired:
    proc.kill()
