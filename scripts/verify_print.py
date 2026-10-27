# -*- coding: utf-8 -*-
"""M4-04 验证：真实跑 Electron 打印通道，校验 PDF 页面尺寸，写 m4-print.json。

判据（PRD M4-04 第 3 条）：PDF 输出成功且页面尺寸元数据正确。
- A4 纵向 595.28 × 841.89 pt（±2pt）
- A4 横向 841.89 × 595.28 pt
- A3 纵向 841.89 × 1190.55 pt
不打印到实体打印机（本环境无打印机），只验 PDF 通道。
"""
import json
import os
import re
import subprocess
import sys

ROOT = r"D:/LUMINA"
ELECTRON = os.path.join(ROOT, "app", "node_modules", "electron", "dist", "electron.exe")
SMOKE = os.path.join(ROOT, "build", "app", "electron", "print-smoke.cjs")
IMG = os.path.join(ROOT, "build", "engine", "m4-embed-sRGB.jpg")
OUT_DIR = os.path.join(ROOT, "build", "engine")

EXPECT = {
    ("A4", False): (595.28, 841.89),
    ("A4", True): (841.89, 595.28),
    ("A3", False): (841.89, 1190.55),
}


def mediabox(pdf_path):
    data = open(pdf_path, "rb").read()
    m = re.search(rb"/MediaBox\s*\[\s*([\d.]+)\s+([\d.]+)\s+([\d.]+)\s+([\d.]+)\s*\]", data)
    if not m:
        return None
    x0, y0, x1, y1 = (float(v) for v in m.groups())
    return (abs(x1 - x0), abs(y1 - y0))


def run_case(page, landscape):
    out = os.path.join(OUT_DIR, "m4-print-%s-%s.pdf" % (page, "land" if landscape else "port"))
    if os.path.exists(out):
        os.remove(out)
    env = dict(os.environ)
    env.pop("ELECTRON_RUN_AS_NODE", None)
    args = [ELECTRON, SMOKE, IMG, out, page]
    if landscape:
        args.append("landscape")
    p = subprocess.run(args, capture_output=True, text=True, timeout=180, env=env, cwd=ROOT)
    line = ""
    for ln in (p.stdout or "").splitlines():
        if ln.strip().startswith("{"):
            line = ln.strip()
    try:
        res = json.loads(line) if line else {"ok": False, "error": (p.stderr or "")[:400]}
    except json.JSONDecodeError:
        res = {"ok": False, "error": line[:400]}
    size = mediabox(out) if os.path.exists(out) else None
    exp = EXPECT[(page, landscape)]
    ok = bool(res.get("ok")) and size is not None and abs(size[0] - exp[0]) <= 2 and abs(size[1] - exp[1]) <= 2
    return {
        "page": page,
        "landscape": landscape,
        "file": out,
        "bytes": res.get("bytes"),
        "mediaBox": [round(size[0], 2), round(size[1], 2)] if size else None,
        "expected": [exp[0], exp[1]],
        "pass": ok,
        "error": res.get("error"),
    }


def main():
    if not os.path.exists(ELECTRON):
        print("FAIL: 找不到 electron", ELECTRON)
        return 1
    if not os.path.exists(SMOKE):
        print("FAIL: 找不到 print-smoke.cjs，先 npm run build:electron")
        return 1
    if not os.path.exists(IMG):
        print("FAIL: 缺少样本图", IMG)
        return 1

    cases = [run_case("A4", False), run_case("A4", True), run_case("A3", False)]
    for c in cases:
        print("%s %s -> MediaBox %s (期望 %s) %s" % (
            c["page"], "landscape" if c["landscape"] else "portrait",
            c["mediaBox"], c["expected"], "PASS" if c["pass"] else "FAIL " + str(c.get("error"))))

    ok = all(c["pass"] for c in cases)
    report = {
        "task": "M4-04",
        "channel": "Electron webContents.printToPDF（无打印机环境的 PDF 分支）",
        "note": "未验证实体打印机；打印对话框/份数为 v1.3 范围（本机无打印机，PRD 允许走 PDF 通道）",
        "cases": cases,
        "pass": ok,
    }
    with open(os.path.join(OUT_DIR, "m4-print.json"), "w", encoding="utf-8") as f:
        json.dump(report, f, ensure_ascii=False, indent=2)
    print("报告已写 build/engine/m4-print.json")
    print("M4-04 PASS" if ok else "M4-04 FAIL")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
