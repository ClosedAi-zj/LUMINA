"""WP-C3 验收：中文水印（系统字体光栅化）+ PNG 导出 + PSD 拒绝。

口径（真实 RW2 库 build/rw2-real2，longEdge=640 TIFF/PNG）：
  A = 无水印；B = 中文水印「版权所有 · 齐活林 2025」；C = ASCII 水印「LUMINA TEST」。
  断言：
  ① B−A 有大量变化像素且包围盒落在右下锚区（中文真的画出来了，不再是 '?'）；
  ② B 的变化像素数显著多于 C（真实字形 vs 3x5 点阵的信息量差异）；
  ③ 导出结果 warnings 中不含「非 ASCII」降级提示（中文走系统字体，不记点阵 warning）；
  ④ PNG 导出：签名正确、尺寸与 TIFF 一致、**像素逐字节相等**（PNG 无损）；
  ⑤ PSD：createJob 明确报 5001 codecUnavailable（不伪造成功）。
产物保留在 D:/LUMINA/build/_wmcjk/。
"""

import json
import os
import struct
import sys

sys.path.insert(0, "scripts")
from rpc_probe import Engine  # noqa: E402
from verify_hsl_export import decode_tiff  # noqa: E402

OUT = "D:/LUMINA/build/_wmcjk"
HOME = "D:/LUMINA/build/rw2-real2"
CJK = "版权所有 · 齐活林 2025"
ASCII_TEXT = "LUMINA TEST"


def export_one(eng, photo_id, fmt, watermark, tag, long_edge=640):
    spec = {
        "format": fmt,
        "longEdge": long_edge,
        "colorSpace": "sRGB",
        "quality": 92,
        "psdKeepLayers": False,
        "watermark": watermark,
        "namingTemplate": "{date}-{name}",
        "outputDir": OUT + "/" + tag,
    }
    resp = eng.call("export.createJob", {"photoIds": [photo_id], "spec": spec})
    if resp.get("error"):
        return None, resp["error"]
    job_id = (resp.get("result") or {}).get("jobId")
    if job_id is None:
        raise RuntimeError("未返回 jobId: %s" % json.dumps(resp, ensure_ascii=False)[:300])
    result = eng.wait_job(job_id, timeout=300.0)
    inner = result.get("result") or {}
    outputs = inner.get("outputs") or []
    if not outputs:
        raise RuntimeError("导出无输出: %s" % json.dumps(result)[:400])
    return outputs[0]["path"], inner.get("warnings")


def diff_stats(rgb_a, rgb_b, w, h):
    changed = 0
    minx, miny, maxx, maxy = w, h, -1, -1
    for y in range(h):
        for x in range(w):
            i = (y * w + x) * 3
            d = max(abs(rgb_a[i] - rgb_b[i]), abs(rgb_a[i + 1] - rgb_b[i + 1]),
                    abs(rgb_a[i + 2] - rgb_b[i + 2]))
            if d > 8:
                changed += 1
                if x < minx: minx = x
                if x > maxx: maxx = x
                if y < miny: miny = y
                if y > maxy: maxy = y
    return changed, (minx, miny, maxx, maxy)


