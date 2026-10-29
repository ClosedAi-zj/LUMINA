# LUMINA IPC 协议规范 v1.0（stdio JSON-RPC 2.0）

> 本文是 **C++ 引擎工程师** 与 **Electron/UI 工程师** 并行开发的唯一契约。
> 任何字段名、错误码、事件名的增改必须先修订本文（版本化），不允许口头传递。
> 上位文档：`docs/架构设计-LUMINA-v1.0.0.md`；参数 ↔ XMP 字段映射以 `specs/XMP-字段映射表.md` 为准。
> **修订记录**：v1.0.0（2026-10-22）首版 · v1.0.1（2026-10-22）：`edit.endSession` 合并为唯一版定义（返回 `{flushed, params, revision, sidecarMtime}`，`flush` 默认 `true`）；§5.7 新增 `raw.selfTest`（L1 判定）；§5.15 新增 `MockSpec` 字段与 `dngSelfTest` 返回。

---

## 1. 传输与帧格式

- 通道：`lumina-engine.exe` 的 **stdin / stdout**（UTF-8 JSON，无 BOM）。**stdout 只允许输出协议帧，日志一律走 stderr 与日志文件**。
- 帧：`4 字节小端 uint32 载荷长度 + 载荷字节`。单帧上限 **8 MiB**（超限即断开并记日志）。
- 二进制数据（缩略图 / 预览 / 渲染产物）**一律不进 JSON**，引擎落盘后只传**绝对路径**。
- UI 侧是客户端（发请求），引擎侧是服务端（发请求方仅 UI；引擎 → UI 只发**通知**，见 §6）。

## 2. 信封（JSON-RPC 2.0）

```jsonc
// 请求（UI → 引擎）
{ "jsonrpc": "2.0", "id": 42, "method": "library.queryPhotos", "params": { } }
// 成功响应
{ "jsonrpc": "2.0", "id": 42, "result": { } }
// 失败响应
{ "jsonrpc": "2.0", "id": 42, "error": { "code": 1001, "message": "file not found", "data": { } } }
// 通知（引擎 → UI，无 id，UI 不回包）
{ "jsonrpc": "2.0", "method": "event.job.progress", "params": { } }
```

- `id` 由 UI 侧递增分配（int64）；引擎必须**原样回显**。
- 通知可**穿插于任意响应之间**到达，UI 必须容忍乱序（同一 jobId 的 progress 事件保序）。
- 未识别的方法：引擎返回 `-32601`，**不得崩溃**。
- 协议版本：`initialize` 握手确认；主版本不一致时 UI 立即降级退出并提示。

## 3. 错误码总表

| 码 | 名称 | 含义 / UI 呈现要求 |
|---|---|---|
| -32700 | PARSE_ERROR | JSON 解析失败（帧内非法 JSON） |
| -32600 | INVALID_REQUEST | 非法信封 |
| -32601 | METHOD_NOT_FOUND | 方法未注册 |
| -32602 | INVALID_PARAMS | 参数校验失败，`data.field` 指明字段 |
| -32603 | INTERNAL_ERROR | 引擎内部错误，`data.stack` 附日志摘要 |
| 1001 | FILE_NOT_FOUND | 文件/目录不存在 |
| 1002 | IO_ERROR | 磁盘 IO 失败 |
| 1003 | DB_ERROR | SQLite 操作失败 |
| 1004 | XMP_PARSE_ERROR | sidecar 解析失败（UI 提示隔离该文件，不静默） |
| 1005 | XMP_EXTERNAL_CONFLICT | sidecar 被外部修改（`data:{sidecarMtime,knownMtime}`）→ UI 必须弹「重读/覆盖/另存」三选 |
| 1006 | SCHEMA_TOO_NEW | ProcessVersion/SchemaVersion 高于引擎认知 → UI 提示「部分参数可能不显示」 |
| 1007 | XMP_WRITE_FAILED | 原子写失败（磁盘满/只读） |
| 2001 | RAW_DECODE_FAILED | LibRaw 解码失败 |
| 2002 | UNSUPPORTED_FORMAT | 不支持的格式 |
| 2003 | PROXY_MISSING | 内嵌代理提取失败 |
| 3001 | JOB_NOT_FOUND | 任务不存在 |
| 3002 | JOB_BUSY | 同类任务已在运行 |
| 3003 | JOB_CANCELLED | 任务已取消（正常语义，UI 不报错弹窗） |
| 3004 | JOB_FAILED | 任务失败，`data.reason` 必填 |
| 4001 | INSUFFICIENT_SPACE | 目标剩余空间不足（黄条预警阈值由 UI 判定，此码=硬不足） |
| 4002 | MD5_MISMATCH | 校验失败 → 必须进隔离视图（红线 R3） |
| 4003 | SOURCE_UNREADABLE | 源不可读（触发整夹缺失三选项流程） |
| 5001 | CODEC_UNAVAILABLE | 编码器不可达，`data.alternative` 给出可用的降级编码（红线：不得产出坏文件） |
| 5002 | ENGINE_SHUTTING_DOWN | 引擎退出中 |
| 6001 | GALLERY_CONFLICT | 画廊重复发布/密码冲突 |

