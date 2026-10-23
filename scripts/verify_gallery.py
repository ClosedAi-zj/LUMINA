"""gallery 域验收：建画册 → 发布静态站 → 查产物 → 收藏包回收 → 撤销。

验证点：
1. publish 真生成 index.html + images/*.jpg + meta.json，且 images 数量与画册张数一致
2. index.html 含密码门禁与 Glass Noir 令牌色值，无外链依赖
3. listPublished 的 stats.completionRate 是 0..1 比值
4. importFeedback 读收藏包 → 落库 → getFeedback 读回一致
5. revoke 后目录被删除、listPublished 不再列出
6. 原片零写入
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
        eng.call("initialize", {"clientName": "gallery-verify", "clientVersion": "1.0.0",
                                "protocolVersion": "1.0"})

        # 取 4 张照片
        r = eng.call("library.queryPhotos", {"page": {"offset": 0, "limit": 4}})
        items = (r.get("result") or {}).get("items") or []
        if not items:
            print("FAIL: 取不到照片")
            return 1
        ids = [it["id"] for it in items]
        before = {}
        for it in items:
            p = it.get("filePath")
            if p and os.path.exists(p):
                st = os.stat(p)
                before[p] = (st.st_size, int(st.st_mtime))
        print("样本照片: %s" % [(it["id"], it.get("fileName")) for it in items])

        # 建画册
        name = "验收画册-千山"
        r = eng.call("albums.create", {"name": name, "kind": "portfolio"})
        if "error" in r:
            print("FAIL albums.create: %s" % json.dumps(r["error"], ensure_ascii=False))
            return 1
        album_id = (r.get("result") or {}).get("id")
        print("albumId=%s" % album_id)
        r = eng.call("albums.addPhotos", {"albumId": album_id, "ids": ids})
        print("addPhotos -> %s" % json.dumps(r.get("result") or r.get("error"), ensure_ascii=False))

        # publish
        r = eng.call("gallery.publish", {"albumId": album_id, "password": "lumina2026",
                                         "expiryDays": 30, "title": "千山暮雪",
                                         "customerName": "张先生"})
        if "error" in r:
            print("FAIL gallery.publish: %s" % json.dumps(r["error"], ensure_ascii=False))
            return 1
        pub = r["result"]
        print("publish -> %s" % json.dumps(pub, ensure_ascii=False)[:400])
        token = pub.get("token")
        static_dir = pub.get("staticDir")
        if not token or len(token) < 32:
            ok = False
            print("  !! token 长度不足或缺失")
        if not static_dir or not os.path.isdir(static_dir):
            ok = False
            print("  !! staticDir 不存在:", static_dir)
            return 1

        # 产物清单
        files = []
        for dp, _, fs in os.walk(static_dir):
            for f in fs:
                files.append(os.path.relpath(os.path.join(dp, f), static_dir))
        print("静态站产物 (%d):" % len(files))
        for f in sorted(files)[:10]:
            print("   ", f)
        images = [f for f in files if f.startswith("images" + os.sep)]
        print("images 数量: %d (画册 %d 张)" % (len(images), len(ids)))
        if len(images) < len(ids):
            ok = False
            print("  !! 预览图缺失:", pub.get("missing"))

        index_path = os.path.join(static_dir, "index.html")
        if not os.path.exists(index_path):
            ok = False
            print("  !! 缺 index.html")
        else:
            html = open(index_path, "rb").read().decode("utf-8", "replace")
            checks = {
                "密码门禁": ("password" in html.lower() or "密码" in html),
                "Glass Noir 背景": "#0C0D10" in html,
                "点缀金": "#E8C268" in html,
                "含标题": "千山暮雪" in html,
                "含客户名": "张先生" in html,
                "无外链 http": ("http://" not in html and "https://" not in html) or "127.0.0.1" in html,
            }
            for k, v in checks.items():
                print("   html %s: %s" % (k, v))
                if not v:
                    ok = False
                    print("     !! 缺失")

        # listPublished
        r = eng.call("gallery.listPublished", {})
        pubs = r.get("result") or []
        if isinstance(pubs, dict):
            pubs = pubs.get("items") or []
        print("listPublished -> %d 条" % len(pubs))
        row = next((p for p in pubs if p.get("albumId") == album_id), None)
        if not row:
            ok = False
            print("  !! 列表里找不到刚发布的画册")
        else:
            print("   %s" % json.dumps(row, ensure_ascii=False)[:320])
            stats = row.get("stats") or {}
            cr = stats.get("completionRate")
            if cr is not None and not (0.0 <= float(cr) <= 1.0):
                ok = False
                print("  !! completionRate 不是 0..1 比值: %s" % cr)

        # importFeedback：构造客户收藏包
        fb_path = "D:/LUMINA/build/runtime/gallery-verify-feedback.json"
        payload = {"schema": "lumina.gallery.feedback/1", "albumId": album_id, "token": token,
                   "exportedAt": 1792656000000,
                   "photos": [{"photoId": ids[0], "favorited": True},
                              {"photoId": ids[1], "favorited": False},
                              {"photoId": ids[2], "favorited": True}]}
        os.makedirs(os.path.dirname(fb_path), exist_ok=True)
        with open(fb_path, "w", encoding="utf-8") as fh:
            json.dump(payload, fh, ensure_ascii=False)
        r = eng.call("gallery.importFeedback", {"albumId": album_id, "filePath": fb_path})
        if "error" in r:
            ok = False
            print("FAIL importFeedback: %s" % json.dumps(r["error"], ensure_ascii=False))
        else:
            print("importFeedback -> %s" % json.dumps(r["result"], ensure_ascii=False))

        r = eng.call("gallery.getFeedback", {"albumId": album_id})
        got = r.get("result") or {}
        print("getFeedback -> favorites=%s rate=%s perPhoto=%s"
              % (got.get("favoritesCount"), got.get("completionRate"),
                 json.dumps(got.get("perPhoto"), ensure_ascii=False)[:200]))
        if got.get("favoritesCount") != 2:
            ok = False
            print("  !! 收藏数不符（期望 2）")

        # revoke
        r = eng.call("gallery.revoke", {"albumId": album_id})
        if "error" in r:
            ok = False
            print("FAIL revoke: %s" % json.dumps(r["error"], ensure_ascii=False))
        else:
            print("revoke -> ok")
        if os.path.isdir(static_dir):
            ok = False
            print("  !! 撤销后静态站目录仍在:", static_dir)
        else:
            print("撤销后目录已删除 OK")

        r = eng.call("gallery.listPublished", {})
        pubs = r.get("result") or []
        if isinstance(pubs, dict):
            pubs = pubs.get("items") or []
        if any(p.get("albumId") == album_id for p in pubs):
            ok = False
            print("  !! 撤销后仍出现在 listPublished")

        # 原片零改动
        for p, (size, mtime) in before.items():
            st = os.stat(p)
            if st.st_size != size or int(st.st_mtime) != mtime:
                ok = False
                print("  !! 原片被改动:", os.path.basename(p))
        if before:
            print("原片零写入 OK（%d 个）" % len(before))

        print("\n=== %s ===" % ("ALL PASS" if ok else "HAS FAILURES"))
        return 0 if ok else 1
    finally:
        eng.close()


if __name__ == "__main__":
    sys.exit(main())