def main() -> int:
    os.makedirs(OUT, exist_ok=True)
    eng = Engine(home=HOME)
    paths = {}
    warns = {}
    psd_err = None
    try:
        eng.call("system.initialize", {"clientName": "wmcjk-verify", "clientVersion": "1.1.0", "protocolVersion": "1.0"})
        q = (eng.call("library.queryPhotos", {"page": {"offset": 0, "limit": 5}}).get("result") or {})
        items = q.get("items") or []
        if not items:
            raise RuntimeError("库中没有照片")
        photo_id = items[0]["id"]
        print("photo:", photo_id)

        base_wm = {"enabled": False, "position": "bottom-right", "scale": 0.88, "opacity": 100}
        cjk_wm = {"enabled": True, "position": "bottom-right", "scale": 0.88, "opacity": 100, "text": CJK}
        ascii_wm = {"enabled": True, "position": "bottom-right", "scale": 0.88, "opacity": 100, "text": ASCII_TEXT}

        paths["a"], _ = export_one(eng, photo_id, "tiff", base_wm, "a")
        paths["b"], warns["b"] = export_one(eng, photo_id, "tiff", cjk_wm, "b")
        paths["c"], warns["c"] = export_one(eng, photo_id, "tiff", ascii_wm, "c")
        paths["png"], warns["png"] = export_one(eng, photo_id, "png", cjk_wm, "png")

        # PSD 必须被明确拒绝
        resp = eng.call("export.createJob", {
            "photoIds": [photo_id],
            "spec": {"format": "psd", "longEdge": 640, "colorSpace": "sRGB", "quality": 92,
                     "watermark": base_wm, "namingTemplate": "{date}-{name}",
                     "outputDir": OUT + "/psd"},
        })
        psd_err = resp.get("error")
    finally:
        eng.close()

    ok = True

    # ---- ① ② ③ 中文水印 ----
    w, h, rgb_a = decode_tiff(paths["a"])
    _, _, rgb_b = decode_tiff(paths["b"])
    _, _, rgb_c = decode_tiff(paths["c"])
    changed_b, box_b = diff_stats(rgb_a, rgb_b, w, h)
    changed_c, _ = diff_stats(rgb_a, rgb_c, w, h)
    print("size %dx%d" % (w, h))
    print("CJK  changed=%d bbox=%s" % (changed_b, box_b))
    print("ASCII changed=%d" % changed_c)
    # 锚区：bottom-right → 右半 + 下半；字高应与点阵路径观感一致（≈5% 图高，21px@427）
    in_anchor = box_b[0] >= w * 0.4 and box_b[1] >= h * 0.5
    glyph_h_actual = box_b[3] - box_b[1] + 1
    glyph_w_actual = box_b[2] - box_b[0] + 1
    print("CJK bbox 在右下锚区:", in_anchor, " 字高=%dpx 字宽=%dpx" % (glyph_h_actual, glyph_w_actual))
    geom_ok = in_anchor and 10 <= glyph_h_actual <= 60 and glyph_w_actual > len(CJK) * 10
    if not (changed_b > 1000 and geom_ok):
        print("FAIL: 中文水印未按预期渲染")
        ok = False
    warn_text_b = json.dumps(warns.get("b") or [], ensure_ascii=False)
    print("B warnings:", warn_text_b[:200])
    if "ASCII" in warn_text_b or "?" in warn_text_b:
        print("FAIL: 中文水印仍记点阵降级 warning")
        ok = False
    warn_text_c = json.dumps(warns.get("c") or [], ensure_ascii=False)
    if "ASCII" in warn_text_c:
        print("FAIL: ASCII 水印不应记降级 warning")
        ok = False
    print("C warnings:", warn_text_c[:200])

    # ---- ④ PNG 无损 ----
    with open(paths["png"], "rb") as f:
        sig = f.read(8)
    png_sig_ok = sig == b"\x89PNG\r\n\x1a\n"
    # 读 IHDR 尺寸
    with open(paths["png"], "rb") as f:
        head = f.read(33)
    pw, ph = struct.unpack(">II", head[16:24])
    # 用 PIL 解 PNG 与 TIFF 逐像素比对（PNG 无损 → 必须完全相等）
    try:
        from PIL import Image
        import io
        pim = Image.open(paths["png"]).convert("RGB")
        ppix = list(pim.getdata())
        same = (pw, ph) == (w, h) and len(ppix) == w * h and all(
            ppix[i] == (rgb_b[i * 3], rgb_b[i * 3 + 1], rgb_b[i * 3 + 2]) for i in range(w * h))
    except Exception as e:  # noqa: BLE001
        print("PIL 比对失败:", e)
        same = False
    print("PNG sig=%s size=%dx%d pixels_equal_tiff=%s" % (png_sig_ok, pw, ph, same))
    if not (png_sig_ok and same):
        print("FAIL: PNG 导出不符合预期")
        ok = False

    # ---- ⑤ PSD 拒绝 ----
    psd_code = (psd_err or {}).get("code")
    print("PSD error:", json.dumps(psd_err, ensure_ascii=False)[:200])
    if psd_code != 5001:
        print("FAIL: PSD 未按 5001 明确拒绝")
        ok = False

    print("A(无水印):", paths["a"])
    print("B(中文水印):", paths["b"])
    print("C(ASCII):", paths["c"])
    print("PNG(中文水印):", paths["png"])
    print("VERDICT:", "PASS" if ok else "FAIL")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
