# -*- coding: utf-8 -*-
"""Heavy shutdown probe variant: on hang, attach gdb and dump all thread stacks."""
import json, os, struct, subprocess, sys, time, shutil

ENGINE = os.environ.get("LUMINA_ENGINE", r"D:/LUMINA/build/engine/lumina-engine.exe")
HOME = r"D:/LUMINA/build/runtime-shutdown-hang"
GDB = r"D:/work/tools/mingw_posix/mingw64/bin/gdb.exe"

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

def main():
    if os.path.exists(HOME):
        shutil.rmtree(HOME, ignore_errors=True)
    os.makedirs(HOME, exist_ok=True)

    proc = subprocess.Popen([ENGINE, "--home", HOME],
                            stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                            stderr=subprocess.PIPE)
    send_frame(proc, {"jsonrpc": "2.0", "id": 1, "method": "system.ping", "params": {}})
    read_frame(proc)
    print("ping ok", flush=True)

    # skip mock.status polling: just wait for generation to finish by job event drain
    send_frame(proc, {"jsonrpc": "2.0", "id": 2, "method": "mock.generate",
                      "params": {"spec": {"total": 60, "rootCount": 1, "folderCount": 3,
                                          "edited": 10, "gps": 40, "libraryRoot": HOME}}})
    # drain frames until we see id=2 reply (notifications come and are discarded)
    for _ in range(500):
        r = read_frame(proc, timeout=30)
        if r is None:
            break
        if r.get("id") == 2:
            print("mock.generate reply", flush=True)
            break
    time.sleep(2)  # let job finish

    # heavy thumb batch + immediate shutdown
    ids = list(range(1, 61))
    send_frame(proc, {"jsonrpc": "2.0", "id": 3, "method": "thumb.ensureBatch",
                      "params": {"ids": ids, "size": 320}})
    send_frame(proc, {"jsonrpc": "2.0", "id": 4, "method": "system.shutdown", "params": {}})
    t0 = time.time()
    r2 = read_frame(proc, timeout=20)
    print("first reply after shutdown send: %.2fs (id=%s)" % (time.time() - t0, r2 and r2.get("id")), flush=True)

    try:
        proc.wait(timeout=30)
        print("PROCESS EXITED, code=%s, %.1fs" % (proc.returncode, time.time() - t0), flush=True)
        return
    except subprocess.TimeoutExpired:
        print("HUNG. attaching gdb...", flush=True)

    pid = proc.pid
    cmds = "thread apply all bt 25\n detach\n quit\n"
    g = subprocess.run([GDB, "-p", str(pid), "-batch", "-ex", "thread apply all bt 25"],
                       capture_output=True, text=True, timeout=120)
    out = g.stdout + g.stderr
    for line in out.splitlines():
        s = line.strip()
        if s.startswith("#") or "Thread" in s or "Current thread" in s:
            print(s[:220], flush=True)
    proc.kill()

if __name__ == "__main__":
    main()
