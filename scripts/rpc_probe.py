"""LUMINA engine stdio JSON-RPC probe.

Frame format: 4-byte little-endian length prefix + UTF-8 JSON payload.

Usage:
  python scripts/rpc_probe.py call  <method> ['{json params}']
  python scripts/rpc_probe.py job   <method> ['{json params}']
  python scripts/rpc_probe.py batch ['[{"method":..,"params":..}, ...]']

Notes:
  - `read` loops until the requested byte count is satisfied. A single
    pipe read() may return a short chunk; treating that as "truncated" was
    a false-failure source during earlier verification.
  - Engine notification frames (event.*) arrive interleaved with responses,
    so every read dispatch checks method/id instead of assuming ordering.
"""

import json
import os
import struct
import subprocess
import sys
import time

EXE = "D:/LUMINA/build/engine/lumina-engine.exe"
HOME = "D:/LUMINA/build/runtime"
MINGW_BIN = "D:/work/tools/mingw_posix/mingw64/bin"


class Engine:
    def __init__(self, exe=EXE, home=HOME, quiet_events=True):
        env = dict(os.environ)
        env["PATH"] = MINGW_BIN + ";" + env.get("PATH", "")
        self.proc = subprocess.Popen(
            [exe, "--home", home],
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=open(os.path.join(HOME, "_probe", "stderr.log"), "ab"),
            env=env,
            bufsize=0,
        )
        self.next_id = 1
        self.quiet_events = quiet_events
        self.events = []

    # ---------- framing ----------
    def _write_frame(self, payload: bytes):
        # bufsize=0 makes stdin an unbuffered FileIO: write() may accept only a
        # prefix, so every write must be looped. A short write here leaves the
        # engine waiting on a half frame and looks exactly like "engine hang".
        frame = struct.pack("<I", len(payload)) + payload
        view = memoryview(frame)
        while view:
            written = self.proc.stdin.write(view)
            if not written:
                raise RuntimeError("engine stdin closed while writing frame")
            view = view[written:]
        self.proc.stdin.flush()

    def _read_exactly(self, n):
        buf = b""
        while len(buf) < n:
            chunk = self.proc.stdout.read(n - len(buf))
            if not chunk:
                raise RuntimeError("engine closed stdout (eof) while reading %d bytes" % n)
            buf += chunk
        return buf

    def _read_message(self):
        header = self._read_exactly(4)
        (length,) = struct.unpack("<I", header)
        if length == 0 or length > 8 * 1024 * 1024:
            raise RuntimeError("invalid frame length: %d" % length)
        return json.loads(self._read_exactly(length).decode("utf-8"))

    # ---------- rpc ----------
    def send(self, method, params=None):
        mid = self.next_id
        self.next_id += 1
        body = {"jsonrpc": "2.0", "id": mid, "method": method, "params": params or {}}
        self._write_frame(json.dumps(body, ensure_ascii=False).encode("utf-8"))
        return mid

    def send_no_reply(self, method, params=None):
        body = {"jsonrpc": "2.0", "method": method, "params": params or {}}
        self._write_frame(json.dumps(body, ensure_ascii=False).encode("utf-8"))

    def wait_response(self, mid, timeout=60.0):
        deadline = time.time() + timeout
        while time.time() < deadline:
            msg = self._read_message()
            if "method" in msg and "id" not in msg:
                self.events.append(msg)
                if not self.quiet_events:
                    print("[event] %s %s" % (msg["method"], json.dumps(msg.get("params"), ensure_ascii=False)[:180]))
                continue
            if msg.get("id") == mid:
                return msg
        raise RuntimeError("timeout waiting for response id=%s" % mid)

    def call(self, method, params=None, timeout=60.0):
        mid = self.send(method, params)
        return self.wait_response(mid, timeout)

    def wait_job(self, job_id, timeout=600.0):
        """Wait for event.job.completed matching job_id (already-sent jobs)."""
        deadline = time.time() + timeout
        while time.time() < deadline:
            msg = self._read_message()
            if "method" not in msg:
                continue
            self.events.append(msg)
            if msg["method"] == "event.job.completed":
                params = msg.get("params") or {}
                if str(params.get("jobId")) == str(job_id):
                    return params
            elif not self.quiet_events:
                print("[event] %s" % msg["method"])
        raise RuntimeError("timeout waiting for event.job.completed jobId=%s" % job_id)

    def run_job(self, method, params=None, timeout=600.0):
        resp = self.call(method, params, timeout=30)
        if "error" in resp:
            return resp
        job_id = (resp.get("result") or {}).get("jobId")
        if not job_id:
            return resp
        completed = self.wait_job(job_id, timeout)
        return {"result": completed.get("result"), "jobId": job_id}

    def close(self):
        try:
            self.proc.stdin.close()
        except Exception:
            pass
        try:
            self.proc.wait(timeout=5)
        except Exception:
            self.proc.kill()


def main(argv):
    if len(argv) < 3:
        print(__doc__)
        return 2
    mode, method = argv[1], argv[2]
    params = json.loads(argv[3]) if len(argv) > 3 else {}
    eng = Engine()
    out = None
    try:
        eng.call("system.initialize", {"clientName": "rpc-probe", "clientVersion": "1.0.0", "protocolVersion": "1.0"})
        if mode == "call":
            out = eng.call(method, params, timeout=180)
        elif mode == "job":
            out = eng.run_job(method, params, timeout=900)
        elif mode == "batch":
            out = [eng.call(m, p, timeout=180) for m, p in [(x["method"], x.get("params", {})) for x in params]]
        else:
            raise SystemExit("unknown mode: " + mode)
    finally:
        print(json.dumps(out, ensure_ascii=False, indent=2))
        eng.close()
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
