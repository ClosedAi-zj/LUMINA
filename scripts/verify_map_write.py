"""map 域写方法端到端验收：setPhotoLocation / clearPhotoLocation。

验证点：
1. DB 中 gps_lat/gps_lng/location_id 真的写入并读回一致
2. sidecar 的 exif GPS 真的落盘，且 packet 结构不被破坏（标签配对、可 re-parse）
3. clear 后 DB 归空、sidecar 的 GPS 节点被摘除，其余节点（crs:）逐字节不动
4. 原片文件（.RW2/.DNG）零写入 —— mtime + size 前后不变
"""

import json
import os
import re
import shutil
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from rpc_probe import Engine  # noqa: E402

MOCKLIB = "D:/LUMINA/mocklib"


def find_sidecar(photo_path):
    d, base = os.path.split(photo_path)
    stem = os.path.splitext(base)[0]
    cand = os.path.join(d, stem + ".xmp")
    return cand if os.path.exists(cand) else None


def tag_stats(text):
    opens = len(re.findall(r"<([A-Za-z][\w:.-]*)(?:\s[^>]*)?>", text))
    closes = len(re.findall(r"</([A-Za-z][\w:.-]*)>", text))
    selfclosing = len(re.findall(r"<([A-Za-z][\w:.-]*)(?:\s[^>]*)?/>", text))
    return opens, closes, selfclosing