## 4. 通用类型定义

> JSON 字段一律 **camelCase**；时间一律 **epoch 毫秒（UTC）**；路径一律绝对路径正斜杠 `D:/...`。

```jsonc
PhotoSummary {
  id: int64, fileName: string, filePath: string,
  format: "RW2"|"JPG"|"DNG"|"ARW"|"MP4"|"PSD"|"PNG",
  width: int, height: int,
  capturedAt: int64, importedAt: int64,
  camera: string, lens: string, iso: int, aperture: string, shutter: string, focalLength: string,
  gpsLat: number|null, gpsLng: number|null,        // 来自 sidecar exif: 命名空间优先，其次原 EXIF
  rating: 0|1|2|3|4|5, pick: "pick"|"reject"|"none", colorLabel: string|null,
  hasEdits: bool,           // sidecar crs: 非空
  hasGps: bool, isMissing: bool, isVideo: bool,
  pairKind: "raw+jpg"|"single",   // RAW+JPG 配对为一条目，JPG 不单列
  thumbState: "ready"|"pending"|"failed", thumbPath: string|null,
  seqId: int64|null
}

PhotoDetail = PhotoSummary + {
  md5: string|null, fileSize: int64,
  sidecarPath: string|null, sidecarMtime: int64|null,
  whiteBalance: "asShot"|"custom", folderId: int64, rootId: int64,
  keywords: string[], description: string,
  seqInfo: { seqId, name, frameNo, frameCount }|null,
  health: "ok"|"missing"|"corrupt"|"unchecked",
  backup: { backedUp: bool, backupPath: string|null, verifiedAt: int64|null }
}

Filter {         // 多维 AND
  folderIds?: int64[], rootIds?: int64[],
  formats?: string[], cameras?: string[], lenses?: string[],
  minRating?: 1|2|3|4|5, pick?: "pick"|"reject"|"none",
  located?: bool, edited?: bool, missing?: bool, isVideo?: bool,
  dateFrom?: int64, dateTo?: int64, locationIds?: int64[], seqId?: int64
}
Sort   { field: "capturedAt"|"importedAt"|"fileName"|"rating", order: "asc"|"desc" }  // 默认 capturedAt asc
Page   { offset: int = 0, limit: int = 200 /*≤500*/ }

QueryResult { total: int, items: PhotoSummary[] }

EditParams {   // 与 XMP 映射表 §1–§5 一一对应，此处为内部模型（写 sidecar 的唯一来源）
  processVersion: "15.4",
  whiteBalance: "asShot"|"custom", temperature?: number, tint?: number,
  exposure: number,             // -5.0..+5.0 EV   → crs:Exposure2012
  contrast: number, highlights: number, shadows: number, whites: number, blacks: number,  // -100..100
  clarity: number, texture: number, dehaze: number, vibrance: number, saturation: number,
  toneCurveRGB: {x:number,y:number}[], toneCurveR?: Curve, toneCurveG?: Curve, toneCurveB?: Curve,
  parametric?: { highlights, lights, darks, shadows, shadowSplit, lightSplit, midSplit },
  hsl: { red:Hsl, orange:Hsl, yellow:Hsl, green:Hsl, aqua:Hsl, blue:Hsl, purple:Hsl, magenta:Hsl },   // Hsl{hue,sat,lum}；orange 为 1.1.0 新增（8 色带 × 3 = 24 字段，crs:Hue/Saturation/LuminanceAdjustmentOrange）
  colorGrade: { shadowHue, shadowSat, midtoneHue, midtoneSat, highlightHue, highlightSat, blend, balance },
  calibration: { shadowTint, redHue, redSat, greenHue, greenSat, blueHue, blueSat },
  detail: { sharpness, sharpenRadius, sharpenDetail, sharpenMasking, luminanceSmoothing, colorNR, colorNRDetail },
  optics: { lensProfileEnable: bool, manualDistortion, vignetteAmount, vignetteMidpoint, vignetteFeather,
            grainAmount, grainFrequency, grainSize },
  geometry: { hasCrop: bool, cropTop?, cropBottom?, cropLeft?, cropRight?, cropAngle?,
              perspective: { vertical, horizontal, rotate, scale, aspect, x, y } },
  masks: MaskLayer[],
  removals?: RemovalArea[],               // crs:RemoveAreas + pm_*
  adjustmentStack?: AdjustmentLayer[]     // lumina:AdjustmentStack
}

MaskLayer {
  id: string, type: "linear"|"radial"|"brush"|"aiSky"|"aiSubject"|"aiBackground"|"luminanceRange"|"colorRange"|"depth",
  enabled: bool, opacity: number,           // 0..100
  amount: number,                            // 主强度（渐变/径向的 -100..100）
  geometry?: { x1,y1,x2,y2 } | { cx,cy,radius,ratio } ,
  strokes?: { x,y,radius,flow,density,erase:bool }[],
  range?: { low:number, high:number, samples: number[] },
  local: { exposure, contrast, highlights, shadows, temperature, tint, saturation, clarity, dehaze }
}

AdjustmentLayer { id: string, name: string, kind: string, enabled: bool,
                  blendMode: "normal"|"multiply"|"screen"|"overlay"|string, opacity: number, params: object }

JobInfo { jobId: int64, kind: string, state: "queued"|"running"|"done"|"failed"|"cancelled",
          progress: number /*0..100*/, detail: string, etaMs: int|null }
```

