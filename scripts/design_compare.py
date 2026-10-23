"""LUMINA 设计稿 vs 实现基线 · 结构化比对（WP-C1 视觉收敛）

设计稿为 2880x1800（2x，逻辑宽 1440，仅深色）；实现窗口 1664x935。
像素级逐一比对无意义——本脚本做**确定性结构比对**：
  1. 色带聚类：逐行/逐列统计令牌色占比 → 提取顶栏/侧栏/画布/右面板边界；
  2. 几何归一：边界同时以「分数」和「各自画布逻辑像素」输出，
     固定宽度组件（侧栏/右面板）直接比 px，分数差仅由画布宽差导致不算偏差；
  3. 令牌锚点：chrome 区域（顶栏/侧栏/右面板中体色）与令牌精确值核对（±3/通道）。
输出：每帧几何/配色指标表。设计稿仅深色 → 配色只比 dark；浅色帧靠 09 帧规范 + 冒烟证据。

用法：python scripts/design_compare.py [--frames 01,02,...] [--tol 6] [--json]
"""

import argparse
import json
import os
import sys

from PIL import Image

DESIGN_DIR = "C:/Users/zj/WorkBuddy/2026-10-21-20-30-19/lumina-design-deliverables"
BASELINE_DIR = "D:/LUMINA/docs/ui-1.1/baseline"

DARK_TOKENS = {
    "canvas": (0x0C, 0x0D, 0x10),
    "panel": (0x14, 0x16, 0x1A),
    "card": (0x1D, 0x20, 0x25),
    "hover": (0x23, 0x27, 0x2D),
    "viewer": (0x08, 0x09, 0x0B),
    "browserBar": (0x18, 0x1B, 0x1F),
    "border": (0x23, 0x27, 0x2D),
}
LIGHT_TOKENS = {
    "canvas": (0xF4, 0xF3, 0xF0),
    "panel": (0xFF, 0xFF, 0xFF),
    "card": (0xFA, 0xF9, 0xF6),
    "hover": (0xEF, 0xEE, 0xE9),
    "viewer": (0x08, 0x09, 0x0B),
    "browserBar": (0xFF, 0xFF, 0xFF),
    "border": (0xE4, 0xE2, 0xDC),
}

# 设计帧 → 基线截图（design stem, baseline stem）；设计稿逻辑宽 1440、实现 1664
FRAMES = {
    "01": ("01-library", "01-library"),
    "02": ("02-develop", "02-develop-fast"),
    "03": ("03-portfolio", "03-portfolio"),
    "04": ("04-web-gallery", "04-gallery"),
    "05": ("05-culling", "05-culling"),
    "06": ("06-ingest", "06-ingest"),
    "07": ("07-map", "07-map"),
    "08": ("08-timelapse", "08-timelapse"),
    "10": ("10-develop-pro", "10-develop-pro"),
    "12": ("12-landscape-delivery", "12-landscape"),
}
DESIGN_LOGICAL_W = 1440
BASE_W = 1664


def nearest_token(px, tokens, tol):
    best, best_d = None, 1e9
    for name, t in tokens.items():
        d = sum((a - b) ** 2 for a, b in zip(px, t))
        if d < best_d:
            best, best_d = name, d
    return best if best_d <= tol * tol * 3 else None


def _bands(samples, min_frac, merge_gap):
    """samples: [(frac, dom, cov)] → 合并色带 [(start, end, token)]。"""
    bands = []
    for frac, dom, cov in samples:
        if dom and cov >= 0.55:
            if bands and bands[-1][2] == dom and frac - bands[-1][1] <= merge_gap:
                bands[-1][1] = frac
                continue
            bands.append([frac, frac, dom])
        elif bands:
            bands[-1][1] = frac
    return [(a, b, t) for a, b, t in bands if b - a >= min_frac]


