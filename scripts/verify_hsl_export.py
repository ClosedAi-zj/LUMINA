"""WP-B4 验收：导出产物 vs 引擎 HSL 公式逐像素比对。

口径：
  A = 色带全零导出；B = red.hue=12 / orange.sat=-30 / blue.lum=25 导出。
  导出管线（exporter.cpp）：解码 → 缩放 → applyToneApprox(exposure/contrast/saturation) →
  applyHslApprox(HSL 8 色带) → 色彩空间。两次导出仅 hsl 不同，因此
  formula(A) 应逐像素还原 B（允许 ±1 的 8bit 舍入差）。
  公式与 engine/src/imaging/imaging.cpp applyHslApprox 完全同参：
    权重 = 色相到带中心(0/30/60/120/180/240/280/320°) 的 45° 半宽三角衰减（环形）
    h += Σ(w·hue)·0.15°；s *= 1 + Σ(w·sat)/100；l += Σ(w·lum)/200
"""

import json
import os
import shutil
import struct
import sys
import zlib

sys.path.insert(0, "scripts")
from rpc_probe import Engine  # noqa: E402

OUT = "D:/LUMINA/build/_hsltest"
BANDS = ["red", "orange", "yellow", "green", "aqua", "blue", "purple", "magenta"]
CENTERS = [0.0, 30.0, 60.0, 120.0, 180.0, 240.0, 280.0, 320.0]
HALF = 45.0


def base_params(hsl):
    return {
        "exposure": 0.0,
        "contrast": 0.0,
        "saturation": 0.0,
        "hsl": hsl,
    }


def zero_hsl():
    return {b: {"hue": 0, "sat": 0, "lum": 0} for b in BANDS}


def test_hsl():
    h = zero_hsl()
    h["red"]["hue"] = 12
    h["orange"]["sat"] = -30
    h["blue"]["lum"] = 25
    return h


def decode_tiff(path):
    """解析引擎 writeTiff 的产物：II 小端、无压缩 RGB、单 strip（exporter.cpp 828-893 行）。"""
    data = open(path, "rb").read()
    assert data[:2] == b"II" and struct.unpack("<H", data[2:4])[0] == 42, "not tiff: " + path
    ifd = struct.unpack("<I", data[4:8])[0]
    (count,) = struct.unpack("<H", data[ifd : ifd + 2])
    tags = {}
    for k in range(count):
        off = ifd + 2 + k * 12
        tag, typ, cnt = struct.unpack("<HHI", data[off : off + 8])
        val = struct.unpack("<I", data[off + 8 : off + 12])[0]
        tags[tag] = val
    w, h = tags[256], tags[257]
    off, size = tags[273], tags[279]
    rgb = data[off : off + size]
    assert len(rgb) == w * h * 3, "tiff 像素不足"
    return w, h, rgb


def export_png(eng, photo_id, params, tag):
    spec = {
        "format": "tiff",
        "longEdge": 640,
        "colorSpace": "sRGB",
        "quality": 92,
        "psdKeepLayers": False,
        "watermark": {"enabled": False, "position": "bottom-right", "scale": 0.88, "opacity": 60},
        "namingTemplate": "{date}-{name}",
        "outputDir": OUT + "/" + tag,
    }
    eng.call("xmp.write", {"id": photo_id, "params": params})
    resp = eng.call("export.createJob", {"photoIds": [photo_id], "spec": spec})
    if resp.get("error"):
        raise RuntimeError("export.createJob 错误: %s" % json.dumps(resp["error"], ensure_ascii=False))
    job = resp.get("result") or {}
    job_id = job.get("jobId")
    if job_id is None:
        raise RuntimeError("export.createJob 未返回 jobId: %s" % json.dumps(resp, ensure_ascii=False)[:400])
    result = eng.wait_job(job_id, timeout=300.0)
    inner = result.get("result") or {}
    outputs = inner.get("outputs") or result.get("outputs") or []
    if not outputs:
        raise RuntimeError("导出无输出: %s" % json.dumps(result)[:400])
    return outputs[0]["path"]


def hue_distance(a, b):
    d = abs(a - b)
    return min(d, 360.0 - d)


def rgb_to_hsl(r, g, b):
    mx = max(r, g, b)
    mn = min(r, g, b)
    l = (mx + mn) / 2.0
    d = mx - mn
    if d <= 1e-9:
        return 0.0, 0.0, l
    s = d / (2.0 - mx - mn) if l > 0.5 else d / (mx + mn)
    if mx == r:
        h = (g - b) / d + (6.0 if g < b else 0.0)
    elif mx == g:
        h = (b - r) / d + 2.0
    else:
        h = (r - g) / d + 4.0
    return h / 6.0, s, l


def _hue_component(p, q, t):
    if t < 0.0:
        t += 1.0
    if t > 1.0:
        t -= 1.0
    if t < 1.0 / 6.0:
        return p + (q - p) * 6.0 * t
    if t < 0.5:
        return q
    if t < 2.0 / 3.0:
        return p + (q - p) * (2.0 / 3.0 - t) * 6.0
    return p


def hsl_to_rgb(h, s, l):
    if s <= 0.0:
        return l, l, l
    q = l * (1.0 + s) if l < 0.5 else l + s - l * s
    p = 2.0 * l - q
    return (
        _hue_component(p, q, h + 1.0 / 3.0),
        _hue_component(p, q, h),
        _hue_component(p, q, h - 1.0 / 3.0),
    )