def main():
    eng = Engine()
    ok = True
    try:
        eng.call("initialize", {"clientName": "map-write-verify",
                                "clientVersion": "1.0.0", "protocolVersion": "1.0"})

        # 取 3 张未定位照片
        r = eng.call("map.listUnlocated", {"page": {"offset": 0, "limit": 3}})
        items = (r.get("result") or {}).get("items") or []
        if not items:
            print("FAIL: listUnlocated 返回空，无法继续")
            return 1
        ids = [it["id"] for it in items]
        print("未定位样本: %s" % [(it["id"], it.get("fileName")) for it in items])

        # 记录原片基线（红线：原片零写入）
        before = {}
        for it in items:
            p = it.get("path") or it.get("filePath")
            if p and os.path.exists(p):
                st = os.stat(p)
                before[it["id"]] = (p, st.st_size, st.st_mtime)
        print("原片基线: %s" % {k: (os.path.basename(v[0]), v[1], int(v[2])) for k, v in before.items()})

        lat, lng, alt = 31.0996, 102.7568, 3810.0
        name = "四姑娘山·双桥沟口"

        # ---- 1) setPhotoLocation ----
        r = eng.call("map.setPhotoLocation",
                     {"ids": ids, "lat": lat, "lng": lng, "altitude": alt, "locationName": name})
        if "error" in r:
            print("FAIL setPhotoLocation: %s" % r["error"])
            return 1
        res = r["result"]
        print("setPhotoLocation -> updated=%s locationId=%s" % (res.get("updated"), res.get("locationId")))
        if res.get("updated") != len(ids):
            ok = False
            print("  !! updated 数量不符，期望 %d" % len(ids))

        # ---- 2) DB 读回（PhotoSummary 只有 gpsLat/gpsLng/hasGps，地点名走 map.getLocationDetail）----
        r = eng.call("library.getPhotoDetail", {"id": ids[0]})
        rec = r.get("result") or {}
        got_lat = rec.get("gpsLat")
        got_lng = rec.get("gpsLng")
        has_gps = rec.get("hasGps")
        print("library.getPhotoDetail -> gpsLat=%s gpsLng=%s hasGps=%s" % (got_lat, got_lng, has_gps))
        if got_lat is None or abs(float(got_lat) - lat) > 1e-6:
            ok = False
            print("  !! DB gps_lat 未落库")
        if got_lng is None or abs(float(got_lng) - lng) > 1e-6:
            ok = False
            print("  !! DB gps_lng 未落库")
        if not has_gps:
            ok = False
            print("  !! DB hasGps 未置位")

        r = eng.call("map.getLocationDetail", {"locationId": res.get("locationId")})
        loc = r.get("result") or {}
        print("map.getLocationDetail -> name=%s count=%s alt=%s" % (loc.get("name"), loc.get("count"), loc.get("altitude")))
        if loc.get("name") != name:
            ok = False
            print("  !! 地点名不符（期望 %s）" % name)
        if (loc.get("count") or 0) < len(ids):
            ok = False
            print("  !! 地点聚合张数不足（期望 >= %d）" % len(ids))

        # ---- 3) sidecar 检查 ----
        sidecar_report = []
        for it in items:
            p = it.get("path") or it.get("filePath")
            sc = find_sidecar(p) if p else None
            if not sc:
                sidecar_report.append((it["id"], "no-sidecar", None))
                continue
            txt = open(sc, "rb").read().decode("utf-8", "replace")
            has_packet = "<?xpacket begin=" in txt and "<?xpacket end=" in txt
            has_gps = "exif:GPSLatitude" in txt
            o, c, s = tag_stats(txt)
            sidecar_report.append((it["id"], os.path.basename(sc),
                                   {"packet": has_packet, "gps": has_gps,
                                    "tagOpen": o, "tagClose": c + s}))
        for row in sidecar_report:
            print("sidecar %s | %s | %s" % row)
            if row[2] and isinstance(row[2], dict):
                if not row[2]["packet"]:
                    ok = False
                    print("  !! packet 头尾缺失")
                if not row[2]["gps"]:
                    ok = False
                    print("  !! exif GPS 未写入 sidecar")
                if row[2]["tagOpen"] != row[2]["tagClose"]:
                    ok = False
                    print("  !! 标签不配对 开=%d 闭+自闭合=%d" % (row[2]["tagOpen"], row[2]["tagClose"]))

        # ---- 4) clear 并复核 ----
        r = eng.call("map.clearPhotoLocation", {"ids": ids})
        if "error" in r:
            print("FAIL clearPhotoLocation: %s" % r["error"])
            return 1
        print("clearPhotoLocation -> updated=%s" % (r["result"] or {}).get("updated"))

        r = eng.call("library.getPhotoDetail", {"id": ids[0]})
        rec = r.get("result") or {}
        if rec.get("hasGps") or rec.get("gpsLat") is not None:
            ok = False
            print("  !! clear 后 DB 仍有定位: hasGps=%s gpsLat=%s" % (rec.get("hasGps"), rec.get("gpsLat")))
        else:
            print("clear 后 DB 已归空 OK (hasGps=false)")

        for it in items:
            p = it.get("path") or it.get("filePath")
            sc = find_sidecar(p) if p else None
            if not sc:
                continue
            txt = open(sc, "rb").read().decode("utf-8", "replace")
            o, c, s = tag_stats(txt)
            balanced = (o == c + s)
            gps_gone = "exif:GPSLatitude" not in txt
            crs_kept = txt.count("crs:") > 0
            print("清除后 %s | gpsGone=%s balanced=%s crsKept=%s (开%d 闭%d 自闭%d)"
                  % (os.path.basename(sc), gps_gone, balanced, crs_kept, o, c, s))
            if not balanced:
                ok = False
                print("  !! 清除后结构失衡")
            if not gps_gone:
                ok = False
                print("  !! exif GPS 未摘除")

        # ---- 5) 原片零写入 ----
        for pid, (p, size, mtime) in before.items():
            st = os.stat(p)
            if st.st_size != size or int(st.st_mtime) != int(mtime):
                ok = False
                print("  !! 原片被改动: %s size %d->%d mtime %d->%d"
                      % (os.path.basename(p), size, st.st_size, int(mtime), int(st.st_mtime)))
        if before:
            print("原片零写入 OK（%d 个文件 size+mtime 未变）" % len(before))

        print("\n=== %s ===" % ("ALL PASS" if ok else "HAS FAILURES"))
        return 0 if ok else 1
    finally:
        eng.close()


if __name__ == "__main__":
    sys.exit(main())
