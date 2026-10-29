# LUMINA 软件功能与实现总览（Agent 可读）

> 用途：给 AI/Agent 一次性读取「当前软件能做什么 + 每件事具体在哪实现」。所有路径相对仓库根 `D:\LUMINA`。
> 版本口径：应用/打包 **1.3.1**，传输协议 **1.0**（`app/src/api/protocol.ts` 的 `PROTOCOL_VERSION='1.0'`），引擎 `kEngineVersion="1.3.0"`（`engine/src/rpc/version.h`，本轮未随应用升位）。
> 生成依据：真实扫描仓库源码（非文档转述）。若代码与本文冲突，以代码为准。

---

## 0. 一句话定位

LUMINA 是「面向摄影全流程的桌面图像工作台」：导入 → 素材库 → 修图（快/专业双模式）→ 精选 → 地图 → 延时 → 风光专项（HDR/景深堆栈/拼接/降噪）→ 导出交付 → 作品集/Web 客户画廊 → 存储健康。风光是特色模块而非唯一身份。

**核心差异化**：编辑参数存**开放 XMP sidecar**（不锁私有数据库）、一切非破坏、RAW 走开源管线（LibRaw）、引擎内置色彩管理（lcms2）与 PSD 读写。

---

## 1. 架构分层

| 层 | 技术 | 目录 | 职责 |
|---|---|---|---|
| 引擎 | C++17 | `engine/src/**` | RAW 解码、DB、修图管线、导出、色彩管理、健康扫描；单实例锁；RPC 服务（stdio + TCP 控制口） |
| 渲染层 | Electron + React 18 + TS + Vite + zustand + Tailwind | `app/src/**`、`app/electron/**` | 全部 UI 与交互；经 `window.lumina` 桥调用引擎 |
| 桥 | IPC/stdio + TCP（MCP） | `app/electron/engine-bridge.ts`、`app/electron/mcp-bridge.ts` | 引擎生命周期、RPC 转发、MCP 控制口 |
| Agent 控制面 | MCP 工具（18 个）+ 渲染层执行器 | `app/src/mcp/executor.ts` | 让外部 Agent 真驱动 UI 动作（非仅读数据） |
| 插件 | Lua（扩展位） | — | v2.0 规划，v1.0.0 仅预留机制 |

关键目录速查（引擎）：`core`(配置/单实例锁) · `json` · `rpc`(方法注册/协议) · `db`(schema+DAO+迁移+FTS5) · `library` · `import` · `thumb` · `imaging`(16bit 像素/EXIF) · `raw`(LibRaw) · `metadata` · `xmp`(crs: 双向) · `edit`(会话/撤销/同步) · `plan`(渲染管线：`ops/*`、`geometry*`、`lens_profile`、`op_table`、`render_exec`) · `graph`(NodeGraph) · `color`(lcms2) · `export`(JPG/TIFF/PNG/PSD/PDF) · `gallery` · `health` · `map` · `timelapse` · `specialty`(HDR/堆栈/降噪) · `jobs` · `mock`。

---

## 2. 功能矩阵（模块 → 页面文件 → 核心 RPC → 引擎实现）