## 5. 方法清单

### 5.0 系统与握手（5 个）

| 方法 | 参数 → 返回 |
|---|---|
| `system.initialize` | `{uiVersion}` → `{protocolVersion:"1.0", engineVersion, capabilities:{libraw:true, fts5:true, codecs:["h264"|"h265"...]\|"none", mock:true}, homeDir, dbPath}` |
| `system.shutdown` | `{}` → `{}`（引擎 flush 所有 sidecar 与 DB 后退出，退出码 0） |
| `system.ping` | `{}` → `{pong, uptimeMs}` |
| `system.getSettings` | `{}` → `{settings: {theme:"dark", homeDir, libraryRoots[], backupTarget, thumbCacheQuotaMB, ...}}` |
| `system.setSettings` | `{patch: object}` → `{settings}`（合并写入 settings.json） |
| `system.listVolumes` | `{}` → `[{path, label, totalBytes, freeBytes, isRemovable}]`（供导入向导 SD 卡检测） |

### 5.1 图库索引与查询（12 个）

| 方法 | 参数 → 返回 |
|---|---|
| `library.getStats` | `{}` → `{totalPhotos, totalBytes, verified, missing, edited, byFormat:{RW2,JPG,...}, months:[{month:"2026-04",count}]}` |
| `library.queryPhotos` | `{filter?: Filter, sort?: Sort, page: Page}` → `QueryResult`（预编译 SQL，路径必走索引） |
| `library.search` | `{text, filter?: Filter, page: Page}` → `QueryResult`（FTS5：文件名+地点+日期；目标 <100ms） |
| `library.getFacets` | `{filter?: Filter}` → `{cameras:[{value,count}], lenses:[...], formats:[...], ratings:[...], locations:[...]}`（当前筛选生效下的可选项计数） |
| `library.getTimeline` | `{filter?: Filter}` → `[{month:"2025-12", count, firstPhotoId}]`（按月柱状，点击跳转用 firstPhotoId） |
| `library.getFolderTree` | `{}` → `[{id, rootId, path, name, count, missing, children:[...]}]`（5 根目录可折叠） |
| `library.getPhotoDetail` | `{id}` → `PhotoDetail` |
| `library.updatePhotos` | `{ids: int64[], patch: {rating?, pick?, colorLabel?, keywords?, description?}}` → `{updated:n}`（写 sidecar 对应字段，不碰原片） |
| `library.removePhotos` | `{ids}` → `{removed:n}`（**仅移索引**，原片与 sidecar 不动；记日志供撤销） |
| `library.restorePhotos` | `{ids}` → `{restored:n}`（5s 撤销窗口的后端支撑） |
| `library.listSmartCollections` | `{}` → `[{id, name, filter}]`（≥6 条预设规则） |
| `library.runSmartCollection` | `{id, page}` → `QueryResult` |

