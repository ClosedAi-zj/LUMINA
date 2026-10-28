#!/usr/bin/env python3
# M2 T2.11（N31）：lensfun 数据库 → LUMINA profile JSON 离线转换脚本
#
# 用法（本环境出网受限，需自备 lensfun 数据库文件）：
#   python lensfun2lumina.py --lensfun-dir <lensfun db 目录> \
#        --camera-make "Panasonic" --camera-model "DC-S5M2" \
#        --lens "LUMIX S 20-60mm F3.5-5.6" --out engine/data/lens-profiles/
#
# lensfun 数据库通常位于 Linux /usr/share/lensfun/（lensfun 包）或 Windows
# lensfun 安装目录。现代 lensfun 0.3.x 常见两种布局：
#   ① 单文件 lensfun.xml（旧版 / 合并快照）—— 原脚本默认路径；
#   ② 版本化目录 data/db/0.3/<maker>/<model>.xml，或按卡口拆分的 flat 文件
#      （data/db/mil-sony.xml、mil-canon.xml、mil-panasonic.xml …），每个文件
#      内含多个 <lens> 元素。本脚本二者都支持：单文件优先，否则 glob 目录下
#      所有 *.xml 逐个解析（最小改动，保留单文件路径）。
#
# 本脚本只读其**数据**，不链接 lensfun 代码（架构 §6 依赖决策：数据 CC-BY-SA
# 3.0 / 代码 LGPL-3.0）。
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
#
# 注：lensfun 0.3 的 <lens> 用 <maker>/<model>/<focal> 子元素（非属性），
# 校正元素（distortion/tca/vignetting）挂在 <calibration> 下；本脚本按此解析，
# 并对旧版「属性式」写法做兜底。

import argparse
import json
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


def lens_child_text(lens, tag):
    """lensfun 0.3 数据库：<maker>/<model>/<focal> 是子元素而非属性"""
    c = lens.find(tag)
    if c is not None and c.text:
        return c.text.strip()
    return ""


def collect_xml_files(lensfun_dir):
    """单文件优先（原行为）；否则把目录下所有 *.xml 作为数据库
    （版本化 db 或按卡口拆分的 flat 文件均适用）。"""
    for name in ("lensfun.xml", "lens.xml"):
        p = os.path.join(lensfun_dir, name)
        if os.path.exists(p):
            return [p]
    files = []
    for root, _dirs, names in os.walk(lensfun_dir):
        for n in names:
            if n.lower().endswith(".xml"):
                files.append(os.path.join(root, n))
    return sorted(files)


def parse_focal_range(lens):
    """lensfun 0.3 焦距来源（按优先级取并集）：
     ① 显式 <focal value=".."/> 子元素；
     ② <focal min=".." max=".."/> 子元素；
     ③ 许多 zoom 的焦距只写在 <distortion focal=../><tca focal=../><vignetting
        focal=../> 属性上，无独立 <focal> 元素 —— 一并收集其极值；
     ④ 旧版 lens 的 focal-min/focal-max 属性（兜底）。"""
    vals = []
    fmin = fmax = None
    for f in lens.iter("focal"):
        if f.get("value"):
            try:
                vals.append(float(f.get("value")))
            except ValueError:
                pass
        lo = f.get("min")
        hi = f.get("max")
        try:
            if lo:
                fmin = float(lo) if fmin is None else min(fmin, float(lo))
            if hi:
                fmax = float(hi) if fmax is None else max(fmax, float(hi))
        except ValueError:
            pass
    # ③ 校正元素上的 focal 属性（zoom 常见）
    for e in lens.iter():
        fa = e.get("focal")
        if fa:
            try:
                vals.append(float(fa))
            except ValueError:
                pass
    if vals:
        return min(vals), max(vals)
    if fmin is not None or fmax is not None:
        return (fmin or fmax or 0.0), (fmax or fmin or 0.0)
    try:
        a = float(lens.get("focal-min")) if lens.get("focal-min") else 0.0
    except (TypeError, ValueError):
        a = 0.0
    try:
        b = float(lens.get("focal-max")) if lens.get("focal-max") else a
    except (TypeError, ValueError):
        b = a
    return a, b


