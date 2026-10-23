"""WP-B8 验收：真跑 export RPC，产出含水印的导出文件，并证明确实画在预期区域。

口径：
  A = 水印关闭导出；B = 水印开启（text='LUMINA TEST', bottom-right, opacity=100）导出。
  引擎文字水印几何（exporter.cpp applyWatermark）：
    margin = max(4, h/50)；block = max(1, h/100)；glyphW = 3b；glyphH = 5b；advance = 4b
    textW = len*advance - (advance-glyphW)；锚点 bottom-right → x0 = w-textW-margin, y0 = h-glyphH-margin
  断言：① A/B 有差异（水印真的画上去了）；② 差异像素 ≥90% 落在按上式算出的矩形内（不是乱画）。
产物保留在 D:/LUMINA/build/_wmtest/（不清理，便于人工查看）。
"""

import json
import os
import sys

sys.path.insert(0, "scripts")
from rpc_probe import Engine  # noqa: E402
from verify_hsl_export import decode_tiff  # noqa: E402

OUT = "D:/LUMINA/build/_wmtest"
TEXT = "LUMINA TEST"


def export_tiff(eng, photo_id, watermark, tag):
    spec = {
        "format": "tiff",
        "longEdge": 640,
        "colorSpace": "sRGB",
        "quality": 92,
        "psdKeepLayers": False,
        "watermark": watermark,
        "namingTemplate": "{date}-{name}",
        "outputDir": OUT + "/" + tag,
    }
    resp = eng.call("export.createJob", {"photoIds": [photo_id], "spec": spec})
    if resp.get("error"):
        raise RuntimeError("export.createJob 错误: %s" % json.dumps(resp["error"], ensure_ascii=False))
    job_id = (resp.get("result") or {}).get("jobId")
    if job_id is None:
        raise RuntimeError("未返回 jobId: %s" % json.dumps(resp, ensure_ascii=False)[:300])
    result = eng.wait_job(job_id, timeout=300.0)
    inner = result.get("result") or {}
    outputs = inner.get("outputs") or []
    if not outputs:
        raise RuntimeError("导出无输出: %s" % json.dumps(result)[:400])
    return outputs[0]["path"]


def main() -> int:
    os.makedirs(OUT, exist_ok=True)
    home = os.environ.get("LUMINA_PROBE_HOME", "C:/Users/zj/.lumina")
    eng = Engine(home=home)
    path_a = None
    path_b = None
    try:
        eng.call("initialize", {"clientName": "wm-export-verify", "clientVersion": "1.1.0", "protocolVersion": "1.0"})
        q = (eng.call("library.queryPhotos", {"filter": None, "sort": {"field": "id", "order": "asc"}, "page": {"offset": 0, "limit": 10}}).get("result") or {})
        items = q.get("items") or []
        if not items:
            raise RuntimeError("库中没有照片")
        photo_id = items[0]["id"]
        print("photo:", photo_id)
        wm_off = {"enabled": False, "position": "bottom-right", "scale": 0.88, "opacity": 100}
        wm_on = {"enabled": True, "position": "bottom-right", "scale": 0.88, "opacity": 100, "text": TEXT}
        path_a = export_tiff(eng, photo_id, wm_off, "off")
        path_b = export_tiff(eng, photo_id, wm_on, "on")
    finally:
        eng.close()

    w, h, rgb_a = decode_tiff(path_a)
    w2, h2, rgb_b = decode_tiff(path_b)
    assert (w, h) == (w2, h2), "尺寸不一致"
    print("size: %dx%d" % (w, h))

    # 预期文字矩形（与引擎同式）
    margin = max(4, h // 50)
    block = max(1, h // 100)
    glyph_w = 3 * block
    glyph_h = 5 * block
    advance = 4 * block
    text_w = len(TEXT) * advance - (advance - glyph_w)
    x0 = w - text_w - margin
    y0 = h - glyph_h - margin
    print("expected rect: x0=%d y0=%d w=%d h=%d (margin=%d block=%d)" % (x0, y0, text_w, glyph_h, margin, block))

    changed = 0
    in_rect = 0
    rect_px = 0
    for y in range(h):
        for x in range(w):
            i = (y * w + x) * 3
            d = max(abs(rgb_a[i] - rgb_b[i]), abs(rgb_a[i + 1] - rgb_b[i + 1]), abs(rgb_a[i + 2] - rgb_b[i + 2]))
            inside = x0 <= x < x0 + text_w and y0 <= y < y0 + glyph_h
            if inside:
                rect_px += 1
            if d > 8:
                changed += 1
                if inside:
                    in_rect += 1
    ratio_in = (in_rect / changed) if changed else 0.0
    print("changed=%d (%.2f%% of image)  inRect=%d  ratioInRect=%.4f  rectPx=%d  rectCoverage=%.4f"
          % (changed, 100.0 * changed / (w * h), in_rect, ratio_in, rect_px, (in_rect / rect_px) if rect_px else 0.0))
    print("A(无水印):", path_a)
    print("B(含水印):", path_b)

    ok = changed > 0 and ratio_in >= 0.9
    print("VERDICT:", "PASS（导出产物含水印，且落在引擎锚点算出的区域内）" if ok else "FAIL（水印未生效或落点不对）")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
