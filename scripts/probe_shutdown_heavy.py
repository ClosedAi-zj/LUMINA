# -*- coding: utf-8 -*-
"""Reproduce: shutdown hang when thread pool queue is busy (thumb decode backlog)."""
import json, os, struct, subprocess, sys, time, shutil

ENGINE = r"D:/LUMINA/build/engine/lumina-engine.exe"
HOME = r"D:/LUMINA/build/runtime-shutdown-heavy"

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
                return None
            buf += ch
        return buf
    hdr = read_n(4, deadline)
    if not hdr or len(hdr) < 4:
        return None
    (n,) = struct.unpack("<I", hdr)
    body = read_n(n, deadline)
    return json.loads(body) if body else None

def rpc(proc, rid, method, params, timeout=60):
    send_frame(proc, {"jsonrpc": "2.0", "id": rid, "method": method, "params": params})
    return read_frame(proc, timeout)

def main():
    if os.path.exists(HOME):
        shutil.rmtree(HOME, ignore_errors=True)
    os.makedirs(HOME, exist_ok=True)

    proc = subprocess.Popen([ENGINE, "--home", HOME],
                            stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                            stderr=subprocess.PIPE)
    r = rpc(proc, 1, "system.ping", {})
    print("ping ok:", bool(r), flush=True)

    # 1. generate a small mock library synchronously via job polling
    r = rpc(proc, 2, "mock.generate", {"spec": {"total": 60, "rootCount": 1,
                                                "folderCount": 3, "edited": 10,
                                                "gps": 40, "libraryRoot": HOME}})
    print("mock.generate job:", r.get("result", {}) if r else None, flush=True)
    for _ in range(120):
        s = rpc(proc, 0, "mock.status", {}, timeout=10)
        st = (s or {}).get("result", {})
        if st.get("state") in ("done", "completed", "idle", "ready") or st.get("done"):
            break
        time.sleep(0.5)
    print("mock status:", st, flush=True)

    # 2. fire a heavy thumbnail batch (RAW decode per photo) then shutdown IMMEDIATELY
    ids = list(range(1, 61))
    send_frame(proc, {"jsonrpc": "2.0", "id": 3, "method": "thumb.ensureBatch",
                      "params": {"ids": ids, "size": 320}})
    t0 = time.time()
    send_frame(proc, {"jsonrpc": "2.0", "id": 4, "method": "system.shutdown", "params": {}})
    r2 = read_frame(proc, timeout=20)  # shutdown reply itself should be fast
    t_reply = time.time() - t0
    print("shutdown reply after %.1fs" % t_reply, flush=True)

    try:
        proc.wait(timeout=60)
        print("PROCESS EXITED, code: %s, total wait %.1fs" % (proc.returncode, time.time() - t0), flush=True)
    except subprocess.TimeoutExpired:
        print("PROCESS STILL ALIVE after 60s  <-- HANG REPRODUCED", flush=True)
        proc.kill()
    # drain pending thumb reply if any
    try:
        proc.stdin.close()
    except Exception:
        pass

if __name__ == "__main__":
    main()