### 5.2 相册 / 精选集 / 画册（5 个）

| 方法 | 参数 → 返回 |
|---|---|
| `albums.list` | `{kind?: "collection"|"portfolio"}` → `[{id, name, kind, itemCount, coverPhotoId, published?}]` |
| `albums.create` | `{name, kind}` → `{id}` |
| `albums.addPhotos` | `{albumId, ids}` → `{added:n}` |
| `albums.removePhotos` | `{albumId, ids}` → `{removed:n}` |
| `albums.reorder` | `{albumId, orderedIds}` → `{}`（画册拖拽排序持久化） |

### 5.3 缩略图与预览（4 个）

| 方法 | 参数 → 返回 |
|---|---|
| `thumb.ensureBatch` | `{ids, size: 320|640}` → `{items:[{id, state:"ready"|"pending"|"failed", path?}]}`；就绪后逐张发 `event.thumb.ready`（UI 拿 path 用 `<img file://>` 加载） |
| `thumb.getStatus` | `{ids, size}` → 同上（不发事件，轮询用） |
| `preview.getBase` | `{id}` → `{path, width, height, source:"libraw"|"embedded-jpeg"|"native"}`（修图台底图：代理缺失时同步解码并缓存；离线原片返回 1001 且 UI 禁编辑导出） |
| `cache.clear` | `{scope: "thumbs"|"previews"|"renders"}` → `{freedBytes}` |

### 5.4 XMP 读写（5 个）

| 方法 | 参数 → 返回 |
|---|---|
| `xmp.read` | `{id}` → `{params: EditParams, schemaVersion, processVersion, warnings: string[], sidecarPath, sidecarMtime, foreignNamespaces: string[]}`（未知命名空间原样缓存于引擎内存，回写吐回；crs 与 lumina 合并进 params；lrt: 单独通道并入 exposureRamp 相关字段） |
| `xmp.write` | `{id, params: EditParams}` → `{sidecarMtime}`（原子写 temp+rename；写前 mtime 比对，外部修改 → **1005**，绝不覆盖） |
| `xmp.checkExternal` | `{id}` → `{changed: bool, sidecarMtime}` |
| `xmp.listNamespaces` | `{id}` → `[{prefix, uri, fieldCount}]`（调试/呈现用） |
| `xmp.migrationStatus` | `{}` → `{current:1, distribution:{"0":n,"1":n}, migrated:n}`（启动扫描 SchemaVersion 分布） |

### 5.5 编辑会话（快速/专业共用）（12 个）

> 会话模型：UI 进入修图台调 `edit.beginSession`；滑块变更调 `edit.setParams`（引擎持有内存会话 + 历史栈；**引擎侧 500ms 防抖落盘 sidecar**）；**关图 / 切图 / 退出修图台必须调 `edit.endSession {sessionId, flush:true}`**（跳过防抖强制落盘，参数以 sidecar 为准）。

