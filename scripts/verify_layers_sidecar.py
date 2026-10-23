"""端到端验证：专业模式图层栈是否真正随 edit.endSession 落入 XMP sidecar。

走真引擎 stdio RPC（与 UI 侧 api.edit.* 同一套方法）：
  initialize -> edit.beginSession -> edit.setParams(adjustmentStack + masks) -> edit.endSession(flush)
然后用 python 直接读 sidecar，打印 lumina:AdjustmentStack / lumina:Masks 内容。
"""

import json
import os
import sys

sys.path.insert(0, "scripts")
from rpc_probe import Engine  # noqa: E402

PHOTO_ID = int(sys.argv[1]) if len(sys.argv) > 1 else 1
SIDECAR = sys.argv[2] if len(sys.argv) > 2 else "D:/LUMINA/mocklib/01-fg-chuanxi/P0000001.xmp"

LAYER = {
    "id": "layer-verify-1",
    "name": "天空压暗",
    "type": "adjustment",
    "blendMode": "softLight",
    "opacity": 65,
    "enabled": True,
    "params": {"exposure": -0.45, "contrast": 12, "saturation": -8},
    "maskId": "mask-verify-1",
}

MASK = {
    "id": "mask-verify-1",
    "type": "luminanceRange",
    "enabled": True,
    "opacity": 100,
    "amount": -20,
    "low": 0.42,
    "high": 0.93,
}


def main() -> int:
    eng = Engine()
    try:
        eng.call("initialize", {"clientName": "layers-verify", "clientVersion": "1.0.0", "protocolVersion": "1.0"})
        begin = eng.call("edit.beginSession", {"id": PHOTO_ID})
        res = begin.get("result") or {}
        sid = res.get("sessionId")
        print("beginSession: sessionId=%s revision=%s paramsKeys=%d" % (sid, res.get("revision"), len(res.get("params") or {})))
        if sid is None:
            print("FATAL: 无 sessionId", json.dumps(begin, ensure_ascii=False))
            return 1

        patch = {
            "adjustmentStack": [LAYER],
            "masks": [MASK],
            "basic": {"exposure": 0.35, "contrast": 8},
        }
        setp = eng.call("edit.setParams", {"sessionId": sid, "patch": patch, "commitHistory": True})
        print("setParams: %s" % json.dumps(setp.get("result"), ensure_ascii=False))

        end = eng.call("edit.endSession", {"sessionId": sid, "flush": True})
        print("endSession: %s" % json.dumps(end.get("result"), ensure_ascii=False))
    finally:
        eng.close()

    raw = open(SIDECAR, "r", encoding="utf-8", errors="replace").read()
    for tag in ("lumina:AdjustmentStack", "lumina:Masks", "crs:Exposure2012"):
        i = raw.find("<" + tag + ">")
        if i < 0:
            print("%-24s MISSING" % tag)
            continue
        j = raw.find("</" + tag + ">", i)
        print("%-24s %s" % (tag, raw[i + len(tag) + 2 : j][:400]))
    return 0


if __name__ == "__main__":
    sys.exit(main())
