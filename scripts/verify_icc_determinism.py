#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""四格式 ICC 跨次确定性比对器（QA 侧，独立实现，不复用引擎任何解析代码）。

用途
----
OI-1 的验收判据是"**导出物里的 ICC 跨进程/跨次运行必须逐字节一致**"。本脚本从
**PNG / JPEG / TIFF / PSD** 四种导出物里各自独立地把 ICC profile 抠出来，然后比较
两批（run1 / run2）的字节是否完全相同。

独立依据（全部只用标准库 struct / zlib / binascii / hashlib / json / sys / pathlib）
- PNG  ：IHDR 之后 iCCP chunk（profileName + NUL + method=0 + zlib 流），
         逐 chunk 用 binascii.crc32 校验；profile 用 zlib.decompress 解出。
- JPEG ：APP2（0xFFE2）+ "ICC_PROFILE\0" + 序号/总数 + 分片，按序号拼回
         （ICC.1 Annex B.4）；遇 SOS/EOI 停止；分片数必须齐全。
- TIFF ：little-endian IFD 逐项走，取 tag 34675（type 7 UNDEFINED）。
- PSD  ：image resources 段里逐条 '8BIM' 走，取 resource id 0x040F。

用法
----
    # 两次运行产物分别在两个目录里（文件名 m4-embed-<space>.<ext>）
    python scripts/verify_icc_determinism.py --run1 build/qa-tmp/run1 --run2 build/qa-tmp/run2

    # 或者显式成对给：
    python scripts/verify_icc_determinism.py --pair png:a.png:b.png --pair jpg:a.jpg:b.jpg