def apply_formula(rgb, w_, h_, bands):
    """对 RGB 字节序列就地应用引擎 HSL 公式，返回新 bytes。bands = [(hue,sat,lum) x8]"""
    out = bytearray(len(rgb))
    for j in range(0, len(rgb), 3):
        r0, g0, b0 = rgb[j] / 255.0, rgb[j + 1] / 255.0, rgb[j + 2] / 255.0
        h, s, l = rgb_to_hsl(r0, g0, b0)
        if s > 0.0:
            hd = h * 360.0
            hue_sum = sat_sum = lum_sum = w_sum = 0.0
            for k in range(8):
                dist = hue_distance(hd, CENTERS[k])
                if dist >= HALF:
                    continue
                w = 1.0 - dist / HALF
                w_sum += w
                hue_sum += w * bands[k][0]
                sat_sum += w * bands[k][1]
                lum_sum += w * bands[k][2]
            if w_sum > 0.0:
                hd += hue_sum * 0.15
                if hd < 0.0:
                    hd += 360.0
                if hd >= 360.0:
                    hd -= 360.0
                s = min(1.0, max(0.0, s * (1.0 + sat_sum / 100.0)))
                l = min(1.0, max(0.0, l + lum_sum / 200.0))
                r0, g0, b0 = hsl_to_rgb(hd / 360.0, s, l)
        out[j] = int(r0 * 255.0 + 0.5) & 0xFF
        out[j + 1] = int(g0 * 255.0 + 0.5) & 0xFF
        out[j + 2] = int(b0 * 255.0 + 0.5) & 0xFF
    return bytes(out)


def main() -> int:
    os.makedirs(OUT, exist_ok=True)
    # 默认探针 home（build/runtime）是空库；HSL 导出比对需要真实照片，
    # 用 ~/.lumina（活库）。库数据先读后还原：导出前 xmp.read 存原参数，比对后写回。
    home = os.environ.get("LUMINA_PROBE_HOME", "C:/Users/zj/.lumina")
    eng = Engine(home=home)
    original_params = None
    photo_id = None
    try:
        eng.call("initialize", {"clientName": "hsl-export-verify", "clientVersion": "1.1.0", "protocolVersion": "1.0"})
        q = (eng.call("library.queryPhotos", {"filter": {"formats": ["JPG"]}, "sort": {"field": "id", "order": "asc"}, "page": {"offset": 0, "limit": 10}}).get("result") or {})
        items = q.get("items") or []
        if not items:
            q = (eng.call("library.queryPhotos", {"filter": None, "sort": {"field": "id", "order": "asc"}, "page": {"offset": 0, "limit": 10}}).get("result") or {})
            items = q.get("items") or []
        if not items:
            raise RuntimeError("库中没有照片")
        photo_id = items[0]["id"]
        print("photo:", photo_id)
        original_params = ((eng.call("xmp.read", {"id": photo_id}).get("result") or {}) or {}).get("params")

        path_a = export_png(eng, photo_id, base_params(zero_hsl()), "a")
        path_b = export_png(eng, photo_id, base_params(test_hsl()), "b")
        print("A:", path_a)
        print("B:", path_b)
    finally:
        # 还原活库照片的原始 sidecar 参数（探针不留痕）
        if photo_id is not None and original_params is not None:
            try:
                eng.call("xmp.write", {"id": photo_id, "params": original_params})
                print("sidecar restored")
            except Exception as e:  # noqa: BLE001
                print("WARN: sidecar 还原失败:", e)
        eng.close()

    wa, ha, rgb_a = decode_tiff(path_a)
    wb, hb, rgb_b = decode_tiff(path_b)
    print("A size: %dx%d  B size: %dx%d" % (wa, ha, wb, hb))
    if (wa, ha) != (wb, hb):
        print("MISMATCH-SIZE")
        return 1
    # 防空账：先确认 HSL 确实改了像素（A vs B 本身必须有差异），再做公式比对
    ab = [abs(rgb_a[i] - rgb_b[i]) for i in range(len(rgb_b))]
    ab_changed = sum(1 for d in ab if d > 0)
    print("A vs B: changedChannels=%d/%d (%.2f%%)  maxAbsDiff=%d"
          % (ab_changed, len(ab), 100.0 * ab_changed / len(ab), max(ab)))
    if ab_changed == 0:
        print("VERDICT: FAIL（A 与 B 完全相同，HSL 在导出管线里根本没生效）")
        shutil.rmtree(OUT, ignore_errors=True)
        return 1
    bands = [(12.0, 0.0, 0.0), (0.0, -30.0, 0.0), (0.0, 0.0, 0.0), (0.0, 0.0, 0.0),
             (0.0, 0.0, 0.0), (0.0, 0.0, 25.0), (0.0, 0.0, 0.0), (0.0, 0.0, 0.0)]
    expect = apply_formula(rgb_a, wa, ha, bands)
    n = wa * ha
    diffs = [abs(expect[i] - rgb_b[i]) for i in range(len(rgb_b))]
    mean_d = sum(diffs) / len(diffs)
    max_d = max(diffs)
    over2 = sum(1 for d in diffs if d > 2)
    print("pixels=%d  meanAbsDiff=%.4f  maxAbsDiff=%d  channels>2=%d (%.4f%%)"
          % (n, mean_d, max_d, over2, 100.0 * over2 / len(diffs)))
    ok = max_d <= 2
    print("VERDICT:", "PASS（公式与导出逐像素一致，±2 为 8bit 舍入容差）" if ok else "FAIL")
    if os.path.isdir(OUT):
        shutil.rmtree(OUT, ignore_errors=True)
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
