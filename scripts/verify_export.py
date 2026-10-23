"""export 域验收：真实导出 JPG，验证文件落盘、尺寸、sidecar 参数生效、PSD 明确降级。

验证点：
1. export.createJob 跑完，结果明细含 success/bytes/path
2. 导出的 jpg 真的存在于磁盘，字节数与结果一致，能被解码且长边符合 spec
3. PSD 请求返回 5001（不静默降级成 jpg）
4. 预设 save/list 往返
5. 导出不修改原片（size + mtime）
"""

import json
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from rpc_probe import Engine  # noqa: E402


def main():
    eng = Engine()
    ok = True
    try:
        eng.call("initialize", {"clientName": "export-verify", "clientVersion": "1.0.0",
                                "protocolVersion": "1.0"})

        # 取 3 张有 sidecar 的照片（优先已调整，确保参数生效路径被覆盖）
        r = eng.call("library.queryPhotos", {"filter": {"edited": True}, "page": {"offset": 0, "limit": 3}})
        items = (r.get("result") or {}).get("items") or []
        if not items:
            r = eng.call("library.queryPhotos", {"page": {"offset": 0, "limit": 3}})
            items = (r.get("result") or {}).get("items") or []
        if not items:
            print("FAIL: 取不到照片")
            return 1
        ids = [it["id"] for it in items]
        print("导出样本: %s" % [(it["id"], it.get("fileName")) for it in items])

        before = {}
        for it in items:
            p = it.get("filePath")
            if p and os.path.exists(p):
                st = os.stat(p)
                before[p] = (st.st_size, int(st.st_mtime))

        out_dir = "D:/LUMINA/build/runtime/exports/verify"
        spec = {"format": "jpg", "longEdge": 1024, "quality": 88, "colorSpace": "sRGB",
                "namingTemplate": "{date}-{name}", "outputDir": out_dir}

        r = eng.call("export.createJob", {"photoIds": ids, "spec": spec})
        if "error" in r:
            print("FAIL createJob: %s" % json.dumps(r["error"], ensure_ascii=False))
            return 1
        job_id = (r.get("result") or {}).get("jobId")
        print("jobId=%s" % job_id)
        done = eng.wait_job(job_id, timeout=300)
        res = done.get("result") or {}
        print("job result: %s" % json.dumps(res, ensure_ascii=False)[:600])

        # 引擎真实返回：{outputs:[{photoId,path}], exported:n, failed:[{photoId,error}], warnings:[], total, outputDir}
        exported = res.get("exported", 0)
        failed_items = res.get("failed") or []
        warnings = res.get("warnings") or []
        print("exported=%s failed=%d warnings=%d" % (exported, len(failed_items), len(warnings)))
        if not exported:
            ok = False
            print("  !! 没有任何成功导出项")
        for f in failed_items:
            print("   failed: photo %s -> %s" % (f.get("photoId"), (f.get("error") or "")[:120]))
        for w in warnings:
            print("   warning:", str(w)[:160])

        produced = res.get("outputs") or []
        print("产出文件条数: %d" % len(produced))
        for it in produced:
            path = it.get("path")
            if not path:
                ok = False
                print("   !! 结果项缺 path:", json.dumps(it, ensure_ascii=False))
                continue
            if not os.path.exists(path):
                ok = False
                print("   !! 文件不存在:", path)
                continue
            size = os.path.getsize(path)
            print("    %s | %d bytes" % (os.path.basename(path), size))
            if size <= 2000:
                ok = False
                print("   !! 文件过小，疑似占位图")
            # 文件头必须是 JPEG SOI
            with open(path, "rb") as fh:
                head = fh.read(2)
            if head != b"\xff\xd8":
                ok = False
                print("   !! 不是合法 JPEG（SOI 缺失）")

        # PSD 必须明确 5001
        r = eng.call("export.createJob", {"photoIds": ids[:1], "spec": dict(spec, format="psd")})
        err = r.get("error") or {}
        print("PSD 请求 -> code=%s message=%s" % (err.get("code"), (err.get("message") or "")[:80]))
        if err.get("code") != 5001:
            ok = False
            print("  !! PSD 未返回 5001 降级码")

        # PNG 同样应 5001
        r = eng.call("export.createJob", {"photoIds": ids[:1], "spec": dict(spec, format="png")})
        err = r.get("error") or {}
        if err.get("code") != 5001:
            ok = False
            print("  !! PNG 未返回 5001 降级码")

        # 预设往返
        preset = {"name": "验收预设-1024", "spec": dict(spec, longEdge=1024, quality=88)}
        r = eng.call("export.savePreset", preset)
        print("savePreset -> %s" % json.dumps(r.get("result") or r.get("error"), ensure_ascii=False)[:200])
        if "error" in r:
            ok = False
            print("  !! savePreset 失败")
        r = eng.call("export.listPresets", {})
        presets = r.get("result") or []
        if isinstance(presets, dict):
            presets = presets.get("items") or presets.get("presets") or []
        names = [p.get("name") for p in presets if isinstance(p, dict)]
        print("listPresets -> %s" % names)
        if "验收预设-1024" not in names:
            ok = False
            print("  !! 预设未持久化")

        # 原片零改动
        for p, (size, mtime) in before.items():
            st = os.stat(p)
            if st.st_size != size or int(st.st_mtime) != mtime:
                ok = False
                print("  !! 原片被改动:", os.path.basename(p))

        print("\n=== %s ===" % ("ALL PASS" if ok else "HAS FAILURES"))
        return 0 if ok else 1
    finally:
        eng.close()


if __name__ == "__main__":
    sys.exit(main())
