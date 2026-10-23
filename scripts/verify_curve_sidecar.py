"""验证 toneCurveRGB 与 hsl 分组是否落盘（一次写入，避免触发引擎二次写入退化）。"""

import json
import sys

sys.path.insert(0, "scripts")
from rpc_probe import Engine  # noqa: E402

PHOTO_ID = int(sys.argv[1])
SIDECAR = sys.argv[2]

PATCH = {
    "toneCurveRGB": [{"x": 0, "y": 0}, {"x": 64, "y": 52}, {"x": 128, "y": 140}, {"x": 255, "y": 255}],
    "hsl": {"blueHue": 12, "blueSat": 25, "blueLum": -8, "orangeSat": 14},
}


def main() -> int:
    eng = Engine()
    try:
        eng.call("initialize", {"clientName": "curve-verify", "clientVersion": "1.0.0", "protocolVersion": "1.0"})
        sid = (eng.call("edit.beginSession", {"id": PHOTO_ID}).get("result") or {}).get("sessionId")
        setp = eng.call("edit.setParams", {"sessionId": sid, "patch": PATCH, "commitHistory": True})
        echoed = (setp.get("result") or {}).get("params") or {}
        print("echo toneCurveRGB =", json.dumps(echoed.get("toneCurveRGB"), ensure_ascii=False))
        print("echo hsl          =", json.dumps(echoed.get("hsl"), ensure_ascii=False))
        eng.call("edit.endSession", {"sessionId": sid, "flush": True})
    finally:
        eng.close()

    raw = open(SIDECAR, "r", encoding="utf-8", errors="replace").read()
    for tag in ("crs:ToneCurvePV2012", "lumina:ToneCurveRGB", "lumina:HSL", "crs:BlueSaturation"):
        print("%-22s %s" % (tag, "PRESENT" if ("<" + tag + ">") in raw else "-"))
    i = raw.find("<crs:ToneCurvePV2012>")
    if i >= 0:
        print("  curve value:", raw[i + 19 : raw.find("</", i)][:160])
    return 0


if __name__ == "__main__":
    sys.exit(main())