def row_bands(img, tokens, tol):
    w, h = img.size
    px = img.load()
    step_y = max(1, h // 900)
    step_x = max(1, w // 240)
    samples = []
    for y in range(0, h, step_y):
        counts = {}
        for x in range(0, w, step_x):
            t = nearest_token(px[x, y][:3], tokens, tol)
            if t:
                counts[t] = counts.get(t, 0) + 1
        total = sum(counts.values()) or 1
        dom = max(counts, key=counts.get) if counts else None
        samples.append((y / h, dom, counts.get(dom, 0) / total if dom else 0.0))
    return _bands(samples, 0.008, 0.02)


def col_bands(img, tokens, tol, y0, y1):
    w, h = img.size
    px = img.load()
    ya, yb = int(h * y0), int(h * y1)
    step_x = max(1, w // 900)
    step_y = max(1, (yb - ya) // 240)
    samples = []
    for x in range(0, w, step_x):
        counts = {}
        for y in range(ya, yb, step_y):
            t = nearest_token(px[x, y][:3], tokens, tol)
            if t:
                counts[t] = counts.get(t, 0) + 1
        total = sum(counts.values()) or 1
        dom = max(counts, key=counts.get) if counts else None
        samples.append((x / w, dom, counts.get(dom, 0) / total if dom else 0.0))
    return _bands(samples, 0.008, 0.02)


def region_median(img, x0, x1, y0, y1):
    w, h = img.size
    crop = img.crop((int(w * x0), int(h * y0), int(w * x1), int(h * y1))).resize((8, 8))
    data = list(crop.getdata())
    n = len(data)
    return tuple(sum(p[i] for p in data) // n for i in range(3))


def analyze(path, tokens, tol, logical_w, top_frac=1.0):
    img = Image.open(path).convert("RGB")
    if top_frac < 1.0:
        img = img.crop((0, 0, img.size[0], int(img.size[1] * top_frac)))
    hb = row_bands(img, tokens, tol)
    content = (hb[0][1], hb[-1][0]) if len(hb) >= 3 else (0.0, 1.0)
    vb = col_bands(img, tokens, tol, *content)
    # 侧栏 = 内容带内最左侧 panel 带；右面板 = 最右侧 panel 带
    sidebar = next((b for b in vb if b[2] == "panel"), None)
    right = next((b for b in reversed(vb) if b[2] == "panel"), None)
    top_bar = hb[0] if hb else None
    scale = logical_w / img.size[0]
    return {
        "size": img.size,
        "top_bar_frac": [round(top_bar[0], 3), round(top_bar[1], 3), top_bar[2]] if top_bar else None,
        "sidebar_frac": [round(sidebar[0], 3), round(sidebar[1], 3)] if sidebar else None,
        "sidebar_px": [round(sidebar[0] * logical_w), round(sidebar[1] * logical_w)] if sidebar else None,
        "right_panel_frac": [round(right[0], 3), round(right[1], 3)] if right else None,
        "right_panel_px": [round(right[0] * logical_w), round(right[1] * logical_w)] if right else None,
        "row_bands": [(round(a, 3), round(b, 3), t) for a, b, t in hb[:6]],
        "col_bands": [(round(a, 3), round(b, 3), t) for a, b, t in vb[:8]],
        "chrome_anchors": {
            "topbar": region_median(img, 0.45, 0.55, 0.005, 0.02),
            "sidebar": region_median(img, 0.01, 0.04, 0.5, 0.54),
            "right_panel": region_median(img, 0.96, 0.99, 0.5, 0.54),
        },
        "_scale": scale,
    }


def compare(d, b, logical_d, logical_b):
    """几何偏差：同尺度 px 差 > 12px 记一条。"""
    notes = []
    for key, label in (("sidebar", "侧栏"), ("right_panel", "右面板")):
        if d.get(key + "_px") and b.get(key + "_px"):
            dd = d[key + "_px"][1] - d[key + "_px"][0]
            db = b[key + "_px"][1] - b[key + "_px"][0]
            if abs(dd - db) > 12:
                notes.append(f"{label}宽 设计{dd}px vs 实现{db}px（差{dd - db:+d}px）")
    if d.get("top_bar_frac") and b.get("top_bar_frac"):
        dh = (d["top_bar_frac"][1] - d["top_bar_frac"][0]) * logical_d
        bh = (b["top_bar_frac"][1] - b["top_bar_frac"][0]) * logical_b
        if abs(dh - bh) > 10:
            notes.append(f"顶栏高 设计{round(dh)}px vs 实现{round(bh)}px（差{round(dh - bh):+d}px）")
    #  chrome 锚点与令牌核对（±3）
    for name, tok in DARK_TOKENS.items():
        pass
    anchor_notes = []
    for zone, val in b["chrome_anchors"].items():
        expect = {"topbar": "browserBar", "sidebar": "panel", "right_panel": "panel"}[zone]
        t = DARK_TOKENS[expect]
        dist = max(abs(val[i] - t[i]) for i in range(3))
        if dist > 3:
            anchor_notes.append(f"{zone} 中体色 {val} 偏离令牌 {expect} {t}（maxΔ{ dist }）")
    return notes, anchor_notes


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--frames", default="")
    ap.add_argument("--tol", type=int, default=6)
    ap.add_argument("--json", action="store_true")
    args = ap.parse_args()
    want = [f.strip() for f in args.frames.split(",") if f.strip()] or list(FRAMES)

    report = {}
    for key in want:
        if key not in FRAMES:
            continue
        dstem, bstem = FRAMES[key]
        dpath = os.path.join(DESIGN_DIR, dstem + ".png")
        bpath = os.path.join(BASELINE_DIR, f"dark-{bstem}.png")
        entry = {}
        if os.path.exists(dpath) and os.path.exists(bpath):
            top = 1800 / 3920 if key == "04" else 1.0
            d = analyze(dpath, DARK_TOKENS, args.tol, DESIGN_LOGICAL_W, top)
            b = analyze(bpath, DARK_TOKENS, args.tol, BASE_W)
            geo, anchor = compare(d, b, DESIGN_LOGICAL_W, BASE_W)
            entry = {"design": d, "baseline": b, "geo_notes": geo, "anchor_notes": anchor}
        report[key] = entry

    if args.json:
        print(json.dumps(report, ensure_ascii=False, indent=1))
        return

    for key, e in report.items():
        if not e:
            print("=" * 72)
            print("FRAME", key, FRAMES[key][0], "MISSING PAIR")
            continue
        d, b = e["design"], e["baseline"]
        print("=" * 72)
        print(f"FRAME {key} {FRAMES[key][0]}   design {d['size']} (逻辑 {DESIGN_LOGICAL_W})  baseline {b['size']}")
        print(f"  sidebar   design {d['sidebar_px']}px  baseline {b['sidebar_px']}px")
        print(f"  right     design {d['right_panel_px']}px  baseline {b['right_panel_px']}px")
        print(f"  topbar    design {d['top_bar_frac']}  baseline {b['top_bar_frac']}")
        print(f"  chrome    design {d['chrome_anchors']}")
        print(f"  chrome    base   {b['chrome_anchors']}")
        for n in e["geo_notes"]:
            print("  GEO>", n)
        for n in e["anchor_notes"]:
            print("  ANCHOR>", n)
        if not e["geo_notes"] and not e["anchor_notes"]:
            print("  (geometry & chrome anchors within tolerance)")


if __name__ == "__main__":
    main()