| 功能 | 渲染层文件（`app/src/modules/…`） | 主要 RPC | 引擎实现 |
|---|---|---|---|
| **素材库** | `library/LibraryPage.tsx`、`GridVirtual.tsx`、`GridItem.tsx`、`Filmstrip.tsx`、`PhotoInspector.tsx`、`SearchBar.tsx`、`SidebarFolderTree.tsx` | `library.queryPhotos` `library.search` `library.getFacets` `library.getFolderTree` `library.getPhotoDetail` `library.getStats` `library.getTimeline` `library.listSmartCollections` `library.runSmartCollection` `library.updatePhotos` `library.removePhotos` `library.restorePhotos` `thumb.ensureBatch` `thumb.getStatus` `preview.getBase` | `library/`、`db/photo_dao.cpp`、`db/fts_indexer.cpp`、`thumb/` |
| **修图台（快/专业）** | `develop/DevelopPage.tsx`、`ViewerCanvas.tsx`、`CurveEditor.tsx`、`panels/`(FastPanel、ProPanel、OptionBar、LeftRail、ToolRail、MaskPanel、RangeMaskBar、TransformBar、LayersPanel、HistoryPanel、HistogramPanel、DocTabs、panelKit)、`webgl/`(renderer、shaders、maskField、ops/*) | `develop.render` `edit.beginSession` `edit.setParams` `edit.undo` `edit.redo` `edit.createSnapshot` `edit.restoreSnapshot` `edit.getSnapshots` `edit.syncToSelection` `edit.syncToSequence` `edit.endSession` `analysis.histogram` `analysis.getExposedRange` `presets.list` `presets.apply` `xmp.read` `xmp.write` `xmp.checkExternal` | `plan/**`（见 §3）、`edit/`、`xmp/`、`color/` |
| **精选 Culling** | `culling/CullingPage.tsx`、`CullingGrid.tsx`、`CullingFilmstrip.tsx`、`CullingToolbar.tsx`、`CullingViewer.tsx`、`CullingBatch.tsx` | `library.updatePhotos`(rating/flag) `edit.syncToSelection` | `db/photo_dao` |
| **导入（单页直导）** | `ingest/IngestWizard.tsx` | `import.scan` `import.scanSource` `import.start` `import.cancel` `import.checkTarget` `import.listRecentSources` `import.resolveMissingFolder` | `import/` |
| **地图** | `map/MapPage.tsx`、`MapCanvas.tsx`、`LocationClusterList.tsx`、`LocationDetailPanel.tsx`、`LocationTimeline.tsx`、`UnlocatedPanel.tsx` | `map.getLocations` `map.getLocationDetail` `map.setPhotoLocation` `map.clearPhotoLocation` `map.toggleFavorite` `map.listUnlocated` `map.importGpx` `map.applyGpxPoints` `map.getTile` | `map/` |
| **延时工作台** | `timelapse/TimelapsePage.tsx`、`Filmstrip.tsx`、`FramePlayer.tsx`、`Histogram.tsx`、`RenderPanel.tsx`、`SequenceList.tsx`、`ExportView.tsx` | `timelapse.listSequences` `timelapse.getSequence` `timelapse.previewFrame` `timelapse.analyzeFlicker` `timelapse.applyDeflicker` `timelapse.frameHistogram` `timelapse.setExposureRamp` `timelapse.render` `timelapse.getRenderJobs` | `timelapse/` |
| **风光专项** | `landscape/LandscapePage.tsx`、`HdrCard.tsx`、`StackCard.tsx`、`DenoiseCard.tsx` | `hdr.composite` `hdr.detectBracketGroups` `stack.composite` `stack.detectStackGroups` `denoise.apply` `denoise.preview` | `specialty/` |
| **导出与交付** | `export/ExportPage.tsx`、`ExportQueue.tsx`、`PresetList.tsx`、`WatermarkForm.tsx`、`PrintCard.tsx`、`SoftProofCard.tsx`、`watermarkPreview.ts` | `export.createJob` `export.listJobs` `export.cancelJob` `export.retryJob` `export.listPresets` `export.savePreset` `softproof.preview` `psd.inspect` | `export/exporter.cpp`、`export/psd_*`、`color/` |
| **Web 客户画廊** | `gallery/GalleryPage.tsx`、`PublishWizard.tsx`、`PublishedList.tsx`、`GallerySettingsPanel.tsx` | `gallery.publish` `gallery.listPublished` `gallery.revoke` `gallery.getFeedback` `gallery.importFeedback` | `gallery/` |
| **作品集** | `portfolio/PortfolioPage.tsx`、`AlbumList.tsx`、`AlbumDetail.tsx` | `albums.list` `albums.create` `albums.addPhotos` `albums.removePhotos` `albums.reorder` `albums.getItems` | `db/album_dao.cpp` |
| **存储健康** | `health/HealthPage.tsx` | `health.getStatus` `health.scan` `health.scanFull` `health.verify` `health.verifyPhotos` `health.listIssues` `health.resolveIssue` `health.backup` `health.backupNow` `health.restore` `health.restoreFromBackup` `health.getBackupConfig` `health.setBackupTarget` `health.cancelJob` `health.exportDiffReport` | `health/` |
| **幻灯片** | `slideshow/SlideshowPage.tsx` | (纯渲染层，读取 `library.*` 结果) | — |

状态管理：`app/src/store/`（`developStore` `libraryStore` `uiStore` `jobStore` `events`）。外壳/菜单：`app/src/App.tsx`、`components/MenuBar.tsx`、`components/shell.tsx`。

---

## 3. 修图渲染管线（20 阶段，单一事实源 `engine/src/plan/op_table.cpp: kPipelineOrder`）

| # | 阶段名 | Stage | 算子 |
|---|---|---|---|
| 1 | 白平衡通道增益 | S0Decode | `basic.whiteBalance` `basic.temperature` `basic.tint` |
| 2 | 镜头 profile 畸变(k1–k3) | S1Geometry | `geometry.apply`(subStep1) |
| 3 | 色差校正 CA | S1Geometry | (geometry 子步2) |
| 4 | 光学暗角校正 | S1Geometry | (子步3) |
| 5 | 透视/变换(含 aspect) | S1Geometry | (子步4) |
| 6 | 裁剪+拉直+唯一一次重采样 | S1Geometry | (子步6) |
| 7 | 降噪(色度→亮度) | S15Denoise | `detail.denoise` |
| 8 | 局部对比 | S2Tone | `basic.clarity` `basic.texture` |
| 9 | 曝光 | S2Tone | `basic.exposure` |
| 10 | 对比 | S2Tone | `basic.contrast` |
| 11 | 高光/阴影/白/黑场 | S2Tone | `basic.highlights` `basic.shadows` `basic.whites` `basic.blacks` |
| 12 | 去雾 | S2Tone | `basic.dehaze` |
| 13 | 原色校准 | S2Tone | `calibration.*` |
| 14 | 自然饱和度/饱和度 | S2Tone | `basic.vibrance` `basic.saturation` |
| 15 | tone map（唯一域分界） | S3ToneMap | `pipeline.toneMap` |
| 16 | 曲线（复合/分通道/参数） | S4Color | `curve.toneCurveRGB/R/G/B` `curve.parametric` |
| 17 | HSL / 颜色分级 | S4Color | `color.hsl` `color.colorGrade` |
| 18 | 蒙版局部调整 | S4Color | `mask.apply` |
| 19 | 锐化 | S5Detail | `detail.sharpen` |
| 20 | 裁后暗角/颗粒 + 输出编码 | S6Effects | `effects.vignette` `effects.grain` `output.encode` |

纪律：阶段顺序为 CI 只读单一事实源，重排/删行 = MR 拒绝；`rank = 行号*100 + 行内算子下标`。域流转：scene-linear（1–14）→ tone map（15）分界 → display（16–19）→ 8/16bit 输出（20）。

预览与导出**同实现**：`develop.render` 走 `displayToPlanBuf → runEditPlan → planBufToDisplay`，与导出同一代码路径（保证所见即所得）。

---

## 4. 编辑参数模型（`EditParams`）

顶层可下发键（MCP/`edit.setParams` 白名单，`app/src/mcp/executor.ts: PARAM_KEYS`）：
`processVersion, whiteBalance, temperature, tint, exposure, contrast, highlights, shadows, whites, blacks, clarity, texture, dehaze, vibrance, saturation, toneCurveRGB, toneCurveR, toneCurveG, toneCurveB, parametric, hsl, colorGrade, calibration, detail, optics, geometry, masks, removals, adjustmentStack`

蒙版：类型 `linear|radial|brush`，`MASK_LOCAL_KEYS`（局部可调）= `exposure, contrast, highlights, shadows, temperature, tint, saturation, clarity, dehaze`；支持布尔组合（replace/intersect/subtract）、范围蒙版（亮度/颜色/深度）、GPU mask pass（32 槽）。
参数存盘：开放 XMP sidecar（`crs:` 命名空间双向映射，字节保留），不写私有 blob。

---

## 5. 引擎 RPC 方法全表（109，`engine/src/rpc/methods.cpp` 的 `registerMethod`）

- **system**：`system.initialize` `system.ping` `system.shutdown` `system.getSettings` `system.setSettings` `system.listVolumes`
- **library**：`library.queryPhotos` `library.search` `library.getFacets` `library.getFolderTree` `library.getPhotoDetail` `library.getStats` `library.getTimeline` `library.listSmartCollections` `library.runSmartCollection` `library.updatePhotos` `library.removePhotos` `library.restorePhotos`
- **thumb / preview / raw**：`thumb.ensureBatch` `thumb.getStatus` `preview.getBase` `raw.decode` `raw.getProxy` `raw.selfTest`
- **develop / edit / analysis**：`develop.render` `edit.beginSession` `edit.setParams` `edit.undo` `edit.redo` `edit.createSnapshot` `edit.restoreSnapshot` `edit.getSnapshots` `edit.syncToSelection` `edit.syncToSequence` `edit.endSession` `analysis.histogram` `analysis.getExposedRange`
- **presets / xmp**：`presets.list` `presets.apply` `xmp.read` `xmp.write` `xmp.checkExternal` `xmp.listNamespaces` `xmp.migrationStatus`
- **import**：`import.scanSource` `import.start` `import.cancel` `import.checkTarget` `import.listRecentSources` `import.resolveMissingFolder`
- **albums**：`albums.list` `albums.create` `albums.addPhotos` `albums.removePhotos` `albums.reorder` `albums.getItems`
- **map**：`map.getLocations` `map.getLocationDetail` `map.setPhotoLocation` `map.clearPhotoLocation` `map.toggleFavorite` `map.listUnlocated` `map.importGpx` `map.applyGpxPoints` `map.getTile`
- **health**：`health.getStatus` `health.scanFull` `health.verifyPhotos` `health.listIssues` `health.resolveIssue` `health.backupNow` `health.restoreFromBackup` `health.getBackupConfig` `health.setBackupTarget` `health.cancelJob` `health.exportDiffReport`
- **timelapse**：`timelapse.listSequences` `timelapse.getSequence` `timelapse.previewFrame` `timelapse.analyzeFlicker` `timelapse.applyDeflicker` `timelapse.frameHistogram` `timelapse.setExposureRamp` `timelapse.render` `timelapse.getRenderJobs`
- **specialty**：`hdr.composite` `hdr.detectBracketGroups` `stack.composite` `stack.detectStackGroups` `denoise.apply` `denoise.preview`
- **export / 色彩 / PSD**：`export.createJob` `export.listJobs` `export.cancelJob` `export.retryJob` `export.listPresets` `export.savePreset` `softproof.preview` `psd.inspect`
- **gallery / vcopy / mock / cache**：`gallery.publish` `gallery.listPublished` `gallery.revoke` `gallery.getFeedback` `gallery.importFeedback` `vcopy.create` `vcopy.list` `mock.generate` `mock.status` `cache.clear`

> 另有 5 个**长任务类型**（非 RPC，经 `jobs->start()` 执行）：`import.scan` / `health.scan` / `health.verify` / `health.backup` / `health.restore`。

---

## 6. MCP Agent 工具（18，`app/src/mcp/executor.ts` + 主进程截图）

`lumina_status` · `list_photos` · `open_photo` · `open_tabs` · `list_tabs` · `close_tab` · `get_develop_params` · `set_develop_params` · `add_mask` · `create_snapshot` · `restore_snapshot` · `switch_module` · `set_develop_mode`(fast/pro) · `set_theme`(Dark/Light) · `export_photo`(jpg/png/psd) · `softproof_preview` · `psd_inspect` · `lumina_screenshot`。

语义：这些工具通过 TCP 控制口 → 渲染层执行器，**真正驱动 store 动作**（打开照片、改参数、加蒙版、切换模块、截图），不是只读数据库。Agent 可据此端到端操作用户界面。

---

## 7. 数据模型（SQLite，`engine/src/db/schema.sql`）

表：`schema_meta` `roots` `folders` `files` `photos` `develop_settings` `locations` `sequences` `sequence_frames` `albums` `album_items` `presets` `export_presets` `health_issues` `backups` `virtual_copies` `snapshots` `sequence_ramps` `jobs` `mock_meta` `gallery_meta` `gallery_feedback`。
索引要点：`photos` 按 captured/folder/root/rating/seq/missing/deleted/camera；`develop_settings` 部分索引 `has_edits=1`、`mask_count>0`。全文检索 FTS5（`db/fts_indexer.cpp`）。
DB 只是**派生缓存**：编辑参数的权威来源是 XMP sidecar（可重建）。

---

## 8. 文件与资源布局

- 档案根：打包版 `%APPDATA%/Roaming/LUMINA/library`（`resolveHomeDir`：`isPackaged && !useDevHome` → `userData/library`）；开发/冒烟/自检用 `~/.lumina`；可用 `LUMINA_HOME` 覆盖（`app/electron/engine-bridge.ts: resolveHomeDir`）。
- 编辑参数：照片同目录 `.xmp` sidecar（开放，可被 LR/PS 读取）。
- 缓存：`{home}/cache/`（thumbs/previews）；缩略图本地 SSD 加速（NAS 场景按需拉原片）。
- 导出格式：`jpg` `tiff` `png` `psd`（PSD 支持分层写出/读回，`psdDepth` 8/16，节点>20 降级单层）。色彩空间：sRGB / AdobeRGB / PrintSim（印刷模拟），JPEG APP2、TIFF 34675、PNG iCCP、PSD 0x040F 嵌入 ICC。
- 打印：`printToPdf`（A4/A3/Letter，MediaBox 精确）。

---

## 9. 关键技术不变量

1. **非破坏**：任何编辑只改参数，原片不动；快照/历史/图层栈均在参数层。
2. **开放 XMP**：参数即文件，不锁库；DB 损坏可由 sidecar 重建（用户 2026-04 亲历 .lrcat 目录损坏 → 这是核心卖点）。
3. **预览==导出**：同一 `runEditPlan` 实现，导出与预览逐像素一致（有 export-parity 回归）。
4. **RAW**：LibRaw 解码，解码后按 EXIF orientation 转正（竖拍不再侧倒）。
5. **色彩管理**：内置 lcms2（sRGB/AdobeRGB/PrintSim），导出物位级可复现（ICC 头归一化，四格式两次导出字节相同）。
6. **16bit 内部**：管线主要工作域为 16bit；8bit 为输出拓宽（已知限制）。
7. **软件渲染降级**：无 WebGL 时走引擎 CPU `develop.render`；前端渲染**串行化 + coalesce + 640/1024 双分辨率**（拖动 640、停手 220ms 补 1024），避免整图 RPC 积压导致卡死（`app/src/modules/develop/ViewerCanvas.tsx`）。
8. **单实例锁**：`engine/src/core/config.cpp`，残留锁按 pid 判死活，防永久降级 Mock。

---

## 10. 构建与验证命令

```bash
# 引擎（CMake+Ninja，需拷 thirdparty/libraw/lib/libraw.a 到新 worktree）
"D:/work/ide/CLion 2024.3.4/bin/cmake/win/x64/bin/cmake.exe" --build build/engine --target lumina-engine -j 8
ctest --test-dir build/engine            # 目标全绿（本轮 43 项）

# 渲染层（Vite + esbuild）
cd app && npx tsc --noEmit && npm run build   # 产物 -> build/app/

# 打包（PowerShell；注意工具会话的 Remove-Item 守卫函数需先移除）
powershell -NoProfile -File scripts/package-app.ps1 -SkipBuild -Zip
# NSIS 安装包（从 scripts/ 内调用；MSYS 下用 MSYS_NO_PATHCONV=1 且省略 /V2）
cd scripts && MSYS_NO_PATHCONV=1 "/c/Program Files (x86)/NSIS/makensis.exe" installer.nsi

# 端到端冒烟（打包版，全模块场景，跑到 DONE）
python scripts/smoke-packaged.py            # 基础冒烟（控制口/RPC）
# 或直接：LUMINA_SMOKE=1 ./build/pkg/LUMINA/LUMINA.exe （渲染层跑 runSmokeScenario，日志见 stdout）
```

产物：`build/pkg/LUMINA-1.3.1-portable.zip`、`build/pkg/LUMINA-1.3.1-setup.exe`。

---

## 11. 已知边界 / 未做

- PSD：8bit 为 16bit 拓宽；外来 PSD 仅合成图降级预览；PS 视觉验收（M3-04）需人工（本机无 Photoshop）。
- 印刷色域 PrintSim 为合成色域，非真实纸墨；真实印刷 ICC 反代 ΔE 验收待拿到用户打印机 profile。
- PNG/PSD 的 16bit 写出仍以 8bit 为基础拓宽。
- 延时：Phase B 色调数学已实装；4K/ProRes 渲染队列为规划项。
- 插件（Lua）与云同步/协作审阅为 v2.0 扩展位，v1.x 未实装。
- 冒烟 `t17-controls` 的 11/19 子项低于像素阈值属既有状态（`docs/v1.2.0/M2-视觉差异报告.md` 记录，非能力缺失）；预览与引擎逐项一致（id=0.0，d≈eng）。
- 引擎 `kEngineVersion` 仍为 1.3.0（本轮应用升 1.3.1，引擎版本号未同步）。

---

## 12. 相关文档

`docs/架构设计-LUMINA-v1.2.0.md`（架构）· `docs/PRD-LUMINA-v1.2.0.md`（需求）· `docs/ipc-protocol.md`（IPC 协议）· `docs/param-leaves.json`（参数叶表）· `docs/release-notes-1.3.0.md` · `docs/BUILD.md`。