| 方法 | 参数 → 返回 |
|---|---|
| `edit.beginSession` | `{id}` → `{sessionId, params, revision, historyLength}` |
| `edit.setParams` | `{sessionId, patch: object /*Partial<EditParams>*/, commitHistory: bool=true}` → `{params, revision}`（commitHistory=false 用于滑块拖动中间态） |
| `edit.undo` | `{sessionId}` → `{params, revision, historyIndex}` |
| `edit.redo` | `{sessionId}` → 同上 |
| `edit.endSession` | `{sessionId, flush: bool=true}` → `{flushed: bool, params: EditParams, revision: int, sidecarMtime: int\|null}`。**唯一定义**：销毁内存会话；`flush=true`（默认）= 立即写 sidecar（跳过 500ms 防抖，走原子写 temp+rename）并返回最终参数/revision/mtime，UI 凭 `sidecarMtime` 熄灭「参数已写入 XMP」指示灯；`flush=false` = **仅销毁会话不落盘**，丢弃防抖窗口内未落盘的 patch（仅用于用户显式放弃本次编辑；已落盘的历史步骤不受影响） |
| `edit.getSnapshots` | `{id}` → `[{snapshotId, name, createdAt}]` |
| `edit.createSnapshot` | `{id, name}` → `{snapshotId}` |
| `edit.restoreSnapshot` | `{id, snapshotId}` → `{params}` |
| `vcopy.create` | `{id, name?}` → `{vcopyId}`（lumina:VirtualCopyOf，指向源文件相对路径） |
| `vcopy.list` | `{id}` → `[{vcopyId, name, createdAt}]` |
| `presets.list` | `{}` → `[{id, name, params}]`（余晖斜刺/苍穹·山岚/深夜深色/建筑·透视/日出金顶/风光·基础…） |
| `presets.apply` | `{presetId, targetIds}` → `{applied:n}`（仅写 sidecar） |

### 5.6 参数同步 / 序列（2 个）

| 方法 | 参数 → 返回 |
|---|---|
| `edit.syncToSelection` | `{sourceId, targetIds, includeMasks: bool}` → `{updated:n}`（目标 sidecar 参数与源一致） |
| `edit.syncToSequence` | `{sourceId, seqId, includeMasks: bool}` → `{updated:n}` |

### 5.7 RAW 解码与渲染（4 个）

| 方法 | 参数 → 返回 |
|---|---|
| `raw.decode` | `{id, scale: "full"|"half", purpose: "preview"|"export"}` → `{path, width, height, source:"libraw"|"embedded-jpeg", decodeMsP50?}`（LibRaw 真解码 → sRGB JPEG 落 cache；>2s 转异步 job 并发 `event.job.*`；内嵌 JPEG 仅作 L2 兜底） |
| `raw.getProxy` | `{id}` → `{path, width, height}` 或错误 2003（内嵌 JPEG 提取；模拟资产为 DNG 内嵌 preview IFD） |
| `raw.selfTest` | `{sample?: int=32, ids?: int64[]}` → `{ok, sampled, succeeded, decodeMsP50, decodeMsP95, failures:[{photoId, error}]}`。**L1 判定**：抽样走 `LibRaw::open_file → unpack → dcraw_process → copy_mem_image`，要求 100% 成功且输出均值 ∈ [16,240]（非全黑/非全白） |
| `analysis.histogram` | `{id, edited: bool}` → `{r:[256], g:[256], b:[256]}`（edited=true 时按当前 params 的 CPU 近似管线计算） |
| `analysis.getExposedRange` | `{id}` → `{underexposedPct, overexposedPct}`（曝光溢出提示） |

### 5.8 导入与去重（6 个）

| 方法 | 参数 → 返回 |
|---|---|
| `import.scanSource` | `{source:{kind:"volume"|"folder", path}}` → `{jobId}`；完成事件 result：`{totalFiles, totalBytes, byFormat:{}, duplicates, rawWithJpg, jpgOnly, unreadable:[{path, error}]}`（读失败标红计入，不中断） |
| `import.checkTarget` | `{targetPath, requiredBytes}` → `{freeBytes, ratio, warning: bool}`（<110% 黄条由 UI 呈现） |
| `import.start` | `{source, target, backup:{enabled, targetPath, timing:"onImport"|"nightly"}, verifyMd5: bool, renameTemplate: string, dedupe:"skip", pairRawJpg: true, postActions:{enterCulling: bool, applyPreset: string|null}}` → `{jobId}`；事件见 §6 |
| `import.cancel` | `{jobId}` → `{}`（执行前任意一步可取消无残留） |
| `import.resolveMissingFolder` | `{path, action:"relocate"|"orphan"|"backfill", newPath?}` → `{affected:n}`（**禁止静默跳过**，三选项后端） |
| `import.listRecentSources` | `{}` → `[string]`（记忆最近 5 路径） |

### 5.9 存储健康（11 个）

