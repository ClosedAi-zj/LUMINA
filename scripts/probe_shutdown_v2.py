# -*- coding: utf-8 -*-
"""Shutdown probe v2: drain stdout (frame queue) + stderr so the probe itself
cannot deadlock the engine on full OS pipes (which masked real behavior)."""
import json, os, queue, struct, subprocess, sys, threading, time, shutil

ENGINE = os.environ.get("LUMINA_ENGINE", r"D:/LUMINA/build/engine/lumina-engine.exe")
HOME = r"D:/LUMINA/build/runtime-shutdown-v2"

def main():
    if os.path.exists(HOME):
        shutil.rmtree(HOME, ignore_errors=True)
    os.makedirs(HOME, exist_ok=True)

    proc = subprocess.Popen([ENGINE, "--home", HOME],
                            stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                            stderr=subprocess.PIPE)

    # stderr: pure drain (engine logs every line to stderr)
    threading.Thread(target=lambda: proc.stderr.read(), daemon=True).start()

    # stdout: frame reader thread -> queue
    frames = queue.Queue()
    def reader():
        f = proc.stdout
        try:
            while True:
                hdr = b""
                while len(hdr) < 4:
                    ch = f.read(4 - len(hdr))
                    if not ch:
                        return
                    hdr += ch
                (n,) = struct.unpack("<I", hdr)
                body = b""
                while len(body) < n:
                    ch = f.read(n - len(body))
                    if not ch:
                        return
                    body += ch
                frames.put(json.loads(body))
        except Exception:
            return
    threading.Thread(target=reader, daemon=True).start()

    def rpc(rid, method, params, timeout=30, want_reply=True):
        payload = json.dumps({"jsonrpc": "2.0", "id": rid, "method": method,
                              "params": params}, separators=(",", ":")).encode("utf-8")
        proc.stdin.write(struct.pack("<I", len(payload)) + payload)
        proc.stdin.flush()
        if not want_reply:
            return None
        deadline = time.time() + timeout
        while time.time() < deadline:
            try:
                r = frames.get(timeout=1)
            except queue.Empty:
                continue
            if r.get("id") == rid:
                return r
            # discard notifications / other replies
        return None

    r = rpc(1, "system.ping", {})
    print("ping ok:", bool(r), flush=True)

    # generate mock library, drain until reply (notifications discarded in rpc)
    rpc(2, "mock.generate", {"spec": {"total": 60, "rootCount": 1, "folderCount": 3,
                                      "edited": 10, "gps": 40, "libraryRoot": HOME}})
    # wait for generation to actually finish via job event / poll mock.status
    for _ in range(60):
        s = rpc(0, "mock.status", {}, timeout=10)
        if s and s.get("result", {}).get("generated"):
            break
        time.sleep(0.3)
    print("mock ready:", bool(s and s.get("result", {}).get("generated")), flush=True)
    time.sleep(0.5)

    t0 = time.time()
    # heavy batch (60 thumb tasks queued) + immediate shutdown
    rpc(3, "thumb.ensureBatch", {"ids": list(range(1, 61)), "size": 320}, want_reply=False)
    r4 = rpc(4, "system.shutdown", {}, timeout=20)
    t_reply = time.time() - t0
    print("shutdown reply: %s after %.2fs" % (bool(r4), t_reply), flush=True)

    try:
        proc.wait(timeout=45)
        print("PROCESS EXITED, code=%s, %.1fs after shutdown send" %
              (proc.returncode, time.time() - t0), flush=True)
    except subprocess.TimeoutExpired:
        print("PROCESS STILL ALIVE 45s after shutdown reply  <-- REAL HANG", flush=True)
        proc.kill()

if __name__ == "__main__":
    main()