输出：build/icc-determinism.json（可用 --out 改）。退出码 0 = 全部一致，1 = 有不一致/抽取失败。
"""

from __future__ import annotations

import binascii
import hashlib
import json
import struct
import sys
import zlib
from pathlib import Path

PNG_SIGNATURE = b"\x89PNG\r\n\x1a\n"
JPEG_ICC_SIG = b"ICC_PROFILE\x00"
ICC_OFF_DATETIME = 24
ICC_OFF_DATETIME_END = 36
ICC_DATETIME_NORMALIZED = (2000, 1, 1, 0, 0, 0)


def be32(b, off):
    return struct.unpack_from(">I", b, off)[0]


def le16(b, off):
    return struct.unpack_from("<H", b, off)[0]


def le32(b, off):
    return struct.unpack_from("<I", b, off)[0]


def decode_datetime(profile):
    if len(profile) < ICC_OFF_DATETIME_END:
        return None
    return list(struct.unpack(">6H", profile[ICC_OFF_DATETIME:ICC_OFF_DATETIME_END]))


# ---------------------------------------------------------------------------
# PNG：iCCP
# ---------------------------------------------------------------------------
def extract_png(buf):
    if len(buf) < 8 or not buf.startswith(PNG_SIGNATURE):
        raise ValueError("不是 PNG（签名不匹配）")
    p = 8
    found = None
    bad_crc = []
    while p < len(buf):
        if p + 8 > len(buf):
            raise ValueError("chunk 头截断 @%d" % p)
        ln = be32(buf, p)
        ctype = buf[p + 4:p + 8]
        data_begin = p + 8
        crc_begin = data_begin + ln
        if crc_begin + 4 > len(buf):
            raise ValueError("chunk 数据截断 @%d（%s）" % (p, ctype))
        data = buf[data_begin:crc_begin]
        stored = be32(buf, crc_begin)
        calc = binascii.crc32(ctype + data) & 0xFFFFFFFF
        if stored != calc:
            bad_crc.append({"type": ctype.decode("latin-1"), "offset": p,
                            "stored": "%08X" % stored, "computed": "%08X" % calc})
        if ctype == b"iCCP" and found is None:
            nul = data.find(b"\x00")
            if nul < 0:
                raise ValueError("iCCP 缺 profileName 的 NUL")
            if data[nul + 1] != 0:
                raise ValueError("compressionMethod != 0（实际 %d）" % data[nul + 1])
            found = zlib.decompress(data[nul + 2:])
        p = crc_begin + 4
        if ctype == b"IEND":
            break
    if found is None:
        raise ValueError("未找到 iCCP chunk")
    if bad_crc:
        raise ValueError("chunk CRC 不符: %s" % bad_crc[:3])
    return found


# ---------------------------------------------------------------------------
# JPEG：APP2 ICC_PROFILE 分片
# ---------------------------------------------------------------------------
def extract_jpeg(buf):
    if len(buf) < 4 or buf[0] != 0xFF or buf[1] != 0xD8:
        raise ValueError("不是 JPEG（缺 SOI）")
    parts = {}
    total = None
    p = 2
    while p + 4 <= len(buf):
        if buf[p] != 0xFF:
            raise ValueError("段结构错位 @%d" % p)
        marker = buf[p + 1]
        if marker == 0xD9 or marker == 0xDA:      # EOI / SOS
            break
        if marker == 0x01 or 0xD0 <= marker <= 0xD7 or marker == 0xFF:
            p += 2
            continue
        ln = (buf[p + 2] << 8) | buf[p + 3]
        if ln < 2 or p + 2 + ln > len(buf):
            raise ValueError("段长度越界 @%d" % p)
        payload = buf[p + 4:p + 2 + ln]
        if marker == 0xE2 and payload.startswith(JPEG_ICC_SIG) and len(payload) >= 14:
            seq = payload[12]
            tot = payload[13]
            if total is None:
                total = tot
            elif total != tot:
                raise ValueError("ICC 分片总数字段不一致（%s vs %s）" % (total, tot))
            parts[seq] = payload[14:]
        p += 2 + ln
    if not parts:
        raise ValueError("未找到 APP2 ICC_PROFILE")
    if total is None or len(parts) != total:
        raise ValueError("ICC_PROFILE 分片不齐：%d/%s" % (len(parts), total))
    return b"".join(parts[i] for i in range(1, total + 1))


# ---------------------------------------------------------------------------
# TIFF：tag 34675
# ---------------------------------------------------------------------------
def extract_tiff(buf):
    if len(buf) < 8 or buf[0:2] != b"II" or le16(buf, 2) != 42:
        raise ValueError("不是 little-endian TIFF")
    ifd = le32(buf, 4)
    if ifd + 2 > len(buf):
        raise ValueError("IFD 偏移越界")
    n = le16(buf, ifd)
    for i in range(n):
        e = ifd + 2 + i * 12
        if e + 12 > len(buf):
            raise ValueError("IFD entry 越界")
        tag = le16(buf, e)
        if tag != 34675:
            continue
        count = le32(buf, e + 4)
        off = le32(buf, e + 8)
        if off + count > len(buf):
            raise ValueError("ICC 偏移/长度越界（off=%d count=%d）" % (off, count))
        return buf[off:off + count]
    raise ValueError("未找到 tag 34675(ICC)")


# ---------------------------------------------------------------------------
# PSD：image resource 0x040F
# ---------------------------------------------------------------------------
def extract_psd(buf):
    if len(buf) < 30 or buf[0:4] != b"8BPS":
        raise ValueError("不是 PSD（缺 8BPS）")
    p = 26
    cm_len = be32(buf, p)
    p += 4 + cm_len
    if p + 4 > len(buf):
        raise ValueError("image resources 长度字段越界")
    res_len = be32(buf, p)
    p += 4
    end = p + res_len
    if end > len(buf):
        raise ValueError("image resources 段越界")
    while p + 12 <= end:
        if buf[p:p + 4] != b"8BIM":
            raise ValueError("resource 签名不是 8BIM @%d" % p)
        rid = (buf[p + 4] << 8) | buf[p + 5]
        q = p + 6
        name_len = buf[q]
        q += 1 + name_len
        if (1 + name_len) % 2:
            q += 1
        if q + 4 > end:
            raise ValueError("resource 长度字段越界 @%d" % p)
        dlen = be32(buf, q)
        q += 4
        if q + dlen > end:
            raise ValueError("resource 数据越界 @%d" % p)
        if rid == 0x040F:
            return buf[q:q + dlen]
        p = q + dlen + (dlen % 2)
    raise ValueError("未找到 resource 0x040F(ICC)")


EXTRACTORS = {
    "png": extract_png,
    "jpg": extract_jpeg,
    "jpeg": extract_jpeg,
    "tif": extract_tiff,
    "tiff": extract_tiff,
    "psd": extract_psd,
}


def probe(path: Path):
    """抽出一个文件里的 ICC，返回 (profile, info)。失败则 info.error 非空。"""
    info = {"file": str(path), "exists": path.is_file(), "error": None,
            "iccBytes": None, "sha256": None, "dateTime": None,
            "dateTimeIsNormalized": None}
    if not info["exists"]:
        info["error"] = "文件不存在"
        return None, info
    ext = path.suffix.lower().lstrip(".")
    fn = EXTRACTORS.get(ext)
    if fn is None:
        info["error"] = "不支持的扩展名: %s" % ext
        return None, info
    try:
        profile = fn(path.read_bytes())
    except (ValueError, zlib.error) as exc:
        info["error"] = "%s: %s" % (type(exc).__name__, exc)
        return None, info
    info["iccBytes"] = len(profile)
    info["sha256"] = hashlib.sha256(profile).hexdigest()
    dt = decode_datetime(profile)
    info["dateTime"] = dt
    info["dateTimeIsNormalized"] = (dt == list(ICC_DATETIME_NORMALIZED))
    return profile, info


def compare_pair(label, p1: Path, p2: Path):
    prof1, info1 = probe(p1)
    prof2, info2 = probe(p2)
    row = {"label": label, "run1": info1, "run2": info2,
           "byteEqual": None, "problems": []}
    if prof1 is None:
        row["problems"].append("run1 抽取失败: %s" % info1["error"])
    if prof2 is None:
        row["problems"].append("run2 抽取失败: %s" % info2["error"])
    if prof1 is not None and prof2 is not None:
        row["byteEqual"] = prof1 == prof2
        if len(prof1) != len(prof2):
            row["problems"].append(
                "长度不同: run1 %d vs run2 %d" % (len(prof1), len(prof2)))
        elif not row["byteEqual"]:
            diff = [i for i in range(len(prof1)) if prof1[i] != prof2[i]]
            outside = [i for i in diff
                       if not (ICC_OFF_DATETIME <= i < ICC_OFF_DATETIME_END)]
            row["diffOffsets"] = diff[:32]
            row["diffOffsetsOutsideTimestamp"] = outside[:32]
            if not outside:
                row["problems"].append(
                    "仅 ICC dateTimeNumber [24:36] 不同（%s vs %s）→ OI-1 未关闭"
                    % (info1["dateTime"], info2["dateTime"]))
            else:
                row["problems"].append(
                    "存在时间戳之外的字节差异，偏移 %s" % outside[:16])
    return row


def main(argv):
    root = Path(__file__).resolve().parent.parent
    out_json = root / "build" / "icc-determinism.json"
    run1 = run2 = None
    pairs = []
    i = 1
    while i < len(argv):
        a = argv[i]
        if a == "--run1":
            run1 = Path(argv[i + 1]); i += 2; continue
        if a == "--run2":
            run2 = Path(argv[i + 1]); i += 2; continue
        if a == "--out":
            out_json = Path(argv[i + 1]); i += 2; continue
        if a == "--pair":
            spec = argv[i + 1].split(":")
            if len(spec) != 3:
                print("用法: --pair <ext>:<path1>:<path2>", file=sys.stderr)
                return 2
            pairs.append((spec[0], Path(spec[1]), Path(spec[2])))
            i += 2
            continue
        print("未知参数: %s" % a, file=sys.stderr)
        return 2

    if run1 and run2:
        for space in ("sRGB", "AdobeRGB"):
            for ext in ("png", "jpg", "tif", "psd"):
                n = "m4-embed-%s.%s" % (space, ext)
                pairs.append(("%s/%s" % (space, ext), run1 / n, run2 / n))
    if not pairs:
        print("至少给 --run1/--run2 或 --pair", file=sys.stderr)
        return 2

    rows = [compare_pair(label, p1, p2) for label, p1, p2 in pairs]
    any_fail = any(r["problems"] for r in rows)
    doc = {
        "generator": "scripts/verify_icc_determinism.py",
        "python": sys.version.split()[0],
        "zlibVersion": zlib.ZLIB_VERSION,
        "expectedDateTime": list(ICC_DATETIME_NORMALIZED),
        "note": "四格式 ICC 独立抽取（PNG iCCP / JPEG APP2 / TIFF tag 34675 / PSD 0x040F），"
                "跨两次导出比字节；未复用引擎解析代码。",
        "pairs": rows,
        "allEqual": not any_fail,
    }
    out_json.parent.mkdir(parents=True, exist_ok=True)
    out_json.write_text(json.dumps(doc, indent=2, ensure_ascii=False), encoding="utf-8")

    for r in rows:
        st = "OK" if not r["problems"] else "FAIL"
        print("[%s] %-12s byteEqual=%s icc=%s/%s dateTime=%s/%s" % (
            st, r["label"], r["byteEqual"],
            r["run1"]["iccBytes"], r["run2"]["iccBytes"],
            r["run1"]["dateTime"], r["run2"]["dateTime"]))
        for p in r["problems"]:
            print("      ! %s" % p)
    print("JSON -> %s" % out_json)
    print("SUMMARY: %d pair(s), %s" % (len(rows), "FAIL" if any_fail else "ALL BYTE-EQUAL"))
    return 1 if any_fail else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
