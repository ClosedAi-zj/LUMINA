# -*- coding: utf-8 -*-
"""发布门槛 · 原片零改写回归（产品红线 2）：
走完整编辑+导出流程后，原片字节必须逐字节不变（md5/size/mtime 三重校验），
同时 sidecar 必须真实写入编辑参数（证明流程真的发生了，不是空跑）。
"""
import hashlib, json, os, queue, struct, subprocess, threading, time, shutil

ENGINE = r"D:/LUMINA/build/engine/lumina-engine.exe"
HOME = r"D:/LUMINA/build/runtime-gate-zero"
LIB = os.path.join(HOME, "mocklib")

def md5(path):
    h = hashlib.md5()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()

def snapshot():
    out = {}
    for root, _, files in os.walk(LIB):
        for name in files:
            p = os.path.join(root, name)
            if name.lower().endswith(".xmp") or name == "README.md":
                continue
            st = os.stat(p)
            out[p] = (md5(p), st.st_size, int(st.st_mtime))
    return out

def main():
    if os.path.exists(HOME):
        shutil.rmtree(HOME, ignore_errors=True)
    os.makedirs(HOME, exist_ok=True)

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

    def rpc(rid, method, params=None, timeout=180):
        pl = json.dumps({"jsonrpc": "2.0", "id": rid, "method": method,
                         "params": params or {}}, separators=(",", ":")).encode()
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
        raise RuntimeError("timeout: " + method)

    # 1) 生成带真实影像文件的 40 张库（missing=0 保证文件在位）
    rpc(1, "system.ping", {})
    rpc(2, "mock.generate", {"spec": {"total": 40, "rootCount": 1, "folderCount": 2,
                                      "missing": 0, "edited": 0, "gps": 10,
                                      "libraryRoot": LIB}})
    for _ in range(120):
        s = rpc(900, "mock.status", {}, timeout=20)
        if s.get("result", {}).get("generated"):
            break
        time.sleep(0.3)
    file_count = sum(1 for r, _, fs in os.walk(LIB) for f in fs if not f.endswith(".xmp"))
    print("[zero] library files:", file_count, flush=True)

    before = snapshot()
    print("[zero] baseline originals:", len(before), flush=True)
    if not before:
        print("VERDICT: FAIL (no originals to check)")
        return

    # 2) 走完整编辑流程：查询 → 会话 → 改参 → 落盘 → 导出
    q = rpc(3, "library.queryPhotos", {"limit": 10})
    ids = [it["id"] for it in q.get("result", {}).get("items", [])][:6]
    print("[zero] editing photos:", ids, flush=True)

    session_ids = []
    mtimes = []
    for pid in ids:
        sess = rpc(1000 + pid, "edit.beginSession", {"id": pid})
        if "error" in sess:
            print("[zero] beginSession error", pid, sess["error"])
            continue
        sid = (sess.get("result") or {}).get("sessionId")
        if sid is None:
            print("[zero] beginSession no sessionId", pid, sess)
            continue
        session_ids.append((pid, sid))
        # 协议 §5.4：字段名是 patch（不是 params），写错会导致空补丁 → 引擎无变更不落盘
        sp = rpc(2000 + pid, "edit.setParams", {
            "sessionId": sid,
            "patch": {"exposure": 0.75, "contrast": 12, "highlights": -15,
                      "shadows": 20, "temperature": 5600, "vibrance": 10},
            "commitHistory": True,
        })
        if "error" in sp:
            print("[zero] setParams error", pid, sp["error"])
        end = rpc(3000 + pid, "edit.endSession", {"sessionId": sid, "flush": True})
        if "error" in end:
            print("[zero] endSession error", pid, end["error"])
        else:
            res = end.get("result") or {}
            mtimes.append((pid, res.get("flushed"), res.get("sidecarMtime")))
    print("[zero] sessions flushed:", len(session_ids), flush=True)
    if not session_ids:
        print("VERDICT: FAIL (no edit session ran)")
        return

    # 导出一次（走解码+编码管线，最容易误伤原片）
    exp = rpc(20, "export.createJob", {
        "photoIds": [p for p, _ in session_ids[:3]],
        "spec": {"format": "jpg", "longEdge": 1600, "colorSpace": "sRGB",
                 "quality": 90, "outputDir": os.path.join(HOME, "exported")},
    })
    if "error" in exp:
        print("[zero] export createJob error:", exp["error"], flush=True)
    else:
        job_id = exp.get("result", {}).get("jobId")
        for _ in range(60):
            st = rpc(21, "jobs.getStatus", {"jobId": job_id}, timeout=20)
            state = (st.get("result") or {}).get("state")
            if state in ("done", "completed", "failed", "cancelled"):
                print("[zero] export state:", state, flush=True)
                break
            time.sleep(0.4)

    # 3) 校验：原片逐字节不变 + sidecar 真的写了
    after = snapshot()
    changed = [p for p in before if after.get(p) != before[p]]
    new_files = [p for p in after if p not in before]
    missing = [p for p in before if p not in after]

    print("[zero] endSession sidecarMtime:", mtimes, flush=True)
    on_disk = 0
    for root, _, files in os.walk(LIB):
        on_disk += sum(1 for f in files if f.lower().endswith(".xmp"))
    print("[zero] sidecar files on disk:", on_disk, flush=True)

    sidecar_ok = 0
    for pid in ids:
        d = rpc(4000 + pid, "library.getPhotoDetail", {"id": pid})
        sc = (d.get("result") or {}).get("sidecarPath")
        if sc and os.path.exists(sc):
            text = open(sc, "r", encoding="utf-8", errors="replace").read()
            if "crs:Exposure2012" in text or "crs:Contrast2012" in text:
                sidecar_ok += 1

    print(f"[zero] originals changed={len(changed)} new={len(new_files)} missing={len(missing)}", flush=True)
    for p in changed[:5]:
        print("   CHANGED:", p, flush=True)
    print(f"[zero] sidecar with crs params: {sidecar_ok}/{len(ids)}", flush=True)

    ok = (not changed) and (not missing) and sidecar_ok >= max(1, len(ids) - 1)
    print("VERDICT:", "PASS" if ok else "FAIL", flush=True)

    rpc(999, "system.shutdown", {})
    try:
        proc.wait(timeout=15)
    except subprocess.TimeoutExpired:
        proc.kill()

if __name__ == "__main__":
    main()
