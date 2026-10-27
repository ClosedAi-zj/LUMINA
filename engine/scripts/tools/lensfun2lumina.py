#!/usr/bin/env python3
# M2 T2.11（N31）：lensfun 数据库 → LUMINA profile JSON 离线转换脚本
#
# 用法（本环境出网受限，需自备 lensfun 数据库文件）：
#   python lensfun2lumina.py --lensfun-dir <lensfun db 目录> \
#        --camera-make "Panasonic" --camera-model "DC-S5M2" \
#        --lens "LUMIX S 20-60mm F3.5-5.6" --out engine/data/lens-profiles/
#
# lensfun 数据库通常位于 Linux /usr/share/lensfun/（lensfun 包）或 Windows
# lensfun 安装目录，含 lensfun.xml（版本 0.3.x）。本脚本只读其**数据**，
# 不链接 lensfun 代码（架构 §6 依赖决策：数据 CC-BY-SA 3.0 / 代码 LGPL-3.0）。
#
# 输出：每支镜头一个 JSON（engine/data/lens-profiles/*.json，格式见
# docs/v1.2.0/M2-架构设计.md §3.4），source 固定 "lensfun-db(CC-BY-SA-3.0)"、
# attribution 自动带上 lensfun 项目署名（须同步进 NOTICE.md，L4 要求）。
#
# 模型映射：
#   lensfun distortion "model" ∈ {poly3/poly4/poly5, ptlens, acon}：
#     poly3:  r_d = r·(1 + k1·r²)              → k1, k2=k3=0
#     poly5/poly4: r_d = r·(1 + k1·r² + k2·r⁴ + k3·r⁶) → k1,k2,k3 直取
#     ptlens: r_d = r·(a·r³ + b·r² + c·r + 1)  → k1=b, k2=a, k3=0
#     acon:   有焦距参数的非多项式模型 → 跳过并告警（M2 不支持）
#   lensfun TCA "model" ∈ {linear, poly3, rational}：
#     linear: cr/cb 为 R/B 通道径向缩放        → scaleR=cr, scaleB=cb
#     poly3/rational: 随半径变化 → 取 r=1 处值近似（告警注明近似）
#   lensfun vignetting poly3/poly4/poly5: v(r)=1 + k1·r² + k2·r⁴ + k3·r⁶
#     （lensfun 语义：v<1 = 压角；LUMINA vignetting 形状 v>1 = 压角）
#     → A=-k1, B=-k2, C=-k3（符号翻转对齐 LUMINA「校正形状」口径）

import argparse
import json
import math
import os
import re
import sys
import xml.etree.ElementTree as ET

ATTRIBUTION = ("lens correction data derived from lensfun "
               "(https://lensfun.github.io/, CC-BY-SA 3.0)")


def parse_distortion(lens_elem):
    dist = lens_elem.find("distortion")
    if dist is None:
        return None
    model = dist.get("model", "poly3")
    if model == "acon":
        return None  # 非多项式，M2 不支持
    k1 = float(dist.get("k1", 0) or 0)
    k2 = float(dist.get("k2", 0) or 0)
    k3 = float(dist.get("k3", 0) or 0)
    if model == "poly3":
        return {"k1": k1, "k2": 0.0, "k3": 0.0}
    if model in ("poly5", "poly4"):
        return {"k1": k1, "k2": k2, "k3": k3}
    if model == "ptlens":
        # ptlens: a·r³+b·r²+c·r+1 → LUMINA k1=b, k2=a
        return {"k1": k2, "k2": k1, "k3": 0.0}
    return None


def parse_tca(lens_elem):
    tca = lens_elem.find("tca")
    if tca is None:
        return None
    model = tca.get("model", "linear")
    if model == "linear":
        return {"scaleR": float(tca.get("cr", 1) or 1),
                "scaleB": float(tca.get("cb", 1) or 1)}
    if model == "poly3":
        # vr(r)=1+kr1·r+kr2·r²+kr3·r³（系数为径向多项式）；取 r=1 处近似
        kr1 = float(tca.get("kr1", 0) or 0)
        kr2 = float(tca.get("kr2", 0) or 0)
        kr3 = float(tca.get("kr3", 0) or 0)
        vr = 1.0 + kr1 + kr2 + kr3
        kb1 = float(tca.get("kb1", 0) or 0)
        kb2 = float(tca.get("kb2", 0) or 0)
        kb3 = float(tca.get("kb3", 0) or 0)
        vb = 1.0 + kb1 + kb2 + kb3
        if abs(vr - 1.0) > 0.05 or abs(vb - 1.0) > 0.05:
            return None  # 近似失真过大，宁缺毋滥
        return {"scaleR": vr, "scaleB": vb, "_approx": True}
    return None


