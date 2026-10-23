# 真实 RW2 样张复测报告（WP-C2 收尾）

> 日期：2026-10-23 ｜ 执行：team-lead ｜ 库：`D:/LUMINA/build/rw2-real2`（20 张真实 PANA*.RW2，561MB，verifyMd5 导入）
> 基准对照：`docs/bench-1.1.0.md` §1.1（合成 Linear DNG，优化后 p95=642ms / max=743ms，DoD <1500ms）

## 指标

| 项 | 结果 | 门槛/对照 | 判定 |
|---|---|---|---|
| 导入 20 张（含 MD5 校验） | 2.3s | — | — |
| 缩略图 20 张（冷缓存） | 20/20 落盘（cache/thumbs/320/） | — | ✅ |
| 6K 预览 p50 | 449.0ms | 对照 571ms | — |
| 6K 预览 p95 | 457.6ms | 对照 642ms | — |
| 6K 预览 max | 465.1ms | **< 1500ms** | ✅ |
| 内嵌代理提取 p50/max | 3.6 / 4.8ms | — | — |
| 全尺寸解码 p50/max | 4616.1 / 4632.8ms | — | — |
| 编辑会话 20 个 | 0.06s | — | — |
| 导出 5 张 JPG | 13.77s | — | — |
| 原片零改写 | changed=0 missing=0 | **0 / 0** | ✅ |
| 样张原文件 | 0 改动 | **0** | ✅ |
| sidecar 落盘 | 20 个 | 20 | ✅ |

> 缩略图计时说明：`thumb.getStatus` 返回 `{items:[…]}`（在途项）而非 pending/running 计数器，
> 探针早前误用字段白等满 300s 轮询窗口；实测 20 张 320px 缩略图全部生成落盘（异步管线，
> ensureBatch 毫秒级返回 + state=ready）。探针已修正为轮询 ensureBatch 的 state。

## 引擎内存（RSS，tasklist 实测）

```
{
  "boot": 18644,
  "after_import": 20104,
  "after_thumbs": 32168,
  "after_preview": 33080,
  "after_full": 33128,
  "after_export": 33324,
  "final": 33324
}
```

## 错误

无

## 结论

**PASS**：真实 RW2 全链路达标，6K 预览 max 465.1ms < 1500ms 门槛，原片零改写成立。

## 附：本轮抓到并已修复的引擎缺陷（真实样张暴露，mock 资产未覆盖）

1. **RAW 拍摄时间量纲错误（P0）**：`raw_decoder.cpp` 把 LibRaw 的 `other.timestamp`（time_t 秒）
   直接赋给 `timestampMs`，导致所有经 `import.start` 导入的 RAW 照片 captured_at 为秒值——
   时间轴/导出命名/筛选日期全部错乱（实测 captured_at=1766968071 → 显示 1970-01-21）。
   已修：`× 1000`。用户现有 ~/.lumina 库为 mock 生成器写入的毫秒值，不受影响，无需迁移。
2. **导入重命名 `{camera}` 令牌为字面量**：`importer.cpp` 硬编码 `ctx.camera = "camera"`，
   导入文件名为 `2025-12-29_camera_PANA0079.RW2`。已修：填真实机型（DC-S5M2）。
3. **导入重命名日期用 mtime 而非 EXIF 拍摄时间**：`ctx.capturedAtMs = item.mtimeMs`，
   文件拷贝/同步后 mtime ≠ 拍摄时间时文件名日期即错。已修：先探测元数据，
   `meta.timestampMs > 0 ? meta.timestampMs : mtime`（与 DB 记录同一口径），且每文件只探测一次。
4. 回归：4 套引擎测试全绿（含 test_rpc_smoke）；修复后重跑本门槛 PASS，
   导出名恢复为 `2025-12-29_DC-S5M2_PANA0079.jpg`（默认导出模板 `{date}-{name}` 叠加库文件名日期，
   属模板语义，UI 导出页可自定义命名模板）。