def main():
    ap = argparse.ArgumentParser(description="lensfun db → LUMINA profile JSON")
    ap.add_argument("--lensfun-dir", required=True, help="lensfun 数据库目录（含 lensfun.xml 或 *.xml）")
    ap.add_argument("--lens", required=True, help="镜头名（lensfun <lens> 的 model，支持正则）")
    ap.add_argument("--camera-make", default="", help="可选：过滤机身 make")
    ap.add_argument("--camera-model", default="", help="可选：机身 model（写进输出）")
    ap.add_argument("--out", required=True, help="输出目录（engine/data/lens-profiles）")
    args = ap.parse_args()
    # lensfun 数据库里机身型号偶含 "@@" 分隔符 artifact（如 "Canon@@EOS R6"），
    # 归一化为空格后再落盘，避免污染 camera.model 显示。
    args.camera_make = args.camera_make.replace("@@", " ")
    args.camera_model = args.camera_model.replace("@@", " ")

    xml_files = collect_xml_files(args.lensfun_dir)
    if not xml_files:
        sys.exit(f"no lensfun xml found under {args.lensfun_dir}")

    pat = re.compile(args.lens, re.IGNORECASE)
    converted = 0
    skipped = 0
    for xml_path in xml_files:
        try:
            root = ET.parse(xml_path).getroot()
        except Exception as e:  # noqa: BLE001 - 单个文件坏不影响其它文件
            print(f"  [warn] parse failed {xml_path}: {e}", file=sys.stderr)
            continue
        for lens in root.iter("lens"):
            # lensfun 0.3：maker/model 是子元素（旧版为属性，作兜底）
            name = lens_child_text(lens, "model") or (lens.get("model") or "")
            if not name:
                continue
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
                skipped += 1
                continue
            # 全恒等（no-op）检查：仅当「出现过的每一种校正类型」都为恒等时才跳过。
            # 例如「恒等 distortion + 非恒等 vignetting」的镜头必须保留（其 vignetting
            # 仍是有意义的校正，不满足「全恒等」条件）。
            #   distortion 恒等: k1==k2==k3==0
            #   tca       恒等: scaleR==scaleB==1.0
            #   vignetting 恒等: a==b==c==0
            dist_id = dist is not None and dist["k1"] == 0 and dist["k2"] == 0 and dist["k3"] == 0
            tca_id = tca is not None and tca["scaleR"] == 1.0 and tca["scaleB"] == 1.0
            vig_id = vig is not None and vig["a"] == 0 and vig["b"] == 0 and vig["c"] == 0
            appeared = []
            if dist is not None:
                appeared.append(dist_id)
            if tca is not None:
                appeared.append(tca_id)
            if vig is not None:
                appeared.append(vig_id)
            if appeared and all(appeared):
                print(f"  [skip] {name}: calibration present but all-identity "
                      f"(no-op), skipping", file=sys.stderr)
                skipped += 1
                continue
            fmin, fmax = parse_focal_range(lens)
            center = (fmin + fmax) / 2.0 if fmax > 0 else 50.0
            b = {"focalMm": round(center, 2)}
            if dist:
                b["distortion"] = {"model": "brown-conrady", **dist}
            if tca:
                b["ca"] = {k: v for k, v in tca.items() if not k.startswith("_")}
            if vig:
                b["vignetting"] = vig
            maker = lens_child_text(lens, "maker") or (lens.get("maker") or "")
            out = {
                "schemaVersion": 1,
                "source": "lensfun-db(CC-BY-SA-3.0)",
                "attribution": ATTRIBUTION,
                "camera": {"make": args.camera_make, "model": args.camera_model},
                "lens": {"make": maker, "model": name},
                "focalBuckets": [b],
            }
            slug = re.sub(r"[^a-z0-9]+", "-", name.lower()).strip("-")[:60]
            out_path = os.path.join(args.out, f"lensfun_{slug}.json")
            with open(out_path, "w", encoding="utf-8") as f:
                json.dump(out, f, ensure_ascii=False, indent=2)
                f.write("\n")
            print(f"  [ok] {name} -> {out_path}")
            converted += 1
    print(f"converted {converted} lens profile(s); skipped {skipped} without usable "
          f"calibration; NOTE: attribution must be synced into NOTICE.md (L4)")
    if converted == 0:
        sys.exit(1)


if __name__ == "__main__":
    main()
