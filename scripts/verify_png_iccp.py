#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""PNG iCCP 独立校验器（QA 侧，不复用引擎任何 C++ 解析代码）。

判据来源
--------
- PNG 结构：PNG Specification 3rd Edition（签名、chunk 布局、CRC-32 覆盖 type+data）
- iCCP：PNG 3rd ed. §11.3.3.3（profileName + NUL + compressionMethod + zlib 流）
- zlib 流：RFC 1950（CMF/FLG/FCHECK/FDICT、Adler-32 尾部），RFC 1951 §3.2.4（stored 块）
- ICC 头：ICC.1:2022 §7.2（[0:4] profile size，[16:20] data colour space，
  [36:40] profile file signature 'acsp'）

被解压的 zlib 流由 ``zlib.decompress`` 处理 —— 这是真正的第三方实现（zlib 本身），
正是本脚本的独立判据：引擎自己写的 stored 块流必须能被 zlib 正确解出。

只用标准库：struct / zlib / binascii / hashlib / json / sys / pathlib。

用法
----
    python scripts/verify_png_iccp.py                  # 自动扫 build/ 下的 m4-embed-*.png
    python scripts/verify_png_iccp.py a.png b.png      # 显式指定
    python scripts/verify_png_iccp.py --ref sRGB=/tmp/srgb.icc --ref AdobeRGB=/tmp/adobe.icc

比对判据（OI-1 修复后的默认口径）
---------------------------------
`--ref` 给的基准 .icc 是**另一个进程** dump 出来的同一 profile，所以"文件里的 profile
是否等于基准"就是**跨进程确定性**的直接判据：

- **默认 = --strict-bytes**：必须**整字节完全相等**，任何差异（包括 ICC
  dateTimeNumber `[24:36]`）都是 FAIL。OI-1 修好后这条自动变绿，不靠人嘴解释。
- `--allow-timestamp-diff`：回退到旧口径（屏蔽 `[24:36]` 后比较），仅供复现历史现象。
- `--require-normalized-time`：额外要求 ICC 创建时间 == `2000-01-01T00:00:00`
  （`ICC_DATETIME_NORMALIZED`），这是"时间戳已归一化"的正面证据。

注意：基准 .icc 必须用**当前构建**重新 dump。若基准是修复前的旧文件，脚本会明确报
"基准 profile 陈旧"而不会误判成 OI-1 未关闭。

