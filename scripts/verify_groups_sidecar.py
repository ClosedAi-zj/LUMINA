"""验证专业模式 7 标签面板的分组参数（colorGrade/detail/optics/calibration/geometry）
是否能被真引擎接受并写入 sidecar。

用法：python scripts/verify_groups_sidecar.py <photoId> <sidecarPath>
"""

import json
import sys

sys.path.insert(0, "scripts")
from rpc_probe import Engine  # noqa: E402

PHOTO_ID = int(sys.argv[1])
SIDECAR = sys.argv[2]

PATCH = {
    "exposure": 0.25,
    "temperature": 5200,
    "colorGrade": {"shadowHue": 30, "shadowSat": 12, "midtoneHue": -10, "midtoneSat": 8, "highlightHue": 45, "highlightSat": 6, "blend": 60, "balance": 5},
    "detail": {"sharpness": 42, "sharpenRadius": 1.4, "sharpenDetail": 30, "sharpenMasking": 18, "luminanceSmoothing": 22, "colorNR": 27, "colorNRDetail": 55},
    "optics": {"vignetteAmount": -30, "vignetteMidpoint": 45, "vignetteFeather": 60, "grainAmount": 12, "grainSize": 30, "grainFrequency": 40, "manualDistortion": 7, "lensProfileEnable": True},
    "calibration": {"shadowTint": -6, "redHue": 5, "redSat": 33, "greenHue": -4, "greenSat": 21, "blueHue": 9, "blueSat": 44},
    "geometry": {"hasCrop": True, "cropAngle": -2.5, "perspective": {"vertical": 18, "horizontal": -9, "rotate": 1.5, "scale": 1.08, "x": 12, "y": -7}},
}


def main() -> int:
    eng = Engine()
    try:
        eng.call("initialize", {"clientName": "groups-verify", "clientVersion": "1.0.0", "protocolVersion": "1.0"})
        begin = eng.call("edit.beginSession", {"id": PHOTO_ID})
        sid = (begin.get("result") or {}).get("sessionId")
        setp = eng.call("edit.setParams", {"sessionId": sid, "patch": PATCH, "commitHistory": True})
        echoed = (setp.get("result") or {}).get("params") or {}
        print("engine echoed keys:", sorted(echoed.keys()))
        for grp in ("colorGrade", "detail", "optics", "calibration", "geometry"):
            print("  %-12s %s" % (grp, json.dumps(echoed.get(grp), ensure_ascii=False)))
        end = eng.call("edit.endSession", {"sessionId": sid, "flush": True})
        print("endSession flushed=%s sidecarMtime=%s" % ((end.get("result") or {}).get("flushed"), (end.get("result") or {}).get("sidecarMtime")))
    finally:
        eng.close()

    raw = open(SIDECAR, "r", encoding="utf-8", errors="replace").read()
    for tag in ("colorGrade", "Detail", "Optics", "Calibration", "Geometry", "Exposure2012"):
        needle = "<lumina:%s>" % tag if tag[0].isupper() else "<crs:%s>" % tag
        i = raw.find(needle)
        if i < 0:
            alt = raw.find("<lumina:%s>" % tag)
            i = alt
        if i < 0:
            print("%-22s NOT IN SIDECAR" % tag)
        else:
            j = raw.find("</", i)
            print("%-22s %s" % (tag, raw[i:j + 1][:260]))
    print("--- sidecar size:", len(raw))
    return 0


if __name__ == "__main__":
    sys.exit(main())
