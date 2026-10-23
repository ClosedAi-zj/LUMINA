# -*- coding: utf-8 -*-
"""Reproduce: does the engine process exit after system.shutdown?"""
import json, os, struct, subprocess, sys, time, shutil

ENGINE = r"D:/LUMINA/build/engine/lumina-engine.exe"
HOME = r"D:/LUMINA/build/runtime-shutdown-test"

def send_frame(proc, obj):
    payload = json.dumps(obj, separators=(",", ":")).encode("utf-8")
    proc.stdin.write(struct.pack("<I", len(payload)) + payload)
    proc.stdin.flush()

def read_frame(proc, timeout=15):
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

def main():
    if os.path.exists(HOME):
        shutil.rmtree(HOME, ignore_errors=True)
    os.makedirs(HOME, exist_ok=True)
    lock_path = os.path.join(HOME, "engine.lock")

    proc = subprocess.Popen([ENGINE, "--home", HOME],
                            stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                            stderr=subprocess.PIPE)
    started = time.time()
    send_frame(proc, {"jsonrpc": "2.0", "id": 1, "method": "system.ping", "params": {}})
    r = read_frame(proc)
    print("ping reply ok:", bool(r), flush=True)

    lock_exists_during = os.path.exists(lock_path)
    print("engine.lock exists while running:", lock_exists_during, flush=True)

    send_frame(proc, {"jsonrpc": "2.0", "id": 2, "method": "system.shutdown", "params": {}})
    r2 = read_frame(proc, timeout=15)
    print("shutdown reply ok:", bool(r2), flush=True)
    print("stdin/stdout left open on purpose; waiting for process exit...", flush=True)

    try:
        proc.wait(timeout=25)
        print("PROCESS EXITED, code:", proc.returncode, flush=True)
    except subprocess.TimeoutExpired:
        print("PROCESS STILL ALIVE after 25s  <-- BUG REPRODUCED", flush=True)
        print("engine.lock still exists:", os.path.exists(lock_path), flush=True)

    print("elapsed since shutdown reply: %.1fs" % (time.time() - started), flush=True)
    if proc.poll() is None:
        proc.kill()
    # dump last log lines
    logs = os.path.join(HOME, "logs")
    if os.path.isdir(logs):
        for name in sorted(os.listdir(logs)):
            p = os.path.join(logs, name)
            if os.path.isfile(p):
                print("---- log tail:", name, flush=True)
                with open(p, "rb") as f:
                    data = f.read()[-4000:]
                print(data.decode("utf-8", "replace"), flush=True)

if __name__ == "__main__":
    main()