| 方法 | 参数 → 返回 |
|---|---|
| `health.getStatus` | `{}` → `{verified, total, missing, corrupt, backup:{state:"ok"|"warning"|"none", lastSuccessAt, target}, md5:{percent, running}, abnormal: bool}` |
| `health.scanFull` | `{}` → `{jobId}`（启动兜底全量扫描；文件系统事件实时监测由引擎自动开启，事件见 §6） |
| `health.listIssues` | `{kind: "missing"|"corrupt", page}` → `{total, items:[{issueId, photoId, path, lastKnown, kind, detectedAt}]}` |
| `health.resolveIssue` | `{issueId, action:"relocate"|"orphan"|"backfill"|"restore", newPath?}` → `{resolved:n}` |
| `health.restoreFromBackup` | `{photoId}` → `{jobId}`（从第二目录/NAS 替身恢复） |
| `health.verifyPhotos` | `{ids?: int64[]}` → `{jobId}`（批量 MD5；失败逐条发 `event.health.issue`） |
| `health.cancelJob` | `{jobId}` → `{cancelled: bool}`（@since 1.1.0；JobManager 取消包装 + job 存在性校验，非 health 域 job 也可取消） |
| `health.exportDiffReport` | `{format: "csv"|"json", path?: string}` → `{path, count}`（@since 1.1.0；对比主库与备份根差异，三类：missing 库有未备份 / extra 备份有多余文件 / checksum_failed 大小或 MD5 不符；默认落 `{home}/reports/`） |
| `health.backupNow` | `{target?: string}` → `{jobId}`（@since 1.1.0；原片备份 job，完成 result `{backedUp, skippedVerified, total}`；增量语义：备份文件存在+大小+MD5 一致者跳过，首跑全量；target 缺省用 settings.backupTarget，显式传入则持久化；未配置返回明确错误，不静默选盘） |
| `health.setBackupTarget` | `{path}` → `{enabled, targetPath, timing, lastRunAt}`（@since 1.1.0；写 settings 并持久化，目录不存在则创建、失败显式报错；读回走 `health.getBackupConfig`） |
| `health.getBackupConfig` | `{}` → `{enabled, targetPath, timing, lastRunAt}`（修改走 system.setSettings） |

### 5.10 导出队列（6 个）

| 方法 | 参数 → 返回 |
|---|---|
| `export.createJob` | `{photoIds, spec:{format:"jpg"|"tiff"|"png"|"psd", longEdge, colorSpace:"sRGB"|"AdobeRGB", quality, psdKeepLayers: bool, watermark:{enabled, position:"bottom-right", scale: 0.88, opacity, text?, imagePath?}, namingTemplate, outputDir}}` → `{jobId}`；完成事件 result：`{outputs:[{photoId, path}]}` |
| `export.listJobs` | `{state?}` → `[JobInfo]` |
| `export.cancelJob` | `{jobId}` → `{}` |
| `export.retryJob` | `{jobId}` → `{}` |
| `export.listPresets` | `{}` → `[{id, name, spec}]` |
| `export.savePreset` | `{name, spec}` → `{id}` |

### 5.11 延时工作台（9 个）

| 方法 | 参数 → 返回 |
|---|---|
| `timelapse.listSequences` | `{}` → `[{seqId, name, frameCount, state:"idle"|"analyzing"|"rendering", renderProgress}]`（导入时自动成组 + 此处可手动建组） |
| `timelapse.getSequence` | `{seqId}` → `{name, frameCount, frames:[{frameNo, photoId}], exposureRamp: {keyframes:[{frameNo, ev, temperatureK}]}, deflicker:{analyzed: bool, brightnessGainPct?: number}, lrtCompatible: bool}` |
| `timelapse.analyzeFlicker` | `{seqId}` → `{jobId}`（完成 result：`{brightnessGainPct, flickerIndex}`，量化结果） |
| `timelapse.applyDeflicker` | `{seqId, strength}` → `{jobId}`（写各帧 sidecar 参数） |
| `timelapse.setExposureRamp` | `{seqId, keyframes:[{frameNo, ev, temperatureK}]}` → `{}`（写 lumina:ExposureRamp + 尽力 lrt: 回写） |
| `timelapse.previewFrame` | `{seqId, frameNo, size}` → `{path}` |
| `timelapse.render` | `{seqId, resolution:"1080p"|"4K"|"6K", fps: 25|30|60, codec:"h264"|"h265"|"prores", colorAdjust?:{toneCurve:{shadows,midtone,highlight}, colorGrade:{shadowHue,shadowSat,midtoneHue,midtoneSat,highlightHue,highlightSat,blend,balance}}}` → `{jobId}`；`colorAdjust` 缺省或不恒等时≈不调色；ProRes 不可达 → **5001** + `data.alternative:"h264"`，UI 置灰不静默（§9-Q3） |
| `timelapse.getRenderJobs` | `{}` → `[JobInfo]` |
| `timelapse.frameHistogram` | `{seqId, frameNo, size?}` → `{r:[256], g:[256], b:[256], luma:[256]}`（引擎侧直方图，规避前端 canvas 因 `lumina-file://` 污染无法读像素） |