输出：build/png-iccp-verify.json ；进程退出码 0 = 全部文件通过，1 = 有失败项。
"""

from __future__ import annotations

import binascii
import json
import struct
import sys
import zlib
from pathlib import Path

PNG_SIGNATURE = b"\x89PNG\r\n\x1a\n"

# PNG 3rd ed. §11.3.3.3：profileName 1..79 字节，不得有前导/尾随/连续空格
PROFILE_NAME_MIN = 1
PROFILE_NAME_MAX = 79

# iCCP 必须出现在这些 chunk 之前（PNG 规范：iCCP 在 IDAT 与 PLTE 之前）
CHUNKS_AFTER_ICCP = ("PLTE", "IDAT", "IEND")

# ICC 头里的 dateTimeNumber（ICC.1:2022 §7.2.10）：6 个 BE uint16 = 年/月/日/时/分/秒。
#
# 历史背景（OI-1）：lcms2 构造 profile 时会写入**当前时间**，导致两个不同进程导出的
# profile 在这一段必然不同 → 导出物不可位级复现。Engineer 已把该字段**归一化**为固定值
# ICC_DATETIME_NORMALIZED，因此现在的判据是"**跨进程整字节一致**"（默认，strict）。
# 保留 mask_datetime 只为：① 复现历史现象时对照；② --allow-timestamp-diff 兼容旧判据。
ICC_OFF_DATETIME = 24
ICC_OFF_DATETIME_END = 36

# 归一化后的期望值：2000-01-01T00:00:00
ICC_DATETIME_NORMALIZED = (2000, 1, 1, 0, 0, 0)


def decode_datetime(b: bytes):
    """解出 ICC dateTimeNumber 的 6 个 uint16 → tuple（年,月,日,时,分,秒）。"""
    if len(b) < ICC_OFF_DATETIME_END:
        return None
    return struct.unpack(">6H", b[ICC_OFF_DATETIME:ICC_OFF_DATETIME_END])


def mask_datetime(b: bytes) -> bytes:
    """把 ICC dateTimeNumber 字段抹零，用于证明"除创建时间外完全一致"。"""
    if len(b) < ICC_OFF_DATETIME_END:
        return b
    return b[:ICC_OFF_DATETIME] + b"\x00" * (ICC_OFF_DATETIME_END - ICC_OFF_DATETIME) + b[
        ICC_OFF_DATETIME_END:
    ]

# 大 profile 用例的合成载荷模式（engine/tests/test_icc_embed.cpp 的 testLargeProfilePng）。
# 这不是真 ICC，只用于覆盖 zlib 多 stored 块路径，所以对它**不做** ICC 头断言 ——
# 这是"不同对象不同判据"，不是放宽断言：只有载荷与合成模式**逐字节完全相同**才走这条分支。
SYNTHETIC_LEN = 140000


def synthetic_pattern(n: int) -> bytes:
    return bytes(((i * 37 + 11) & 0xFF) for i in range(n))

# ICC 头字段偏移（ICC.1:2022 §7.2）
ICC_OFF_SIZE = 0            # 4 字节大端：整个 profile 的字节数
ICC_OFF_DATA_COLORSPACE = 16  # 4 字节：data colour space，如 b"RGB " / b"GRAY"
ICC_OFF_PCS = 20            # 4 字节：PCS，如 b"XYZ " / b"Lab "
ICC_OFF_SIGNATURE = 36      # 4 字节：profile file signature，必须是 b"acsp"
ICC_OFF_TAG_COUNT = 128     # 4 字节大端：tag 数量


def be32(buf: bytes, off: int) -> int:
    """读 4 字节大端无符号整数。"""
    return struct.unpack_from(">I", buf, off)[0]


def walk_chunks(buf: bytes):
    """逐 chunk 切分 PNG，返回 (chunks, problems)。

    chunk = length(4) + type(4) + data(length) + crc(4)，length/crc 均为大端。
    CRC 覆盖 type + data，**不覆盖 length**（PNG 3rd ed. §5.5），此处独立重算。
    """
    chunks = []
    problems = []
    p = 8  # 跳过 8 字节签名
    while p < len(buf):
        if p + 8 > len(buf):
            problems.append("chunk 头被截断（偏移 %d，剩余 %d 字节）" % (p, len(buf) - p))
            break
        length = be32(buf, p)
        ctype_bytes = buf[p + 4:p + 8]
        if length > 0x7FFFFFFF:
            problems.append("chunk 长度越界（偏移 %d，声明 %d）" % (p, length))
            break
        ctype = ctype_bytes.decode("latin-1")
        data_begin = p + 8
        crc_begin = data_begin + length
        if crc_begin + 4 > len(buf):
            problems.append(
                "chunk 数据被截断（偏移 %d，类型 %s，声明长度 %d）" % (p, ctype, length)
            )
            break
        data = buf[data_begin:crc_begin]
        stored_crc = be32(buf, crc_begin)
        # binascii.crc32 = 标准 CRC-32（多项式 0xEDB88320，反射），与 PNG 规范一致
        actual_crc = binascii.crc32(ctype_bytes + data) & 0xFFFFFFFF
        chunks.append(
            {
                "index": len(chunks),
                "type": ctype,
                "offset": p,
                "dataOffset": data_begin,
                "length": length,
                "crcStored": "%08X" % stored_crc,
                "crcComputed": "%08X" % actual_crc,
                "crcOk": stored_crc == actual_crc,
                "data": data,
            }
        )
        p = crc_begin + 4
        if ctype == "IEND":
            break
    trailing = len(buf) - p
    if trailing > 0:
        problems.append("IEND 之后还有 %d 字节垃圾数据" % trailing)
    return chunks, problems, trailing


def parse_zlib_stream_header(z: bytes):
    """按 RFC 1950 §2.2 手工校验 zlib 头（不依赖 zlib 模块的判断）。"""
    info = {
        "cmf": None,
        "flg": None,
        "cmIs8": False,
        "cinfo": None,
        "fcheckOk": False,
        "fdict": None,
        "flevel": None,
        "headerOk": False,
    }
    if len(z) < 2:
        return info
    cmf, flg = z[0], z[1]
    info["cmf"] = "0x%02X" % cmf
    info["flg"] = "0x%02X" % flg
    info["cmIs8"] = (cmf & 0x0F) == 8         # CM = 8 → deflate
    info["cinfo"] = (cmf >> 4) & 0x0F         # 窗口大小 2^(CINFO+8)
    info["fcheckOk"] = ((cmf << 8) | flg) % 31 == 0
    info["fdict"] = (flg >> 5) & 0x01         # PNG 中必须为 0（无预置字典）
    info["flevel"] = (flg >> 6) & 0x03
    info["headerOk"] = (
        info["cmIs8"]
        and info["fcheckOk"]
        and info["fdict"] == 0
        and info["cinfo"] <= 7
        and cmf == 0x78
    )
    return info


def walk_deflate_blocks(z: bytes):
    """结构性地走一遍 deflate 块头，统计块数与块类型（仅作旁证，非判定依据）。

    stored 块（BTYPE=00）可以无损跳过；一旦遇到 Huffman 块（BTYPE=01/10）说明
    profile 走了真压缩，本函数无法继续，返回 truncated=True —— 这不是错误，
    解压正确性仍以 zlib.decompress 为准。
    """
    blocks = []
    if len(z) < 2:
        return {"blocks": blocks, "truncated": True, "reason": "zlib 流过短"}
    p = 2  # 跳过 CMF/FLG
    while p < len(z):
        if p + 1 > len(z):
            return {"blocks": blocks, "truncated": True, "reason": "块头被截断"}
        hdr = z[p]
        p += 1
        bfinal = hdr & 0x01
        btype = (hdr >> 1) & 0x03
        if btype != 0:
            blocks.append({"final": bfinal, "btype": btype, "storedLen": None})
            return {
                "blocks": blocks,
                "truncated": True,
                "reason": "BTYPE=%d（Huffman），结构走查停止" % btype,
            }
        if p + 4 > len(z):
            return {"blocks": blocks, "truncated": True, "reason": "stored 块头被截断"}
        ln = struct.unpack_from("<H", z, p)[0]
        nln = struct.unpack_from("<H", z, p + 2)[0]
        p += 4
        if (ln ^ 0xFFFF) != nln:
            return {"blocks": blocks, "truncated": True, "reason": "NLEN 不是 LEN 的按位取反"}
        if p + ln > len(z):
            return {"blocks": blocks, "truncated": True, "reason": "stored 块数据被截断"}
        blocks.append({"final": bfinal, "btype": btype, "storedLen": ln})
        p += ln
        if bfinal:
            break
    return {"blocks": blocks, "truncated": False, "reason": None}


def verify_zlib_payload(z: bytes):
    """用 zlib.decompress 解压并用 zlib.adler32 独立复核 Adler-32。"""
    out = {
        "zlibHeader": parse_zlib_stream_header(z),
        "decompressOk": False,
        "decompressError": None,
        "streamFullyConsumed": False,
        "unusedBytes": None,
        "adlerStored": None,
        "adlerComputed": None,
        "adlerOk": False,
        "profile": None,
    }
    if len(z) < 6:
        out["decompressError"] = "zlib 流过短（%d 字节）" % len(z)
        return out
    # 独立算 Adler-32：zlib 流最后 4 字节是大端 Adler-32
    out["adlerStored"] = "%08X" % be32(z, len(z) - 4)
    try:
        d = zlib.decompressobj()
        profile = d.decompress(z)
        profile += d.flush()
        out["decompressOk"] = True
        out["profile"] = profile
        out["streamFullyConsumed"] = (len(d.unused_data) == 0) and (len(d.unconsumed_tail) == 0)
        out["unusedBytes"] = len(d.unused_data)
    except zlib.error as exc:  # 含 Adler-32 校验失败
        out["decompressError"] = "%s: %s" % (type(exc).__name__, exc)
        return out
    computed = zlib.adler32(out["profile"]) & 0xFFFFFFFF
    out["adlerComputed"] = "%08X" % computed
    out["adlerOk"] = out["adlerStored"] == out["adlerComputed"]
    return out


def inspect_icc_profile(profile: bytes):
    """校验 ICC 头关键字段，并给出真实字段位置（ICC.1:2022 §7.2）。"""
    info = {
        "iccBytes": len(profile),
        "declaredSizeBe32": None,
        "sizeMatchesLength": False,
        "dataColorSpace": None,      # [16:20]，如 b"RGB "
        "pcs": None,                 # [20:24]，如 b"XYZ "
        "deviceClass": None,         # [12:16]
        "fileSignature": None,       # [36:40]，应为 b"acsp"
        "fileSignatureIsAcsp": False,
        "tagCount": None,
        "tagsFitInProfile": None,
        "colorSpaceIsRgb": False,
        "dateTime": None,              # [24:36] 解出的 6 个 uint16
        "dateTimeIsNormalized": False,  # == ICC_DATETIME_NORMALIZED
        "ok": False,
    }
    if len(profile) < 132:
        return info
    info["declaredSizeBe32"] = be32(profile, ICC_OFF_SIZE)
    info["sizeMatchesLength"] = info["declaredSizeBe32"] == len(profile)
    dt = decode_datetime(profile)
    info["dateTime"] = list(dt) if dt is not None else None
    info["dateTimeIsNormalized"] = dt == ICC_DATETIME_NORMALIZED
    info["deviceClass"] = profile[12:16].decode("latin-1")
    info["dataColorSpace"] = profile[
        ICC_OFF_DATA_COLORSPACE:ICC_OFF_DATA_COLORSPACE + 4
    ].decode("latin-1")
    info["pcs"] = profile[ICC_OFF_PCS:ICC_OFF_PCS + 4].decode("latin-1")
    info["fileSignature"] = profile[
        ICC_OFF_SIGNATURE:ICC_OFF_SIGNATURE + 4
    ].decode("latin-1")
    info["fileSignatureIsAcsp"] = profile[
        ICC_OFF_SIGNATURE:ICC_OFF_SIGNATURE + 4
    ] == b"acsp"
    info["colorSpaceIsRgb"] = profile[
        ICC_OFF_DATA_COLORSPACE:ICC_OFF_DATA_COLORSPACE + 4
    ] == b"RGB "
    tag_count = be32(profile, ICC_OFF_TAG_COUNT)
    info["tagCount"] = tag_count
    # 132 + tag_count*12 是 tag 表末尾，必须不超过 profile 长度
    info["tagsFitInProfile"] = (132 + tag_count * 12) <= len(profile)
    info["ok"] = (
        info["sizeMatchesLength"]
        and info["fileSignatureIsAcsp"]
        and info["colorSpaceIsRgb"]
        and info["tagsFitInProfile"]
    )
    return info


def verify_png(path: Path, refs=None, strict_bytes=True, require_normalized_time=False):
    """校验单个 PNG 文件，返回结果 dict（含 problems 列表，空 = 通过）。"""
    refs = refs or {}
    res = {
        "file": str(path),
        "exists": path.is_file(),
        "fileBytes": 0,
        "signatureOk": False,
        "chunks": [],
        "chunkCount": 0,
        "allCrcOk": False,
        "badCrcChunks": [],
        "iCCPPresent": False,
        "iCCPIndex": None,
        "iCCPIsFirstAfterIhdr": False,
        "iCCPBeforeIdat": False,
        "ihdrFirst": False,
        "iendLast": False,
        "profileName": None,
        "profileNameLen": None,
        "profileNameAsciiPrintable": None,
        "profileNameHasIllegalSpacing": None,
        "compressionMethod": None,
        "zlibHeaderOk": False,
        "adlerOk": False,
        "iccBytes": None,
        "profileColorSpace": None,
        "iccHeaderOk": False,
        "byteEqual": None,
        "byteEqualRef": None,
        "pillowCrossCheck": "not_attempted",
        "largePattern": None,
        "problems": [],
    }
    if not res["exists"]:
        res["problems"].append("文件不存在: %s" % path)
        return res

    buf = path.read_bytes()
    res["fileBytes"] = len(buf)
    res["sha256"] = None
    import hashlib

    res["sha256"] = hashlib.sha256(buf).hexdigest()

    if len(buf) < 8 or not buf.startswith(PNG_SIGNATURE):
        res["problems"].append("PNG 签名不匹配（前 8 字节 %s）" % buf[:8].hex(" "))
        return res
    res["signatureOk"] = True

    chunks, problems, trailing = walk_chunks(buf)
    res["problems"].extend(problems)
    res["chunkCount"] = len(chunks)
    res["chunks"] = [c["type"] for c in chunks]
    res["trailingBytes"] = trailing

    bad = [c for c in chunks if not c["crcOk"]]
    res["allCrcOk"] = (len(chunks) > 0) and (len(bad) == 0) and not problems
    res["badCrcChunks"] = [
        {
            "index": c["index"],
            "type": c["type"],
            "crcStored": c["crcStored"],
            "crcComputed": c["crcComputed"],
        }
        for c in bad
    ]
    for c in bad:
        res["problems"].append(
            "chunk %d (%s) CRC 不符：文件 0x%s，实算 0x%s"
            % (c["index"], c["type"], c["crcStored"], c["crcComputed"])
        )

    if chunks:
        res["ihdrFirst"] = chunks[0]["type"] == "IHDR"
        res["iendLast"] = chunks[-1]["type"] == "IEND"
        if not res["ihdrFirst"]:
            res["problems"].append("首个 chunk 不是 IHDR，而是 %s" % chunks[0]["type"])
        if not res["iendLast"]:
            res["problems"].append("末个 chunk 不是 IEND，而是 %s" % chunks[-1]["type"])
        ihdr = chunks[0]
        if res["ihdrFirst"]:
            if ihdr["length"] != 13:
                res["problems"].append("IHDR 数据长度应为 13，实际 %d" % ihdr["length"])
            else:
                w, h, depth, ctype, comp, filt, inter = struct.unpack(">IIBBBBB", ihdr["data"])
                res["ihdr"] = {
                    "width": w,
                    "height": h,
                    "bitDepth": depth,
                    "colorType": ctype,
                    "compression": comp,
                    "filter": filt,
                    "interlace": inter,
                }
        if res["iendLast"] and chunks[-1]["length"] != 0:
            res["problems"].append("IEND 数据长度应为 0，实际 %d" % chunks[-1]["length"])

    iccp = next((c for c in chunks if c["type"] == "iCCP"), None)
    if iccp is None:
        res["problems"].append("未找到 iCCP chunk（共 %d 个 chunk）" % len(chunks))
        return res
    res["iCCPPresent"] = True
    res["iCCPIndex"] = iccp["index"]
    res["iCCPIsFirstAfterIhdr"] = iccp["index"] == 1
    idat_idx = [c["index"] for c in chunks if c["type"] == "IDAT"]
    res["iCCPBeforeIdat"] = bool(idat_idx) and iccp["index"] < min(idat_idx)
    if not res["iCCPIsFirstAfterIhdr"]:
        res["problems"].append(
            "iCCP 索引为 %d，不是 IHDR 之后的第一个 chunk" % iccp["index"]
        )
    if not res["iCCPBeforeIdat"]:
        res["problems"].append("iCCP 没有出现在所有 IDAT 之前（IDAT 索引 %s）" % idat_idx)
    for c in chunks:
        if c["index"] > iccp["index"] and c["type"] in ("PLTE",):
            res["problems"].append("iCCP 位于 PLTE 之后（索引 %d），违反 PNG 规范" % c["index"])

    # ---- 解析 iCCP data：profileName + NUL + compressionMethod + zlib 流 ----
    data = iccp["data"]
    nul = data.find(b"\x00")
    if nul < 0:
        res["problems"].append("iCCP data 中没有 profileName 的 NUL 终止符")
        return res
    name_bytes = data[:nul]
    res["profileName"] = name_bytes.decode("latin-1")
    res["profileNameLen"] = len(name_bytes)
    res["profileNameAsciiPrintable"] = all(0x20 <= b <= 0x7E for b in name_bytes)
    res["profileNameHasIllegalSpacing"] = (
        name_bytes.startswith(b" ")
        or name_bytes.endswith(b" ")
        or b"  " in name_bytes
    )
    if not (PROFILE_NAME_MIN <= len(name_bytes) <= PROFILE_NAME_MAX):
        res["problems"].append(
            "profileName 长度 %d 不在 1..79" % len(name_bytes)
        )
    if not res["profileNameAsciiPrintable"]:
        res["problems"].append("profileName 含非 ASCII 可打印字节")
    if res["profileNameHasIllegalSpacing"]:
        res["problems"].append(
            "profileName 有前导/尾随/连续空格（PNG 规范建议禁止）：%r" % res["profileName"]
        )
    if nul + 2 > len(data):
        res["problems"].append("iCCP data 在 compressionMethod 处被截断")
        return res
    res["compressionMethod"] = data[nul + 1]
    if res["compressionMethod"] != 0:
        res["problems"].append(
            "compressionMethod 应为 0，实际 %d" % res["compressionMethod"]
        )
    z = data[nul + 2:]
    res["zlibStreamBytes"] = len(z)

    zres = verify_zlib_payload(z)
    res["zlibHeader"] = zres["zlibHeader"]
    res["zlibHeaderOk"] = zres["zlibHeader"]["headerOk"]
    res["adlerOk"] = zres["adlerOk"]
    res["adlerStored"] = zres["adlerStored"]
    res["adlerComputed"] = zres["adlerComputed"]
    res["streamFullyConsumed"] = zres["streamFullyConsumed"]
    res["unusedBytes"] = zres["unusedBytes"]
    res["deflateStructure"] = walk_deflate_blocks(z)
    if not res["zlibHeaderOk"]:
        res["problems"].append("zlib 头不合法：%s" % json.dumps(zres["zlibHeader"]))
    if not zres["decompressOk"]:
        res["problems"].append("zlib.decompress 失败：%s" % zres["decompressError"])
        return res
    if not res["adlerOk"]:
        res["problems"].append(
            "Adler-32 不符：流内 %s，实算 %s" % (zres["adlerStored"], zres["adlerComputed"])
        )
    if not res["streamFullyConsumed"]:
        res["problems"].append(
            "zlib 流未被完全消费，尾部残留 %d 字节" % res["unusedBytes"]
        )

    profile = zres["profile"]
    is_synthetic = (len(profile) == SYNTHETIC_LEN) and (profile == synthetic_pattern(SYNTHETIC_LEN))
    res["syntheticPayload"] = is_synthetic
    if is_synthetic:
        res["syntheticNote"] = (
            "载荷与 testLargeProfilePng 的 (i*37+11)&0xFF 模式逐字节相同，"
            "是覆盖多 stored 块的合成载荷而非真 ICC，故不做 ICC 头断言"
        )

    icc = inspect_icc_profile(profile)
    res["icc"] = icc
    res["iccBytes"] = icc["iccBytes"]
    res["profileColorSpace"] = icc["dataColorSpace"]
    res["iccHeaderOk"] = icc["ok"] if not is_synthetic else None
    if not is_synthetic:
        if not icc["sizeMatchesLength"]:
            res["problems"].append(
                "ICC 头声明的 profile 长度 %s 与实际 %d 不符"
                % (icc["declaredSizeBe32"], icc["iccBytes"])
            )
        if not icc["fileSignatureIsAcsp"]:
            res["problems"].append(
                "ICC [36:40] 应为 'acsp'，实际 %r" % icc["fileSignature"]
            )
        if not icc["colorSpaceIsRgb"]:
            res["problems"].append(
                "ICC [16:20] data colour space 应为 'RGB '，实际 %r" % icc["dataColorSpace"]
            )
        if not icc["tagsFitInProfile"]:
            res["problems"].append(
                "ICC tag 表越界：count=%s 需要 %d 字节，profile 只有 %d"
                % (icc["tagCount"], 132 + icc["tagCount"] * 12, icc["iccBytes"])
            )

    # ---- 与基准 profile 逐字节比对 ----
    # 基准是由**另一个进程** dump 出来的同一 profile，所以这里的相等性就是"跨进程一致性"：
    #   strict（默认）→ 必须整字节相等，任何差异（含 [24:36] 时间戳）都算 FAIL。
    #                   OI-1 修好后本判据会自动通过，无需人工解释。
    #   --allow-timestamp-diff → 旧行为：屏蔽 [24:36] 后比较，仅供复现历史现象。
    for label, ref_bytes in refs.items():
        if ref_bytes is None:
            continue
        if len(ref_bytes) != len(profile):
            res["problems"].append(
                "与基准 profile %s 长度不同：文件 %d vs 基准 %d"
                % (label, len(profile), len(ref_bytes))
            )
            break
        res["byteEqualRef"] = label
        res["byteEqual"] = ref_bytes == profile
        res["sha256Profile"] = hashlib.sha256(profile).hexdigest()
        res["sha256ProfileNoTimestamp"] = hashlib.sha256(mask_datetime(profile)).hexdigest()
        res["sha256Ref"] = hashlib.sha256(ref_bytes).hexdigest()
        res["sha256RefNoTimestamp"] = hashlib.sha256(mask_datetime(ref_bytes)).hexdigest()
        res["byteEqualNoTimestamp"] = (
            res["sha256ProfileNoTimestamp"] == res["sha256RefNoTimestamp"]
        )
        diff = [i for i in range(len(profile)) if ref_bytes[i] != profile[i]]
        res["diffOffsets"] = diff[:64]
        res["diffCount"] = len(diff)
        outside = [i for i in diff if not (ICC_OFF_DATETIME <= i < ICC_OFF_DATETIME_END)]
        res["diffOffsetsOutsideTimestamp"] = outside[:64]
        dt_file = decode_datetime(profile)
        dt_ref = decode_datetime(ref_bytes)
        res["dateTimeFile"] = list(dt_file) if dt_file else None
        res["dateTimeRef"] = list(dt_ref) if dt_ref else None
        if res["byteEqual"]:
            res["byteEqualNote"] = "与基准整字节一致（跨进程确定性成立）"
        elif not strict_bytes:
            if not outside:
                res["byteEqualNote"] = (
                    "严格比较不等，差异仅在 ICC dateTimeNumber [24:36]"
                    "（--allow-timestamp-diff 模式下不算失败）"
                )
            else:
                res["problems"].append(
                    "与基准 profile %s 存在时间戳字段之外的字节差异：偏移 %s"
                    % (label, outside[:16])
                )
        else:
            file_norm = decode_datetime(profile) == ICC_DATETIME_NORMALIZED
            ref_norm = decode_datetime(ref_bytes) == ICC_DATETIME_NORMALIZED
            if not outside:
                if file_norm and not ref_norm:
                    res["problems"].append(
                        "跨进程字节不一致，且原因是**基准 profile 陈旧**："
                        "被检文件 dateTime=%s（已归一化），基准 dateTime=%s（未归一化）。"
                        "请用当前构建重新 dump 基准 .icc 后再比较 —— 这不是 OI-1 未关闭。"
                        % (res["dateTimeFile"], res["dateTimeRef"])
                    )
                elif not file_norm:
                    res["problems"].append(
                        "跨进程字节不一致：差异仅在 ICC dateTimeNumber [24:36]，"
                        "文件 %s 未归一化（期望 %s）—— OI-1 尚未关闭"
                        % (res["dateTimeFile"], list(ICC_DATETIME_NORMALIZED))
                    )
                else:
                    res["problems"].append(
                        "跨进程字节不一致：差异仅在 ICC dateTimeNumber [24:36]，"
                        "文件 %s vs 基准 %s" % (res["dateTimeFile"], res["dateTimeRef"])
                    )
            else:
                res["problems"].append(
                    "跨进程字节不一致：偏移 %s（时间戳字段之外 %s）"
                    % (diff[:16], outside[:16])
                )
        break

    # ---- ICC 创建时间是否已归一化为 2000-01-01T00:00:00（OI-1 的正面证据）----
    if not is_synthetic and res.get("icc"):
        dt = decode_datetime(profile)
        res["dateTime"] = list(dt) if dt else None
        res["dateTimeIsNormalized"] = dt == ICC_DATETIME_NORMALIZED
        if require_normalized_time and not res["dateTimeIsNormalized"]:
            res["problems"].append(
                "ICC dateTimeNumber 未归一化：实际 %s，期望 %s"
                % (list(dt) if dt else None, list(ICC_DATETIME_NORMALIZED))
            )

    # ---- 大 profile 合成模式旁证：(i*37+11)&0xFF ----
    if is_synthetic:
        res["largePattern"] = {"applies": True, "matches": True, "firstMismatch": None}
        blocks = res["deflateStructure"]["blocks"]
        res["largeProfile"] = {
            "bytes": len(profile),
            "storedBlocks": len(blocks),
            "expectedStoredBlocks": (SYNTHETIC_LEN + 65534) // 65535,
            "blockStoredLens": [b["storedLen"] for b in blocks],
            "nonFinalBlocksBfinal0": sum(
                1 for b in blocks[:-1] if b["final"] == 0 and b["storedLen"] is not None
            ),
        }
        if len(blocks) != res["largeProfile"]["expectedStoredBlocks"]:
            res["problems"].append(
                "stored 块数 %d 与 140000/65535 向上取整 %d 不符"
                % (len(blocks), res["largeProfile"]["expectedStoredBlocks"])
            )
        if any(b["btype"] != 0 for b in blocks):
            res["problems"].append("deflate 块类型不是全 stored（BTYPE != 0）")

    return res


def find_candidates(root: Path):
    """在 build/ 下找 test_icc_embed 产出的 PNG（含 large / badcrc 变体）。"""
    names = [
        "m4-embed-sRGB.png",
        "m4-embed-AdobeRGB.png",
        "m4-embed-large.png",
        "m4-embed-sRGB.png.badcrc.png",
        "m4-embed-AdobeRGB.png.badcrc.png",
    ]
    found = []
    for name in names:
        for hit in sorted(root.rglob(name)):
            found.append(hit)
    seen = set()
    out = []
    for f in found:
        key = str(f.resolve())
        if key in seen:
            continue
        seen.add(key)
        out.append(f)
    return out


def main(argv):
    root = Path(__file__).resolve().parent.parent          # D:\LUMINA
    build = root / "build"
    out_json = build / "png-iccp-verify.json"

    args = [a for a in argv[1:]]
    refs = {}
    explicit = []
    strict_bytes = True          # 默认判据：跨进程整字节必须一致（OI-1 已归一化时间戳）
    require_normalized_time = False
    i = 0
    while i < len(args):
        if args[i] == "--ref":
            if i + 1 >= len(args) or "=" not in args[i + 1]:
                print("用法: --ref <label>=<path.icc>", file=sys.stderr)
                return 2
            label, p = args[i + 1].split("=", 1)
            rp = Path(p)
            if rp.is_file():
                refs[label] = rp.read_bytes()
            else:
                print("警告: 基准 profile 不存在 %s" % p, file=sys.stderr)
            i += 2
            continue
        if args[i] == "--out":
            out_json = Path(args[i + 1])
            i += 2
            continue
        # --strict-bytes：显式声明严格判据（等价于默认行为，供 CI 命令行自证）
        if args[i] == "--strict-bytes":
            strict_bytes = True
            i += 1
            continue
        # --allow-timestamp-diff：回退到旧判据（屏蔽 [24:36] 后比较），仅供复现历史现象
        if args[i] == "--allow-timestamp-diff":
            strict_bytes = False
            i += 1
            continue
        # --require-normalized-time：要求 ICC 创建时间 == 2000-01-01T00:00:00（OI-1 正面证据）
        if args[i] == "--require-normalized-time":
            require_normalized_time = True
            i += 1
            continue
        explicit.append(args[i])
        i += 1

    targets = [Path(p) for p in explicit] if explicit else find_candidates(build)
    if not targets:
        print("未找到任何 m4-embed-*.png（先跑 ctest -R icc_embed）", file=sys.stderr)
        return 2

    # 未显式给基准时，自动挑同目录下的 .icc（若之前 dump 过）
    auto_refs = {}
    for label in ("sRGB", "AdobeRGB"):
        for cand in (
            build / ("ref-%s.icc" % label),
            Path(root) / "build" / "icc-ref" / ("%s.icc" % label),
        ):
            if cand.is_file():
                auto_refs.setdefault(label, cand.read_bytes())
    merged_refs = dict(auto_refs)
    merged_refs.update(refs)

    files = []
    any_fail = False
    for t in targets:
        # 把 refs 里的键归一：m4-embed-sRGB.png → 用 sRGB 基准
        per_refs = {}
        name = t.name
        for label, b in merged_refs.items():
            if label.lower() in name.lower():
                per_refs[label] = b
        r = verify_png(t, per_refs, strict_bytes=strict_bytes,
                       require_normalized_time=require_normalized_time)
        # 负控：文件名带 .badcrc. 的是被测实现**必须**拒绝的坏文件，
        # 它 FAIL 才是正确结果（这不是放宽断言，而是明确标注期望方向）
        r["expect"] = "reject" if ".badcrc." in name else "accept"
        r["verdict"] = (
            ("negative-control-ok" if r["problems"] else "negative-control-BROKEN")
            if r["expect"] == "reject"
            else ("pass" if not r["problems"] else "fail")
        )
        if r["verdict"] in ("fail", "negative-control-BROKEN"):
            any_fail = True
        files.append(r)

    summary = {
        "generator": "scripts/verify_png_iccp.py",
        "python": sys.version.split()[0],
        "zlibVersion": zlib.ZLIB_VERSION,
        "note": (
            "全部判据由 Python 标准库独立实现；zlib.decompress / zlib.adler32 是"
            "真正第三方实现，未复用引擎的任何 C++ 解析代码。"
        ),
        "refsLoaded": sorted(merged_refs.keys()),
        "mode": {
            "strictBytes": strict_bytes,
            "requireNormalizedTime": require_normalized_time,
            "expectedDateTime": list(ICC_DATETIME_NORMALIZED),
        },
        "files": files,
    }
    # 大 profile 若没跑出来，必须显式标 not_verified，不许默认算过
    large_hit = [f for f in files if f.get("largeProfile")]
    summary["largeProfileVerification"] = (
        "verified" if large_hit else "not_verified"
    )
    if not large_hit:
        summary["largeProfileNotVerifiedReason"] = (
            "未找到含 140000 B 合成 profile 的 PNG（build/ 下没有 m4-embed-large.png）；"
            "多 stored 块路径本次未被独立覆盖"
        )
    build.mkdir(parents=True, exist_ok=True)
    out_json.write_text(json.dumps(summary, indent=2, ensure_ascii=False), encoding="utf-8")

    for r in files:
        status = r["verdict"].upper()
        print("[%s] %s" % (status, r["file"]))
        print(
            "      chunks=%d %s" % (r["chunkCount"], ",".join(r["chunks"]) if r["chunks"] else "-")
        )
        print(
            "      allCrcOk=%s iCCPIndex=%s iccBytes=%s colorSpace=%r zlibHeaderOk=%s "
            "adlerOk=%s byteEqual=%s byteEqualNoTimestamp=%s synthetic=%s"
            % (
                r["allCrcOk"],
                r["iCCPIndex"],
                r["iccBytes"],
                r["profileColorSpace"],
                r["zlibHeaderOk"],
                r["adlerOk"],
                r["byteEqual"],
                r.get("byteEqualNoTimestamp"),
                r.get("syntheticPayload"),
            )
        )
        if r.get("byteEqualNote"):
            print("      note: %s" % r["byteEqualNote"])
        if r.get("dateTime") is not None:
            print(
                "      dateTime=%s normalized=%s (expect %s)"
                % (r["dateTime"], r.get("dateTimeIsNormalized"), list(ICC_DATETIME_NORMALIZED))
            )
        if r.get("diffOffsets"):
            print(
                "      diffOffsets=%s (outsideTimestamp=%s)"
                % (r["diffOffsets"], r.get("diffOffsetsOutsideTimestamp"))
            )
        lp = r.get("largeProfile")
        if lp:
            print(
                "      largeProfile: %d B -> %d stored blocks %s (expect %d)"
                % (
                    lp["bytes"],
                    lp["storedBlocks"],
                    lp["blockStoredLens"],
                    lp["expectedStoredBlocks"],
                )
            )
        for p in r["problems"]:
            print("      ! %s" % p)
    print("JSON -> %s" % out_json)
    print("SUMMARY: %d file(s), %s" % (len(files), "FAIL" if any_fail else "ALL EXPECTED"))
    return 1 if any_fail else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
