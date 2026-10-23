# -*- coding: utf-8 -*-
"""Verify map.getLocations name dedupe: same location name across grids must
yield ONE cluster item (unique locationId), with merged count/timeSpan."""
import json, os, queue, struct, subprocess, threading, time, shutil

ENGINE = os.environ.get("LUMINA_ENGINE", r"D:/LUMINA/build/engine/lumina-engine.exe")
HOME = r"D:/LUMINA/build/runtime-map-dedupe"

def main():
    if os.path.exists(HOME):
        shutil.rmtree(HOME, ignore_errors=True)
    os.makedirs(HOME, exist_ok=True)
    proc = subprocess.Popen([ENGINE, "--home", HOME],
                            stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                            stderr=subprocess.PIPE)
    threading.Thread(target=lambda: proc.stderr.read(), daemon=True).start()
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

    def rpc(rid, method, params, timeout=30):
        payload = json.dumps({"jsonrpc": "2.0", "id": rid, "method": method,
                              "params": params}, separators=(",", ":")).encode("utf-8")
        proc.stdin.write(struct.pack("<I", len(payload)) + payload)
        proc.stdin.flush()
        deadline = time.time() + timeout
        while time.time() < deadline:
            try:
                r = frames.get(timeout=1)
            except queue.Empty:
                continue
            if r.get("id") == rid:
                return r
        raise RuntimeError("rpc timeout: " + method)

    rpc(1, "system.ping", {})
    rpc(2, "mock.generate", {"spec": {"total": 30, "rootCount": 1, "folderCount": 2,
                                      "gps": 0, "edited": 0, "missing": 0, "libraryRoot": HOME}})
    for _ in range(60):
        s = rpc(0, "mock.status", {}, timeout=10)
        if s.get("result", {}).get("generated"):
            break
        time.sleep(0.3)
    print("mock ready", flush=True)

    # same name on two photos in grid A (30.01,100.01) and two in grid B (30.15,100.15)
    r = rpc(3, "map.setPhotoLocation", {"ids": [1, 2], "lat": 30.01, "lng": 100.01,
                                        "locationName": "雾浓顶"})
    print("set A ok:", "error" not in r, r.get("result", {}).get("updated"), flush=True)
    r = rpc(4, "map.setPhotoLocation", {"ids": [3, 4], "lat": 30.15, "lng": 100.15,
                                        "locationName": "雾浓顶"})
    print("set B ok:", "error" not in r, r.get("result", {}).get("updated"), flush=True)

    ok = True
    for zoom in (6, 8, 12):
        r = rpc(10 + zoom, "map.getLocations", {"zoom": zoom})
        items = r.get("result", [])
        names = [i.get("name") for i in items]
        ids_ = [i.get("locationId") for i in items]
        wucd = [i for i in items if i.get("name") == "雾浓顶"]
        unique_ids = len(ids_) == len(set(ids_))
        one_wucd = len(wucd) == 1 and wucd[0].get("count") == 4
        print("zoom=%d items=%d names=%s ids=%s uniqueIds=%s wucdCount=%s" %
              (zoom, len(items), names, ids_, unique_ids,
               wucd and wucd[0].get("count")), flush=True)
        if not unique_ids or not one_wucd:
            ok = False

    print("MAP DEDUPE:", "PASS" if ok else "FAIL", flush=True)
    rpc(99, "system.shutdown", {})
    try:
        proc.wait(timeout=15)
        print("engine exited code", proc.returncode, flush=True)
    except subprocess.TimeoutExpired:
        proc.kill()
        print("engine hung", flush=True)

if __name__ == "__main__":
    main()