### 5.12 风光专项（4 个）

| 方法 | 参数 → 返回 |
|---|---|
| `hdr.detectBracketGroups` | `{filter?: Filter}` → `[{groupId, photoIds, exposureStops}]`（按连拍间隔 + EV 识别包围组） |
| `hdr.composite` | `{groupId, options:{deghost: bool, outputFormat:"tiff"|"dng", autoAlign: true}}` → `{jobId}`；完成 result：`{photoId}`（产物入索引、可进修图台） |
| `denoise.apply` | `{photoId, strength, luminanceMask: bool}` → `{jobId}`（NLM + 亮度蒙版；非破坏：结果入缓存，参数写 sidecar lumina:/crs:） |
| `denoise.preview` | `{photoId, strength, rect:{x,y,w,h}}` → `{path}`（局部对比小样） |

### 5.13 地图（9 个）

| 方法 | 参数 → 返回 |
|---|---|
| `map.getLocations` | `{viewport?:{minLat,maxLat,minLng,maxLng}, zoom, onlyLocated?: bool}` → `[{locationId?: int64, name, lat, lng, count, cluster: bool, children?: [...]}`（缩放聚合/展开由引擎按 zoom 计算） |
| `map.getLocationDetail` | `{locationId}` → `{name, lat, lng, altitude, count, timeSpan:{from,to}, favorite, samplePhotoId}` |
| `map.setPhotoLocation` | `{ids, lat, lng, altitude?, locationName?}` → `{updated:n}`（**写 sidecar 的 exif: 命名空间**，原片零写入，§9-Q1） |
| `map.clearPhotoLocation` | `{ids}` → `{updated:n}` |
| `map.listUnlocated` | `{page}` → `QueryResult` |
| `map.toggleFavorite` | `{locationId, favorite}` → `{}` |
| `map.getTile` | `{z, x, y}` → `{path, source: "local"|"online"}`（@since 1.1.0；**二进制不进 JSON**，返回瓦片文件路径；目录瓦片 `{home}/maptiles/{z}/{x}/{y}.png` 优先；未命中且 settings `mapOnlineFallback=true` 时在线下载并缓存，否则错误 1001） |
| `map.importGpx` | `{path}` → `{count, points:[{lat, lng, ele?, time?, name?}]}`（@since 1.1.0；解析 GPX 航点+轨迹点，按文件顺序编号；点位集驻留引擎内存供 applyGpxPoints 择点） |
| `map.applyGpxPoints` | `{photoIds: int64[], pointIndexes: int64[]}` → `{updated:n, locationId, sidecarFailed, warnings:[]}`（@since 1.1.0；photoIds[i] ← 最近一次 importGpx 的第 pointIndexes[i] 个点，按位配对；越界索引计入 warnings 不中断） |

### 5.14 Web 画廊（5 个）

| 方法 | 参数 → 返回 |
|---|---|
| `gallery.publish` | `{albumId, password, expiryDays: 30, title, customerName}` → `{token, staticDir, expiresAt, previewUrl}`（生成静态站点到 `{home}/gallery/{token}/`；previewUrl 由 Electron 主进程起 localhost http 提供；定位「本地预览级门禁」） |
| `gallery.revoke` | `{albumId}` → `{}` |
| `gallery.listPublished` | `{}` → `[{albumId, token, previewUrl, expiresAt, stats}]` |
| `gallery.importFeedback` | `{albumId, filePath /*收藏数据包 JSON*/}` → `{favorites:n, completionRate}` |
| `gallery.getFeedback` | `{albumId}` → `{favoritesCount, completionRate, perPhoto:[{photoId, favorited}]}` |

