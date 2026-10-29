# LUMINA v1.3.0 发布说明

焦点堆栈（景深堆栈）正式交付 + 编辑落盘 / 退出 / 竞态验证加固入库。版本号统一为 **1.3.0**
（`engine/src/rpc/version.h`、`app/package.json`、`app/package-lock.json`、`scripts/installer.nsi`、`scripts/package-app.ps1`、`app/electron/engine-bridge.ts` 全部对齐）。

---

## 一、新增功能：景深堆栈（Focus Stacking）

风光专项主页「焦点堆栈」卡片由原来的规划占位升级为可用功能（与 HDR 卡同架构）：

- **检测（stack.detectStackGroups）**：仅识别「曝光冻结」的连拍——组内全部帧 `aperture / iso / shutter / focalLength` 完全相同、时序接近（默认 30s，可配 0–600s）、序列连续（同 `seq_id` 或文件名数字连续）。与 HDR 包围组天然互斥（HDR 靠 EV 差识别）。组 ID = 组内最小 photoId，稳定可复现。
- **合成（stack.composite）**：任务化（JobManager），完成回执 `result.photoId` 已入图库索引，可直接进修图台。
- **融合算法**：以全局焦点测度（四邻域拉普拉斯幅度和）最高的帧为对齐基准，逐像素按 `(focusMeasure + ε) ^ smoothness` 软加权融合（`smoothness` 默认 4，1 柔 → 12 锐），全清晰且无硬接缝；参照 HDR 的零静默 / 取消感知 / 源片只读纪律。
- **落盘与入库**：产物写 `{home}/cache/composite/stack-<groupId>-<stamp>.{dng|tiff}`（复用 HDR 的线性 DNG / RGB8 TIFF 写出），`upsert` 进 `photos` 表（`folderId / rootId` 沿用参考帧，缺失时 `ensureRoot / ensureFolder` 登记「Focus Stack 合成」），要求 `newPhotoId > 0`，否则删文件抛错。
- **UI**：`StackCard` 镜像 `HdrCard` 视觉与交互：组选择 chips、自动对齐开关、DNG/TIFF 分段、锐度/融合滑块；产物回执「在修图台打开」；路线图新增 v1.3.0 且标为 current。

实现要点：抽取 `engine/src/specialty/compositing_common.{h,cpp}` 承载 HDR 与景深堆栈共用的灰度金字塔、`autoAlign`（`estimateShift`）、`FrameView` 采样、DNG/TIFF 写出；HDR 的 `fuseFrames` 与新增 `focusStackFrames` 各自保留（前者含去鬼影，后者纯焦点加权），`hdr_service` / `denoise_service` 改为调用 `compositing::`。

## 二、验证加固（上批次遗留，本批次入库）

`engine/tests/test_edit_flush_on_tick.cpp` 新增 T5（E20 真实并发：写线程持续 `putContrast` 期间 `end(flush=true)`，断言 `flushed` 真且 sidecar 落地）、T6（E24 `kPending` 覆盖：写线程 busy-wait 期间 `end()`，断言 `ended` 非 null 且 `flushed=false`）。测试运行目录按 PID 隔离，根治「两并发 ctest 互相踩掉共享 `test-*` 目录」的假失败。

---

## 三、体积与速度

| 项 | v1.2.3 | v1.3.0 | 变化 |
|---|---|---|---|
| 引擎可执行文件 | 6.46 MB | **6.53 MB** | +1.1%（新增景深堆栈代码 + 共享合成辅助；Release 仍带 `-s` strip） |
| 打包目录 / portable zip / setup.exe | 236 / 102.4 / 75.5 MB | 未测 | 本环境发布打包被安全钩子拦截，未能产出（见「已知限制 1」） |

测试：全量 ctest **40/40 全绿**（39 + 新增 focus_stack），Total Test time ≈ 227s；`edit_flush_on_tick` 仍为 15.5s（T5/T6 稳定 PASS，4× 连跑无抖动）。

---

## 四、验证（均为实测，非推断）

- 引擎 **40/40 ctest 全绿**：含新 `focus_stack`（核心 `compositing::focusStackFrames` 混合 + `estimateShift` 对齐单测）。
- 引擎二进制字符串核查：`stack.composite` / `stack.detectStackGroups` 各出现 2 次，证明 RPC 注册已编译进交付二进制。
- 重构后 release 干净重建成功，引擎 exe = 6,851,584 字节（≈6.53 MB）。
- 前端 `tsc --noEmit` 零错误（exit 0）。
- 新文件均在 CMakeLists 注册（`add_test(NAME focus_stack ...)`）。

---

## 五、已知限制（如实登记，不粉饰）

1. **发布打包（便携目录 / zip / NSIS setup.exe）本环境未能产出**：`package-app.ps1` 的清理步骤被环境级「safe-delete bulk guard」安全钩子拦截（非脚本或代码缺陷）。因此在沙箱内无法给出 v1.3.0 包体积 / zip 体积，也无法跑 package-app 冒烟。可量化体积代理 = 引擎 exe 6,851,584 字节。请在具备 NSIS 与安全钩子放行的环境执行 `scripts/package-app.ps1 -Zip` 与 `makensis scripts/installer.nsi` 完成发布包，并跑 `scripts/smoke-packaged.py`。
2. **端到端景深堆栈合成（stack.detectStackGroups + stack.composite 对真实 focus-bracket 库）未实跑**：当前无真实对焦连拍夹具。核心混合算法由 `test_focus_stack` 覆盖（左 / 右半区各自取最锐帧），集成层（RPC ↔ service ↔ DB ↔ upsert）由主导代码审查确认。建议后续用真实 RW2 对焦包围集补一条 MCP 端到端用例。
3. **与旧版引擎二进制的体积对比无旧文件可对照**，故仅报新体积。
4. **Photoshop 2026 视觉验收（M3-04）仍未做**，本机无 Photoshop。
5. **AI 降噪目前是 NLM + 亮度蒙版**，不是真正的 AI 模型。
6. 仍规划未实现的模块：**全景拼接 / 星空 / 书籍排版 / Web 画廊 / 移动端**（见下）。

---

## 六、排期中的后续项

- 真实对焦连拍夹具 + MCP 端到端景深堆栈用例
- 全景拼接（v1.3.x 候选）：对齐算法 / 接缝 blending 是独立技术链
- 星空 / 书籍排版 / Web 画廊 / 移动端（v2.0 路线）
- AI 降噪升级为真模型（当前为 NLM）