def parse_vignetting(lens_elem):
    vig = lens_elem.find("vignetting")
    if vig is None:
        return None
    k1 = float(vig.get("k1", 0) or 0)
    k2 = float(vig.get("k2", 0) or 0)
    k3 = float(vig.get("k3", 0) or 0)
    # lensfun: v(r)=1+k1·r²+k2·r⁴+k3·r⁶，v<1=压角 → LUMINA 形状取负（v>1=压角）
    return {"a": -k1, "b": -k2, "c": -k3}


def main():
    ap = argparse.ArgumentParser(description="lensfun db → LUMINA profile JSON")
    ap.add_argument("--lensfun-dir", required=True, help="lensfun 数据库目录（含 lensfun.xml）")
    ap.add_argument("--lens", required=True, help="镜头名（lensfun <lens> 的 model 属性，支持正则）")
    ap.add_argument("--camera-make", default="", help="可选：过滤机身 make")
    ap.add_argument("--camera-model", default="", help="可选：机身 model（写进输出）")
    ap.add_argument("--out", required=True, help="输出目录（engine/data/lens-profiles）")
    args = ap.parse_args()

    xml_path = os.path.join(args.lensfun_dir, "lensfun.xml")
    if not os.path.exists(xml_path):
        alt = os.path.join(args.lensfun_dir, "lens.xml")
        if os.path.exists(alt):
            xml_path = alt
        else:
            sys.exit(f"lensfun.xml not found under {args.lensfun_dir}")
    tree = ET.parse(xml_path)
    root = tree.getroot()

    pat = re.compile(args.lens, re.IGNORECASE)
    converted = 0
    for lens in root.iter("lens"):
        name = lens.get("model", "")
        if not pat.search(name):
            continue
        # 校正元素可能挂在 lens 自身或其 calibration 组下
        calib = lens.find("calibration")
        src = calib if calib is not None else lens
        dist = parse_distortion(src)
        tca = parse_tca(src)
        vig = parse_vignetting(src)
        if dist is None and vig is None and tca is None:
            print(f"  [skip] {name}: no usable calibration", file=sys.stderr)
            continue
        fmin, fmax = 0.0, 0.0
        fl = lens.get("focal-min"), lens.get("focal-max")
        try:
            fmin = float(fl[0]) if fl[0] else 0.0
            fmax = float(fl[1]) if fl[1] else fmin
        except (TypeError, ValueError):
            pass
        buckets = []
        center = (fmin + fmax) / 2.0 if fmax > 0 else 50.0
        b = {"focalMm": center}
        if dist:
            b["distortion"] = {"model": "brown-conrady", **dist}
        if tca:
            b["ca"] = {k: v for k, v in tca.items() if not k.startswith("_")}
        if vig:
            b["vignetting"] = vig
        buckets.append(b)
        out = {
            "schemaVersion": 1,
            "source": "lensfun-db(CC-BY-SA-3.0)",
            "attribution": ATTRIBUTION,
            "camera": {"make": args.camera_make, "model": args.camera_model},
            "lens": {"make": lens.get("maker", ""), "model": name},
            "focalBuckets": buckets,
        }
        slug = re.sub(r"[^a-z0-9]+", "-", name.lower()).strip("-")[:60]
        out_path = os.path.join(args.out, f"lensfun_{slug}.json")
        with open(out_path, "w", encoding="utf-8") as f:
            json.dump(out, f, ensure_ascii=False, indent=2)
            f.write("\n")
        print(f"  [ok] {name} -> {out_path}")
        converted += 1
    print(f"converted {converted} lens profile(s); "
          f"NOTE: attribution must be synced into NOTICE.md (L4)")
    if converted == 0:
        sys.exit(1)


if __name__ == "__main__":
    main()