### 5.15 模拟图库（2 个）

| 方法 | 参数 → 返回 |
|---|---|
| `mock.generate` | `{spec?: MockSpec}` → `{jobId}`；完成 result：`{dbPhotos, imageFiles, sidecars, missing, diskBytes, durationMs, dngSelfTest:{ok, sampled, succeeded, decodeMsP50}, readmePath}` |
| `mock.status` | `{}` → `{generated: bool, libraryRoot: string, dbPhotoCount, assetFormat:"linear-dng", lastSelfTest}` |

**MockSpec（默认见架构设计 §2.4）**：

```jsonc
{
  total: 18192, rootCount: 5, folderCount: 79, monthRange: ["2025-12","2026-08"],
  displayFormat: { RW2: 16470, JPG: 1387, DNG: 273, ARW: 15, MP4: 45, PSD: 1, PNG: 1 },
  edited: 4347, maskRatio: 0.838, removal: 58, aiMask: 183,
  rating: { "2": 1365, "3": 1, "4": 57 }, gps: 137,
  whiteBalance: { asShot: 14310, custom: 3861 }, missing: 421, lrtGroups: 2,
  assetFormat: "linear-dng", assetWidth: 1200, assetHeight: 800, assetScale: 1.0,
  seed: 20261022
}
```

> 影像本体为**结构合法未压缩 Linear DNG**（含内嵌 JPEG 预览），索引层 `display_format` 复刻 92.2% RW2 的真实分布（架构设计 §1.5）。生成后自动跑 `raw.selfTest` 判定 L1。

## 6. 事件（引擎 → UI 通知）

| 事件 | params |
|---|---|
| `event.job.progress` | `{jobId, kind, state:"running", progress, detail, etaMs?, extra?}` |
| `event.job.completed` | `{jobId, kind, state:"done"|"failed"|"cancelled", result?, error?:{code,message,data?}}`（各方法的 result 结构见对应方法行注） |
| `event.thumb.ready` | `{photoId, size, path}` |
| `event.fs.changed` | `{rootId, path, change:"missing"|"restored"|"modified"}`（ReadDirectoryChangesW 实时监测，触发 UI 红点） |
| `event.health.issue` | `{issueId, photoId, kind:"missing"|"corrupt", path}`（**任何异常不得被吞掉**） |
| `event.import.fileResult` | `{jobId, path, status:"ok"|"duplicate"|"failed", error?, photoId?}` |
| `event.engine.log` | `{level, module, message}`（调试面板可选消费） |

## 7. 并发与时序约定

1. **请求并发**：引擎对查询类方法并发处理（SQLite WAL 多读单写）；**同一照片的 XMP 写**由引擎串行化（per-photo 写队列）。
2. **超时**：UI 侧默认超时 10s（`raw.decode`/`mock.generate` 等转 job 的方法立即返回 jobId 不占超时）；超时后 UI 不重发写类请求（防重复落盘），只重发幂等读请求。
3. **取消**：所有 job 类方法返回 jobId 后，用 `*.cancel` 或 `export.cancelJob` 取消；引擎在文件粒度检查取消标志，已完成的单文件结果保留。
4. **崩溃看护**：Electron 侧检测引擎退出（非 `system.shutdown` 触发）→ 自动重启 + 重放 `initialize` → UI 提示「引擎已恢复，未落盘参数已从 sidecar 重读」（参数以 sidecar 为准，天然可恢复）。
5. **顺序保证**：同一 jobId 的 progress 事件与 completed 事件保序；thumb.ready 事件晚于对应请求的响应到达是合法的。

## 8. 版本化

- `protocolVersion` 主版本变更 = 破坏性（字段删除/语义变更）；次版本 = 只增字段。
- 引擎与 UI 各自在构建产物中内嵌协议版本常量（`engine/src/rpc/version.h` / `app/src/api/protocol.ts`），CI 校验二者一致。
- 本文档变更流程：改文档 → 双方确认 → 同步实现 → 在 §5 方法表标注 `@since`。
