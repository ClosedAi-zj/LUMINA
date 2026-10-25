# R3 · PhotoCraft 白盒逆向工程报告

> **对象**：`D:/work/project/pycharm/p/refs/photocraft/`（上游 <https://github.com/storytold/photocraft>，`--depth 1`，HEAD = `1c3db2e`）
> **许可**：MIT OR Apache-2.0（`LICENSE-MIT`/`LICENSE-APACHE`，`NOTICE` 另有品牌与第三方资产条款）
> **用途**：LUMINA v1.2.0 架构借鉴。**本文只描述"怎么做的"，不搬运代码**，也未修改 refs 或 LUMINA 源码。
> **证据约定**：所有结论后附 `file:line`；无代码证据的判断标注「推断」。

---

## 1. 项目定位与规模

一个用**纯 Rust** 写的 Photoshop 克隆，目标是"同菜单、同快捷键、同行为、同文件保真度，但性能更好，且每个功能都能被 Agent 驱动"（`AGENTS.md:3`）。这不是演示项目：它有 alpha 阶段验收门、perf 预算、CI 分层检查、以及对着 Photoshop 2026 的量化 parity 打分。

| 维度 | 实测值 | 证据 |
|---|---|---|
| 语言 / 版本 | Rust，`edition = "2024"`，`rust-version = "1.95"` | `Cargo.toml:7-9` |
| workspace 版本 | `0.15.1`（但 `ROADMAP.md:6` 仍写"at version 0.6.0"——版本推进很快，文档滞后） | `Cargo.toml:8` |
| crate 数 | **26** 个 `crates/*`（`docs/architecture.md:10` 写 25，已过期） | `crates/` 目录 |
| 代码量 | **683,916 行 / 1,542 个 `.rs`**（crates 661,308 + apps 16,708 + xtask 5,371） | `find … | xargs wc -l` |
| 测试数 | **11,113 个 `#[test]`**（不含 `#[tokio::test]` 与 doctest） | `grep -rc "#\[test\]"` |
| 最大 crate | `ui-egui` 240,354 行（457 文件）→ `engine` 146,364 行（314 文件）→ `algo` 59,927 行 | 统计 |
| UI / 渲染 | egui 0.36 + eframe + **wgpu 30**；同一份 Rust 编译到 wasm32 | `Cargo.toml` workspace deps |
| 平台 | macOS(arm64)/Windows(x64)/Linux(x64) + wasm32 构建门 | `AGENTS.md:69` |
| 命令数 | ~700 条 `CommandSpec`；菜单目录 631 项**全部 live** | `docs/architecture.md:18`、`docs/scorecard.md:16` |
| 阶段 | alpha；"ready for real work ~45%"，到 beta 还需 ~1,100–1,800 小时 | `ROADMAP.md:3`、`docs/roadmap.md:25` |

**值得注意的规模特征**：这不是"引擎大、UI 小"的分配，而是 **UI 比引擎还大**（240k vs 146k）。原因是它把 68 个工具的状态机、14 套主题、35 个面板全部手写在 `ui-egui` 里，只是让每个动作都绕回 `engine`。这一点对 LUMINA 的排期判断很重要——**"UI 很薄"是逻辑上的薄，不是代码量上的薄**。

---

## 2. crate 依赖图

层级不是文档里的理想分层，而是**由 `cargo xtask layers` 强制检查**的分类表（`xtask/src/layers.rs:26` 的 `TABLE`，CI 解析 `cargo metadata` 后逐条核对）。违规会让 CI 红。

```
L7  apps/*            photocraft(桌面) · photocraft-cli(无头) · photocraft-web(wasm) · web-boot
     ▲
L6  ui-egui · automation(mcp)          ← 唯一允许依赖 egui/winit/rfd 的层
     ▲
L5  engine                             ← Session + 命令注册表 + 作业池
     ▲
L4  io · plugins(wasmi 沙箱)           (计划中未拆: tools / viewport / ml)
     ▲
L3  compose(CPU 参考) · gpu(wgpu) · format(.pcraft 原生格式)
     ▲
L2  ops(历史) · paint(画笔) · algo(滤镜) · text(排印) · vector(路径)
     ▲
L1  doc                                 ← 纯数据文档模型，无渲染无 UI
     ▲
L0  geom · cms(ICC) · color · raster(COW tile)
     ‖
     ┗ 独立零依赖（可单独发布）: psd · codecs→heif · raw · affinity · ipp · tablet
```

| crate | 职责一句话 | 内部依赖（实测 `Cargo.toml`） | 行数 |
|---|---|---|---|
| `geom` | 点/矩形/仿射/透视/贝塞尔/瓦片坐标，`TILE_SIZE = 256` | — | 2,000 |
| `cms` | 纯 Rust ICC v2/v4：profile 解析、Transform、BPC、软打样、3D LUT、GPU 矩阵/shaper | — | 6,153 |
| `color` | 像素格式、色彩空间、27 种混合模式的标量数学 | cms | 1,558 |
| `raster` | 稀疏 COW tile 表面、掩膜、区读区写、可中断 | geom, color | 2,179 |
| `psd` | PSD/PSB 格式级读写 + `.abr/.asl/.aco/.atn/.grd` 等资产 | **无**（可发布） | 19,105 |
| `codecs` | 扁平格式对称编解码（写得出就读得回） | heif | 27,381 |
| `raw` | clean-room 相机 RAW 解码 + 显影（DNG/CR2/NEF/ARW/RW2/ORF/RAF） | **无** | 14,486 |
| `affinity` | Affinity `.af*` 只读 | **无** | 4,585 |
| `ipp` | CUPS/IPP 打印客户端 | **无** | 1,349 |
| `tablet` | 数位板输入（唯一允许 `unsafe` 的 crate） | **无** | 1,230 |
| `doc` | 文档模型：图层树/掩膜/调整/效果/通道/路径/切片/变量 | geom, color, raster | 5,250 |
| `ops` | **只有撤销/重做历史**（不是"操作对象"） | doc | 1,396 |
| `paint` | 画笔引擎：笔尖、动态、笔迹缓冲、混合器、预设 | geom, color, raster | 8,697 |
| `algo` | 77 个模块的 CPU 滤镜/选区/修复/变形 | geom, color, raster | 59,927 |
| `text` | 字体库 + parley 排版 + 栅格 + PSD `TySh`/EngineData | doc, raster, psd | 14,374 |
| `vector` | 路径/形状/矢量蒙版 → 覆盖率（自研扫描线） | doc, raster | 4,829 |
| `compose` | **CPU 参考合成器**（一切 GPU 结果的判据） | doc, vector, raster | 21,042 |
| `gpu` | wgpu 合成器 + WGSL 核 + 常驻缓存 + 内存预算 | compose, doc | 11,918 |
| `format` | `.pcraft`：manifest + 内容寻址 tile(zstd) | doc, codecs | 8,361 |
| `io` | 文档 ⇄ 各格式映射（PSD 映射 2,326+1,352 行、智能对象 4,070 行） | 几乎所有 L0–L3 | 47,833 |
| `plugins` | wasm 插件宿主（wasmi，无 host import，fuel/内存/栈/时限） | geom, color, raster | 1,754 |
| `engine` | `Session`：命令注册表、事务式 `edit`、作业、事件、偏好 | 全部 L1–L4 | 146,364 |
| `ui-egui` | egui 外壳（逻辑薄、代码厚） | engine, gpu | 240,354 |
| `automation` | MCP server（rmcp）+ JSON-lines 控制协议 | engine | 8,855 |
| `testkit` | 金标图、感知 diff、fixture 助手 | **无**（dev-only） | 328 |

**依赖方向的三个关键约束**（`xtask/src/layers.rs`）：
1. 只能依赖**更低**层；同层内另有固定次序（`INTRA_LAYER_ORDER`：`geom→cms→color→raster`、`compose→gpu`）。
2. `psd`/`codecs`/`cms`/`raw`/`affinity` 被标为 `Standalone`——**不允许依赖任何 workspace crate**，以保证可单独发布。唯一例外是 `codecs → heif`。
3. L6 以下（含 L5 engine）**不得出现 egui/eframe/winit/rfd**。这就是"UI 可换"在工程上的落地方式：不是靠约定，是靠 CI 报错。

---

## 3. 三个核心设计

### 3.1 文档模型：图层树 + 稀疏 COW tile + 位深/色彩模式全运行时化

**设计出发点写在 `docs/architecture.md:61-99`**：他们说 GIMP 的教训是"位深、色彩模型、'图层就是一块像素'被烧进核心当成假设，而不是数据"。于是这里从第一天就把这四件事做成运行时字段。

**结构（三层）**：

| 层 | 是什么 | 证据 |
|---|---|---|
| `Document` | 画布尺寸、`mode: ColorMode`（Rgb/Gray/Cmyk/Lab/Bitmap/Indexed/Multichannel/Duotone）、`depth: SampleType`（U8/U16/F32）、ICC profile、`layers: Vec<Layer>`（**底→顶**）、通道、选择、路径、切片、变量、时间线、图层复合 | `doc/src/lib.rs:864-926` |
| `Layer` | id、名称、可见、5 种锁、混合模式、opacity/**fill_opacity 分离**、裁剪标记、像素蒙版、矢量蒙版、效果、标签色、`psd_blocks`（保留未建模块）、link_group、Blend If、高级混合、XMP | `doc/src/lib.rs:614-666` |
| `LayerContent` | 7 元枚举：`Raster(Surface)` / `Group` / `Adjustment` / `Fill` / `Text` / `Shape` / `Smart` | `doc/src/lib.rs:582-590` |

**像素存储**：`Surface` = `PixelFormat` + 默认像素 + `BTreeMap<TileCoord, Arc<Tile>>`，256² 一块，缺块读默认值（图层透明 / 蒙版全白）。写时 `Arc::make_mut` 触发 COW（`raster/src/lib.rs:1-7, 134-138`）。**后果**：克隆一个 Document 是 O(层数 + 块数) 的指针拷贝，不是像素拷贝——这一个决定同时支撑了 undo、后台作业、autosave、UI 无锁读四件事（`docs/architecture.md:56`）。

**几个值得抄的细节**：
- 所有可能很大的载荷都是 `Arc` 共享：XMP、EXIF、原始 PSD 资源/块、SmartObject 源字节、未支持调整的原始字节（`doc/src/lib.rs:300, 400, 514, 554, 641, 845-861`）。注释直接写明"a snapshot never copies one"。
- 递归有硬上限：`MAX_GROUP_DEPTH = 100`，理由写得很实际——Windows 与 wasm 的主线程栈只有 1 MiB（`doc/src/lib.rs:307-313`）。
- `Background` 图层**按结构识别，不按名字**：最底层 + 非裁剪的像素层 + 锁定透明与位置。这样德语" Hintergrund"、日语"背景"文件也正确（`doc/src/lib.rs:1099-1106`，并有对应测试）。
- 22 条 `layer` 属性里，`psd_blocks: Vec<([u8;4], Arc<Vec<u8>>)>` 是关键：**没建模的 PSD 块原样存着，导出时原样写回**，这是"打开→保存不丢东西"的物理基础。

**代价（必须看到的另一面）**：tile 是 256² 定长块，一个 1 像素的笔触就要抬 256 KiB（RGBA8）；32-bit 一张块 1 MiB。历史里 50 步、每步改一个块，就是 50 块。他们的 `History::trim_with` 就是为这件事写的字节预算（见 3.2）。

### 3.2 命令/撤销体系：一切皆命令，但**撤销是全文档快照，不是逆操作**

这是本次逆向**最反直觉、也最该讲清楚**的一点。任务书假设的"命令模式 / Op 逆操作栈"在这里**不存在**。`crates/ops` 只有 1,396 行，且只有历史（`ops/src/lib.rs:41`），没有任何 `Op` trait、`apply/revert` 对。

**真实机制（三段式）**：

```
UI/CLI/MCP/自动化
   │  按 id + JSON params 调用
   ▼
Session::execute(id, params)                    engine/src/lib.rs:692
   │  → dispatch → 参数校验 → 前置条件 → 取 run 函数指针
   ▼
Session::edit(label, |&mut Document, &mut active| …)   engine/src/lib.rs:833
   │
   ├─① let before = st.doc.clone()             :842   ← 只有 Arc 指针拷贝
   ├─② let mut doc = (*before).clone(); f(&mut doc)  :844-847  ← 真正的修改发生在副本上
   ├─③ st.doc = Arc::new(doc)                  :851   ← 原子换掉当前快照
   ├─④ damage = step_damage(&before, &st.doc)  :885   ← 结构 diff 求脏矩形
   └─⑤ history.record(label, before, layers)   :892   ← 把"旧快照"压进历史
```

**命令注册表**：`CommandSpec` 有 8 个字段——`id / label / menu 路径 / 默认快捷键 / params 文档 / enabled 判定 / run 函数指针 / 是否记日志`（`engine/src/commands.rs:15-28`）。注册方式是一个 `build()` 函数里几百行 `v.extend(crate::xxx_cmds::specs())`（`engine/src/commands.rs:1491-1529`），每个领域一个 `*_cmds.rs`，各自导出 `specs()`，最后 `OnceLock` 建 id→index 的 HashMap（`:198-206`）。

**631 个菜单项怎么组织**：`menu_catalog.rs` 是一张**静态表**，每行 `(菜单路径, 标签, 默认快捷键, 命令 id)`，`"---"` 表示分隔线，共 770 行（`ui-egui/src/menu_catalog.rs:6`）。菜单渲染时逐行查 `is_live(id)`（`ui-egui/src/menus.rs:922-937`）——命中 engine 命令、工具、UI 专有命令、面板别名、工作区名、预设之一就算 live，否则渲染成灰色不可点。

这个设计的经济性很好：**加一个功能 = 在 `*_cmds.rs` 里加一条 CommandSpec（id 与 catalog 完全一致）；菜单项自动变亮**。UI、CLI、JSON 控制通道、MCP 全部按 id 分发（`AGENTS.md:63`），所以"给 Agent 加一个能力"不需要写第二份代码。

**撤销粒度与内存策略**（`ops/src/lib.rs`）：

| 机制 | 做法 | 证据 |
|---|---|---|
| 状态内容 | `HistoryState { label, doc: Arc<Document>, layers: LayerTarget, id }`——**整份文档快照** | `:30-38` |
| 为什么可行 | 像素是 Arc tile，快照只多占"真正改过的块" | `:3-9` |
| 撤销语义 | 把 `Arc` 换回去；`undo(current)` 把 current 放进 redo 队列 | `:159-179` |
| 上限 | `max_states`（默认 50）+ `max_bytes` 字节预算 | `:41-64, 85-99` |
| 预算计算 | 只在需要时懒建 tile 计数器；用 `Arc::as_ptr` 去重，共享块只算一次 | `:347-363, 473-483` |
| 淘汰顺序 | 最旧先丢，但**永远保留最近一步**（否则最后一次操作不可撤销）；History Brush 的源状态被"钉住"不淘汰 | `:396-469` |
| 合并（coalesce） | 命令可带 `"coalesce": "<key>"`，key 相同且中间无其它编辑时**共用一个历史步**——滑块拖动、连续打字都只产生一步 | `engine/src/lib.rs:686-691, 887-898` |
| 面板语义对齐 | 每个状态记住"当时选中的图层"，undo/redo 会把选择一起还原（Photoshop 行为）；选图层本身不是一步 | `:20-28, 152-155` |
| Clear History 可撤销 | 被清掉的状态先存进 `cleared`，下一次 undo 放回去，下一次新编辑才真丢 | `:310-330` |

**脏矩形**：`step_damage(a, b)` 对两棵树按 layer id 做配对 diff，能算出"只有这些像素可能变了"；算不准时（顺序变了、clip/channel 排除变了、视频层）返回 `None` 表示"全画布"（`engine/src/layer_multi_cmds.rs:957-1016`）。注释里给了一个真实教训：一次套索选区过去会重合成整幅大图，CPU 路径要几秒（#1773），所以选择/参考线/改名这类"零像素"步骤要显式返回空矩形（`engine/src/lib.rs:876-884`）。

**评价**：
- 优点：正确性"显然"、实现极简、后台作业/autosave/UI 读取全都免费拿到一致快照、无锁。
- 缺点：**每次属性变更都是一次全文档 clone**。`Arc::new(doc)` 后旧快照留在历史里，下一个 `record` 又压一份。改一次不透明度 = 复制整棵图层树（指针级）+ 一次全量结构 diff。这直接解释了 scorecard 里 P1–P8 全部超标（见 §7）。**这是全项目最大的性能债，也是我们不该照抄的部分。**

### 3.3 GPU 合成：一份 plan，两个后端；常驻页 + checkpoint 前缀复用

**架构原则**：合成语义只在一个地方编码——CPU 参考实现。GPU 必须与之在 8-bit 输出下误差 ≤ 1/255（`docs/architecture.md:347`）。这条原则让"GPU 结果和导出结果不一致"这类经典 bug 变成可测的。

| 环节 | 做法 | 证据 |
|---|---|---|
| 语义层 | `compose` 把图层树摊平成"按矩形合成"的直-alpha RGBA f32 缓冲；编码 pass-through/隔离组、裁剪组、蒙版密度、opacity×fill、调整层、Dissolve | `compose/src/lib.rs:1-13, 172` |
| 计划层 | `gpu::plan` 把树变成线性 `Pass` 列表，每个 pass 有 `Kernel` 枚举（`LayerBlend`/`Atop`/`Adjust`/`FxInit`/`FxPaint`/`FxMerge`/`MStroke`/`MBevel*` 等 20 余种）与资源角色（`Content`/`Mask`/`Stroke`） | `gpu/src/plan.rs:31-155` |
| 分块 | 画布按 **1024² chunk** 处理，累加器 `Rgba32Float`（某些设备降到 `Rgba16Float`）；理由写得很具体：Posterize/Threshold/Hard Mix/Dissolve 的阈值判定和 Color Burn/Dodge 的误差放大必须与 CPU 同侧 | `gpu/src/lib.rs:50-52` |
| 常驻 | 图层/蒙版表面按 **up-to-2048² page** 上传；只在被绘制时上传；因为 tile 是 COW，**只有 `Arc` 变了的 tile 才重传**；LRU 淘汰受 `RESIDENT_BUDGET = 4 GiB` 约束 | `gpu/src/lib.rs:7-14, 86-87` |
| 特效缓存 | 图层效果的 shadow/glow/satin/bevel/stroke 距离场按图层状态缓存，预算 1.5 GiB，每帧未用到的图层先淘汰 | `gpu/src/lib.rs:80, 15-18` |
| 前缀复用 | 根栈单元可以把背景 checkpoint 到有界 chunk 纹理；因为快照是 COW，下层前缀可以精确失效——**在上层编辑时只需恢复 checkpoint 并执行剩余 pass**；重复同一编辑（复制粘贴再撤销）能跳更多 | `gpu/src/lib.rs:21-26` |
| 提交节制 | 三个上限防止驱动被压死：`FLUSH_BYTES = 512 MiB`（暂存）、`FLUSH_WORK = 1 GiB`（chunk×pass）、`FLUSH_OPS = 1000`（copy/render pass 数）。第三个的注释直接记了一次真实事故：Metal 把 >4096 未完成 command buffer 当成设备丢失 | `gpu/src/lib.rs:95-105` |
| 逃生口 | 不支持的组合（多通道文档、超大 pattern、重合 stop 的未缓存渐变）返回 `Unsupported`，调用方退回 CPU 合成 | `gpu/src/lib.rs:30-33` |

**这套设计的三个要点**：
1. **CPU 是 oracle，GPU 是优化**。任何 GPU 崩溃/不支持都能退回，产品不会"打不开图"。
2. **缓存键是 "tile 身份 + 世代号"**，不是脏矩形。脏矩形只决定"重算哪些像素"，世代号决定"哪些上传/缓存还能用"。这比纯脏矩形鲁棒，因为 COW 让 tile 身份天然精确。
3. **checkpoint 前缀复用**是它性能的关键，也是它复杂度的来源——需要在 plan 阶段选出"可切分单元"，且组和裁剪单元必须不可分（`gpu/src/lib.rs:23-25`）。

---

## 4. PSD 兼容工程

### 4.1 分层：格式层与文档层物理隔离

`psd` crate **不依赖 workspace 任何东西**（`xtask/src/layers.rs:44`），只做格式级读写：header/resources/layer records/channel data/tagged blocks/descriptor/EngineData/merged composite（`docs/architecture.md:366-380`）。文档映射全部在 `io`：`psd_import.rs` 1,352 行、`psd_export.rs` 2,326 行、智能对象映射 `smart_map.rs` 4,070 行、块处理 `blocks.rs` 1,770 行。

好处是双向的：`psd` 能独立发布、能被别的项目用；`io` 可以放心地"尽量映射，映射不了就保留"。

### 4.2 逼近真实 Photoshop 写法的四个手段

| 手段 | 做法 | 证据 |
|---|---|---|
| **字节稳定往返** | 未修改的文件 `to_bytes()` 与输入**逐字节相等**：通道数据保留原编码、未知 tagged block 保留原始字节、padding 原样保留 | `psd/src/lib.rs:5-9`（doctest 直接断言 `file.to_bytes()? == bytes`） |
| **未知块原样回写** | 图层级 `psd_blocks` + 全局 `psd_global_blocks` + 图像资源 `psd_resources`，三者都存 `Arc<Vec<u8>>` | `doc/src/lib.rs:634-641, 856-860` |
| **保留 Photoshop 的"过期块"清理规则** | 光栅层不得携带填充/智能对象/文字/形状填充块，导出时显式清掉（`RASTER_STALE_KEYS` = `SoCo GdFl PtFl SoLd PlLd SoLE TySh vscg`） | `io/src/psd_export.rs:19-21` |
| **对着 corpus 打分量** | 三套 corpus + 三道地板（floor），低于地板 CI 就红 | 见下 |

### 4.3 "Photoshop 拒绝打开我们的文件"——根因分类

这是最有价值的一节，因为每类都能在代码里找到对应的防御和一个带 issue 号的回归测试。**全部是格式字节层面的"格式契约"违反，不是内容错误**：

| 类别 | 具体失效 | 案例与证据 |
|---|---|---|
| **块顺序** | 32-bit 文档的图层块 `Lr32` 若排在合并透明标记 `Mt32` 之前，Photoshop 直接拒开。修复：图层块必须紧跟最后一个保留下来的 `Mtrn/Mt16/Mt32` 之后 | `io/src/psd_export.rs:1944-1955`，回归测试 `:2075-2092`（issue #5068，在 Photoshop 2026 上实测确认） |
| **填充/对齐** | 图层信息区要 4 字节对齐；若用"偶数填充"会让后续全局块错位。#200。而全局块的 length 字段必须是**精确数据长度**，否则像 Unity PSD Importer 这类严格读者会拒收（#4654） | `io/src/psd_export.rs:1957-1966` |
| **图层 id 冲突/回绕** | 生成的 `lyid` 必须全局唯一；导入的组 id 要保留；当现有 id 已达 `u32::MAX` 时要复用"最小的未用正值"而不是回绕到 0 撞车 | `io/src/psd_export.rs:2060-2074`，测试断言三条 |
| **取值域边界** | 例如 Duotone 网格"既无 0% 也无 100% 点"时写 −1，Photoshop 拒开 | `io/src/duotone_map.rs:275` |
| **智能对象能被"看见"但不可编辑** | 只写扁平像素而不写 `SoLd`+`lnk2`，Photoshop 打开后是普通层，用户认为"坏文件" | `docs/scorecard.md` FILE-215-1 done 条目 |
| **大文件阈值** | >2 GB 必须切 PSB；PSB 的长度字段 64-bit。估算按"未压缩"算会偏保守地被迫切 PSB | `io/src/psd_export.rs:33-42` |
| **窗口/平台限制** | >4 GB PSB 在 Windows 上从 app 内 File›Open 失败（#375），但经 `io` 层测试通道能成功——说明是外壳层而非格式层问题 | `docs/gaps.md` G1 证据段 |

### 4.4 量化：他们的 PSD 到底多准

这是整份报告里**最该抄的度量方式**（`docs/scorecard.md` "File compatibility"）：

| Corpus | 文件数 | Composite oracle 地板 | 往返地板 |
|---|---:|---:|---:|
| 自家 io corpus（`corpus/psd`） | 170 | 148（87%） | 169（99.4%） |
| psd-tools 语料（MIT） | 309 | 249（81%） | 308（99.7%） |
| **Photoshop 亲手生成的 oracle** | 256 | **190（74%）** | 258（100.6%，允许新增） |

**oracle 的定义极聪明**：PSD 文件里本身就存着 Photoshop 自己渲染的合并图（merged composite）。**用我们的合成器渲染各图层，再和文件里 Photoshop 的合并图逐像素比**——等于免费获得了一个 256 张图的、由 Adobe 官方出题的一致性测试集（`docs/architecture.md:356-358`）。地板数只许升不许降。

他们自己也承认这不够：**"有没有在第三方编辑器里打开过我们的导出"这件事没有测试**（`docs/gaps.md` G1，FILE-216-4），只能靠人拿 Photoshop 试。这是 LUMINA 可以直接补上的空白——如果我们有 Photoshop 授权机器或 Photoshop 自动化脚本。

---

## 5. 画笔引擎

### 5.1 分层：模型 / 动态 / 渲染 / 资产

| 模块 | 职责 | 行数 |
|---|---|---|
| `brush.rs` | serde 参数模型：`BrushSettings`（尺寸/硬度/间距/间距开关/opacity/flow/…）、`ShapeDynamics`、`Scattering`、`Texture`、`DualBrush`、`ColorDynamics`、`Transfer`、`Pose`、`Smoothing`、`SectionLocks` | 828 |
| `dynamics.rs` | 输入点 → dab 列表：平滑、brush pose、路径行走（spacing / airbrush build-up）、逐 dab 动态 | 860 |
| `render.rs` | dab 栅格化 + 笔迹缓冲合成（coverage + 可选前景色），支持增量喂入重合成 | 1,116 |
| `tile.rs` | `GrayTile`（笔尖/纹理位图）、mip | 402 |
| `presets.rs` | 内置预设 | 477 |
| `mixer.rs` / `replace.rs` / `retouch.rs` | 混合器笔刷、颜色替换、修复类工具的累计语义 | 1,248 |
| `rng.rs` | **确定性哈希 jitter**：`hash2(x, y, salt)`，靠 (种子, dab 序号) 取随机数 | 84 |

### 5.2 参数模型：完整覆盖 Photoshop 的 Brush Settings

`BrushSettings`（`paint/src/brush.rs:548`）一个结构装下 Photoshop 笔刷面板的全部节；每节用 `#[serde(default, rename_all = "camelCase")]` 做前向兼容。设计上几个点：

- **"Control" 是显式枚举**（`Control`: Off / Pressure / Tilt / Rotation / Wheel / Velocity …），每节有 `Dynamic { control, minimum, jitter }`，所以"随压力变化"不是布尔开关而是三元组（`paint/src/brush.rs:28-83, 140-160`）。
- **`SectionLocks`**：Photoshop 每个节的锁（大小/动态/散布/纹理…），这里也建了模型（`:398-414`）——说明他们连"切换笔刷时哪些设置保留"都建模了。
- **`Pose`** 的 tilt 用 W3C Pointer Events 约定（−90..90 度），rotation 0..360，四个 override 开关独立（`:429-441`）。为兼容旧预设还保留了一个 `PoseWire` 迁移层（`:446-520`）——预设格式演进的现实做法。
- **`Smoothing`** 把 Photoshop 的六件事都做成了字段：enabled（可关闭但保留数值）、amount、pulled_string（懒人鼠标）、catch_up、catch_up_on_end、adjust_for_zoom（`:524-546`）。

### 5.3 笔迹语义：**stroke buffer 私有图层** + build-up up to stroke opacity

这是他们在正确性上最用心的部分（`paint/src/render.rs:1-8`、`paint/src/lib.rs:5-9`）：

1. 一笔画在一个**稀疏 tiled 的"笔迹缓冲"**里，不是直接写图层像素。缓冲里存 coverage（开 Color Dynamics 时还存预乘颜色）。
2. dab 之间用 **flow 累积到各自 opacity 上限**（Photoshop 的 Flow vs Opacity 区别）。
3. 整笔一次性乘 **stroke opacity** 合成到"落笔前的像素"上。
4. 因为合成**永远从落笔前的像素开始**，所以可以分段喂入、增量重合成——这就是实时预览能边画边显示又不越画越黑的原因。

对 LUMINA 的启示：如果我们的画笔是"逐 dab 直接往图层上 alpha 混合"，那么同一笔交叉处就会变深，且无法做 stroke-level opacity。**这是画笔手感差异的结构性根因，不是参数没调对。**

### 5.4 确定性与可重放

`dynamics.rs:1-5` 明确：一切增量（任意分块喂点得到同样 dab），一切 jitter 来自 `(stroke seed, step/dab index)` 的哈希。后果：
- 自动化/重放能复现同一笔；
- 实时预览与最终提交像素一致（不会"松手后变样"）；
- 8-bit 量化前加**位置哈希抖动**（`dither8`，`render.rs:30-40`），让低 flow 的细腻渐变变成颗粒而不是色阶，且预览/提交/重放三者一致。

### 5.5 他们自己的评估

`docs/brush-parity.md`：**广度 ~90%（每节每个选项都有），可实用度 ~55%**。缺的是"手感"：延迟（#2619 大笔刷卡）、压力响应（Windows 上跳变 #2046）、边缘曲线（#2149）、插值/平滑匹配（#2138）、tap 灵敏度（#1798/#1933）、Windows 上没有旋转（#1301）。**结论对我们很有用：把 Brush Settings 全部做出来只值 55%，剩下 45% 是曲线拟合和延迟工程，且必须真机对着 Photoshop 比。**

---

## 6. 工程纪律（最值得抄的部分）

`AGENTS.md` 不是写给 AI 看的装饰，它和 CI、clippy、xtask 是咬合的。逐条评估能否搬到 C++ 项目：

| # | 规则 | 他们的实现 | 能否照搬到 LUMINA（C++17/CMake） | 迁移成本 |
|---|---|---|---|---|
| 1 | **Never crash > 功能** | 非测试代码禁止 `unwrap/expect/panic/unreachable/todo/unimplemented`；越界索引用 `get()`；输入决定的尺寸用 `checked_*/saturating_*`；递归有深度上限；锁中毒用 `into_inner`；外壳层兜底 catch panic 并保住文档（`AGENTS.md:50-61`） | **能，且应该。** C++ 里对应：禁裸 `assert` 做输入校验、禁用 `at()` 之外的 `[]` 处理外部索引、所有 `new` 走 checked 分配、`-fno-exceptions` 不行就用异常边界 | 中 |
| 2 | **clippy 强制** | `clippy.toml` 只在测试里允许 unwrap/expect/panic/indexing；每个 crate 顶层 `#![deny(...)]`（26/26 crate 都有，实测；scorecard 记作 30/30，口径含 apps/xtask） | **等价物**：clang-tidy 的 `bugprone-*`、`cppcoreguidelines-pro-bounds-*`、自定义 `-Werror` 集 + 一个 grep 白名单门（禁止 `strcpy/atoi/[]` 于外部索引路径） | **低到中**（clang-tidy 比 clippy 啰嗦，误报更多） |
| 3 | **分层由工具检查** | `cargo xtask layers` 解析 `cargo metadata`，比 `TABLE` 表；违规即红 | **能**：CMake target 层面本来就有依赖，写个脚本解析 `link_libraries` 建图做拓扑校验即可 | **低**（我们已经是按 target 组织） |
| 4 | **parity 可自动跑** | 菜单 631 项 × 命令注册表 → 覆盖率；`FLOOR = 631`，测试断言 `live >= FLOOR`；文档由 `cargo xtask parity` 重新生成，CI 检查未提交的生成物 | **能，且强烈建议**：把"PS 菜单项 → LUMINA 命令 id"做成静态表 + 注册表反查，生成 `checklist.md`，并在单元测试里钉住下限 | **低**（1 张表 + 1 个生成器 + 1 个断言） |
| 5 | **scorecard 由数据生成** | `scorecard/*.toml` + `perf/budgets.toml` + `perf/baseline.json` + corpus 地板 → `docs/scorecard.md`；CI 跑 `--check` 防手改 | **能**：用 JSON/TOML 作为唯一事实源，生成 Markdown，`git diff --exit-code` 守门 | **低到中** |
| 6 | **panic_hunt** | 对**每条命令**喂对抗参数，独立线程 + 4 秒超时；panic 通过进程级 hook 计数（因为引擎自己会 catch，unwind 到不了测试层）；同一用例超时算 hang，**重跑一次仍超时才判 hang**（排除首次字体扫描 3.5–5 s 的一次性开销） | **能**：C++ 里对应 `fork`/子进程 + 超时，或 SEH/`std::set_terminate` + 计时。语义完全相同 | 中 |
| 7 | **corpus 不提交、按 assert 版本拉取** | `cargo xtask corpus --all` 按 `corpus_pins.rs` 的 commit 拉取；`corpus/` 被 gitignore；CI 有专门 job 缓存 | **能**：git submodule 或带 SHA 的下载脚本。关键是"永不提交二进制语料" | 低 |
| 8 | **禁止 `unsafe`** | workspace `unsafe_code = "forbid"`；唯一例外是 `tablet` crate 的 macOS/Windows 两文件，每个块要 `SAFETY:` 注释 | **不完全适用**：C++ 没有"safe 子集"。可借鉴的是**"把不安全集中到一个 crate/目录"**的思路 | — |
| 9 | **原子写盘** | `.pcraft` 增量保存 + 原子替换；Windows 用 `ReplaceFileW` + backup name（少了 backup 会丢文件） | **能，且必须**：见 `docs/decisions.md` 2026-10-17 条，论证过程值得完整读 | 低 |
| 10 | **原生格式故意编译失败** | `format` crate 在 `doc` 结构体新增字段时**故意不编译**，强制开发者补 manifest/convert 映射，防止静默丢数据（`AGENTS.md:137-142`） | **能，非常巧**：C++ 里用 `static_assert(sizeof(Document) == N)` 或"序列化字段清单与结构体字段清单必须等长"的编译期检查 | **低** |
| 11 | **并行 Agent 协作约定** | 每人独立 `CARGO_TARGET_DIR`；新命令放**新模块**不挤共享文件；共享文件小步改；写 manifest 要用临时文件再 `mv`（`crates/*` glob 一个坏 manifest 全员不编译） | **部分适用**：前三条适用；`mv` 那条是 Rust glob 特有 | 低 |

**最值得单独强调的一条**：`AGENTS.md` 每条规则都能指向一个**执行机制**（clippy lint / xtask 子命令 / 测试 / CI job），没有一条是"请大家注意"。这是它作为工程文档有效的真正原因——**规则 = 检查**，不是规则 = 倡议。

---

## 7. 他们踩过的坑（风险预警）

数据来自 `docs/scorecard.md` 的 64 个 perf 场景（**只有 3 个达标，22 个超标/失败**）和 `docs/gaps.md`。

| 坑 | 现象 | 根因（我们的判断） | 对 LUMINA 的预警 |
|---|---|---|---|
| **属性变更极慢** | P1 移动带样式的图层 321 ms（目标 ≤25）；P2 改不透明度 276 ms（≤20）；P5 切可见性 178 ms（≤10）；P8 复制 12MP 图层 508 ms | 每次属性变更都走 `Session::edit`：全文档 clone + 全量 `step_damage` 结构 diff + 重新合成（§3.2） | **如果我们照抄"全文档快照"必定重蹈覆辙。**必须要么走"属性单独成 op、脏区显式给出"，要么把 clone 做成增量结构 |
| **大画布移动图层** | P14 在 15000×10000 16-bit 文档移动图层 **2,698 ms**（目标 ≤80） | 整层重传 GPU + 整块重合成 | 大画布场景要按"块级移动"处理，不能整层重传 |
| **GPU 直接崩** | **P12**：150 层文档里按方向键微移 → `wgpu Validation Error: Buffer with 'pc_compose_uniforms' label is invalid`，整个场景"missing" | 单次提交的 render pass 数量超过后端上限（Metal >4096 未完成 command buffer 当设备丢失）。他们后来加了 `FLUSH_OPS = 1000`（`gpu/src/lib.rs:100-105`），但 P12 仍是 unresolved | **必须做"提交分片"**：按 (字节, 工作量, pass 数) 三个维度切 submit。并且要有 device-lost 恢复路径（`gpu/src/health.rs` 就是干这个的） |
| **Content-Aware Scale 慢到不可用** | P28：24 MP 缩到 90% 宽 = **954,192 ms（约 16 分钟）**，目标 3,000 ms；内存 2.1 GB | 算法本身复杂度 + 无可用 GPU 实现 | **这类"前沿算法"不要排进 v1.2 主线**；即便做，也要先设定"超时降级为普通缩放"的 UX |
| **文本工具卡死** | 选 Type 工具后**冻结约 60 秒**（#2518，macOS，未复现）；首次 `type.create` 会扫描全部系统字体，负载高时 3.5–5 s（#2994，`engine/tests/panic_hunt.rs:83-87` 的注释） | 字体库构建在 UI 线程同步执行 | **字体扫描必须异步 + 有可见进度**；首个文本操作不允许阻塞帧 |
| **CMYK 画笔慢** | P33：A4 300ppi CMYK 一笔 34.7 ms p50 / 235 ms p95（目标 6 ms） | CMYK 需经 ICC 转换才能混合 | 若 LUMINA 支持 CMYK 编辑，笔刷路径必须避免每 dab 做 ICC 变换 |
| **选区/修饰类中招** | P26 Smooth 99.9 ms、P27 Feather 122 ms（目标 100 ms）；P29 Select Subject 559 ms（目标 100 ms） | 全分辨率逐像素 + 无分块并行 | 选区运算要按 256² 块并行，且要能复用上一版结果 |
| **"改一个数字要等 3 秒"类** | P41 36MP 高斯模糊 r40 = 2,963 ms；P48 14000² 全刷新 8,715 ms | 单层全画布、未走 GPU | 预览必须走降采样代理（他们有 `compose::proxy` 模块） |
| **文档与实现脱节** | `architecture.md` 写 25 crate / 539k 行，实测 26 / 684k；`ROADMAP.md` 写版本 0.6.0，`Cargo.toml` 是 0.15.1 | 文档靠人维护 | 数字要么生成（scorecard 做法），要么标明"截至某日" |

---

## 8. 对 LUMINA 的可借鉴清单

### 8.1 架构思想可借鉴（直接照搬概念，不需担心许可）

| 思想 | 为什么值得 | 落地建议 |
|---|---|---|
| **一切皆命令 + 命令注册表** | UI/CLI/MCP/录制宏共用一份 dispatch；菜单项与命令 id 强绑定，加功能不用改 UI | 在 C++ 引擎里建 `CommandSpec{id,label,menu,shortcut,paramsSchema,enabled,run}` 表；UI 从表生成菜单 |
| **分层由工具强制** | 保证"引擎不依赖 UI"不靠自觉 | CMake target + 依赖图脚本 + CI 校验 |
| **CPU 参考实现 + GPU 实现 + 误差 ≤1/255 的对拍** | 消灭"导出和屏幕不一致"这类最难查的 bug | 先写 CPU 逐像素版本作为 oracle，GPU 版本进 CI 对拍（可用软件适配器） |
| **PSD 合并图当免费 oracle** | 免费用 Adobe 自己出的题 | **立刻可做**：拿一批 PSD，比较"我们的合成结果" vs "文件内嵌合并图" |
| **稀疏 COW tile（256²）** | 一次决定换来 undo / 后台作业 / 无锁读 / 增量保存四件事 | 我们若要改造，先在"新建图层/涂抹"路径试点，不要全量重构 |
| **未知格式块原样保留** | "打开→保存不丢东西"的唯一可靠办法 | 在任何格式读写层都加 `Vec<(key, bytes)>` 逃生舱 |
| **菜单 parity 用 floor 钉住** | 防止"重构完发现少了 20 个菜单" | 一张表 + 一个断言，成本极低 |
| **scorecard 由数据生成 + CI 防手改** | 让"我们到底多快/多准"变成可审计的数字 | 用 JSON 作事实源，生成 md |
| **原生格式故意编译失败** | 防静默丢字段 | `static_assert` 守字段清单 |
| **undo 面板语义对齐 Photoshop**（状态记住当时选中图层、选图层不是一步） | 用户肌肉记忆 | 在历史项里带 `activeLayer/selected` |
| **coalesce key 合并历史步** | 滑块拖动/连续打字只产生一步 | 命令可带可选 `coalesce` 参数 |

### 8.2 算法思路可借鉴（需自研实现，不可读其代码）

> 注意：**这些是"知道该往哪个方向做"，不是"可以翻译它的实现"**。下面的内容全部来自公开论文/公开规范，他们在文档里也标了出处。

| 思路 | 出处（他们标注的） | 我们的动作 |
|---|---|---|
| 笔迹缓冲 + build-up up to stroke opacity + 从落笔前像素重合成 | Photoshop 行为观察 | 自行设计（这是结构，不是秘密算法） |
| 低 flow 8-bit 量化前加位置哈希抖动 | 自研 | 自行实现 |
| jitter 用 `(seed, dab index)` 哈希而非 PRNG 流 | 自研 | 自行实现（保证可重放） |
| dab 面积覆盖率（小半径笔尖用 4×4 子采样） | 自研 | 自行实现 |
| 高斯模糊/双边/PatchMatch/Poisson/ARAP 等 | Malvar-He-Cutler 2004、Hirakawa-Parks 2005、Lu-Tan 2003、PatchMatch、Poisson blending、ARAP | 读论文自己写 |
| ICC 变换：四面体插值、BPC、3D LUT | ICC.1:2010、CIE 15、Adobe BPC 论文 | 用 lcms2（MIT）或自研 |
| PSD 规范：header/资源/图层记录/通道压缩/tagged block/descriptor/EngineData | Adobe 公开 PSD 规范 | 按规范实现；注意 §4.3 的七类"格式契约"坑 |
| DNG/CR2/NEF/ARW 解码 | TIFF/EP、DNG 规范 + 观察样本文件 | 见 §9 的 LibRaw 议题 |

### 8.3 不可用

| 项 | 原因 |
|---|---|
| 直接复用其 Rust 源码 | 语言栈不匹配（我们是 C++17）；跨语言移植工时 ≈ 重写；且一旦移植就继承其架构债（§7 的性能问题） |
| 其字体（Inter/JetBrains Mono 及 craft-fonts） | 字体本身是 OFL，可用；但**他们明确"永不提交字体"**，走独立仓库 + 构建期注入。我们可以直接用 OFL 字体，但不要把字体塞进 git |
| 其 `docs/brand/` 里的 ArtCraft logo | **不在 MIT/Apache 范围内**（`NOTICE` 明确"不是开源"），商标 |
| 其 `craft-ui` / `craft-print` / `craft-docpath`（私有 craft-libs 的 git 依赖） | 私有仓库，外部不可得；且是 Rust |
| `libfacedetection` 权重（algo crate 内） | BSD-3-Clause，**可用但必须保留 LICENSE 与 ATTRIBUTION 行**；若我们用需单独合规 |
| `docs/brand`、`ATTRIBUTION.md` 中的每个第三方资产 | 每个都要单独核许可，不可整体当成 MIT |

---

## 9. 许可与红线

### 9.1 PhotoCraft 自己的许可边界

| 对象 | 许可 | 我们的义务 |
|---|---|---|
| `crates/*`、`apps/*` 源码 | **MIT OR Apache-2.0** 双许可（`Cargo.toml:9`、`NOTICE`） | 若参考/移植：保留版权与许可声明；Apache-2.0 分支还需保留 NOTICE 与专利授权条款 |
| `docs/brand/` 品牌资产 | **非开源**（`NOTICE`、`docs/brand/LICENSE-brand.txt`） | 不得使用 |
| 第三方资产（字体/图标/词表/人脸检测权重） | 各自独立（OFL-1.1 / ISC / MIT / SCOWL / BSD-3-Clause） | 逐个核；他们要求"同一提交内加 `ATTRIBUTION.md` 一行" |
| 语料（PSD/XCF 测试文件） | 各上游（psd-tools MIT、ag-psd MIT、PngSuite、自家 photoshop-corpus） | **永不提交进仓库**，按 commit pin 拉取 |

### 9.2 他们自己拒绝的来源——以及为什么

`crates/raw/README.md:63` 一句话说得很硬：

> No code from dcraw, LibRaw, rawspeed, rawler, rawloader or darktable was read or used, and no camera colour tables were copied.

`crates/raw/README.md:80` 给出了代价：

> Nikon "lossy after split" NEF、Sony "Compressed RAW 2"、Pentax 压缩 PEF、RW2 RawFormat 6+、Olympus 压缩 ORF → **Unsupported**：这些编码除 GPL 解码器源码外没有公开描述，而本 crate 不得使用（clean-room）。`photocraft-io` 改为打开文件内嵌的 JPEG 预览。

也就是说：**他们宁可在这些机型上退化成"打开内嵌预览"，也不碰 GPL 血统的实现。** 这是 clean-room 纪律的直接代价，也是它最真实的一面。

同时要看到一个 nuance：`docs/architecture.md:58, 186` 其实**允许** C/C++ 依赖，且明确举例"an optional LibRaw"，条件是：只在 Cargo feature 后、只在 IO 边缘 crate、默认不开、web 目标绝不开。**所以他们对 LibRaw 的态度是"架构上留了口子，clean-room 上拒绝使用"**——两件事不矛盾。

### 9.3 与 LUMINA 现状的直接冲突

**事实核对（本机实测，非推断）**：

| 事实 | 证据 |
|---|---|
| LUMINA 引擎**静态链接** LibRaw | `engine/CMakeLists.txt:104` 链接 `${THIRDPARTY_DIR}/libraw/lib/libraw.a`；`:17-19` 缺 `libraw.a` 直接 `FATAL_ERROR` |
| 全部静态运行时 | `engine/CMakeLists.txt:29`：`-static-libgcc -static-libstdc++` |
| LibRaw 的许可**不是 GPL** | `thirdparty/libraw/LICENSE.LGPL`（LGPL-2.1）+ `LICENSE.CDDL`（CDDL-1.0）双许可，由使用者择一 |
| LibRaw 的**血统**来自 dcraw（GPL） | 上游 README 与业界共识；photocraft 正是因此拒绝读它 |

**冲突性质要说清三件事，不能混为一谈**：

1. **不是 GPL 传染**。LGPL-2.1 / CDDL-1.0 都不要求 LUMINA 整体开源。任务书里"GPL 污染"这个说法对**许可**不成立。
2. **静态链接 + LGPL-2.1 有明确义务**。LGPL-2.1 §6 要求：以目标码形式分发"与库结合的作品"时，必须提供**可重新链接**的形式（典型做法：随包提供 LUMINA 自身的 `.o`/`.a` 或链接脚本，或改为动态链接 LibRaw），并附许可文本与 LibRaw 源码获取方式。**"静态 .a + 全静态运行时 + 不提供 relink 材料"是不合规的**，这一点与我们是否闭源无关。
3. **clean-room 主张与 LibRaw 不相容**。这跟许可无关，是"来源清白"的问题：LibRaw 的算法实现源自 dcraw。若 LUMINA 对外声称"引擎 clean-room 实现"，同时又静态链接 LibRaw，这个声明**在 RAW 解码模块上不成立**。photocraft 拒绝 LibRaw 的真正理由是这一条，不是许可。

**可选处置方案（不替团队决定，只列选项与代价）**：

| 方案 | 做法 | 代价 | 能解决什么 / 不能解决什么 |
|---|---|---|---|
| **A. 换解码器** | 用许可更清晰、且非 dcraw 血统的解码路径：DNG 走 DNG SDK（Adobe，BSD 风格）或自研；机身专有格式先只做"取内嵌预览"降级 | **最高**：photocraft 用了 43 文件 / 14,486 行仍只覆盖部分机型；自研全格式不现实 | 解决 1/2/3 全部。业绩上损失机型覆盖 |
| **B. 改动态链接 + 履行 LGPL** | `libraw.dll`/`.so` 动态链接，随包附 LGPL 文本、LibRaw 源码或获取方式、以及"如何替换该库"的说明 | 中：打包与安装器要改；Windows 上多一个 DLL；需法务确认 §6 满足方式 | 解决 2。**不解决 3**（血统问题依旧） |
| **C. 维持静态 + 补齐 relink 材料** | 保持 `libraw.a`，但发布物中提供 LUMINA 自身目标文件（或 `.a`）与链接命令，允许用户重链接替换 LibRaw | 低到中：发布物体积增大；需要构建流程输出这些产物；需法务确认"提供目标文件"是否足够 | 解决 2。**不解决 3** |
| **D. 隔离为独立进程/独立动态库** | RAW 解码放进独立可执行文件，通过 IPC 传像素；LibRaw 只与该进程静态链接 | 中：多一个进程、IPC 开销、崩溃隔离反而变好 | 解决 2；对 3 有**部分**帮助（"引擎主体"可声称 clean-room，RAW 子进程单独声明为 LGPL 组件）——但这是话术层面的修补，需法务认可 |
| **E. 维持现状，只修正对外表述** | 不声称整体 clean-room；明确写出"包含 LGPL-2.1 组件 LibRaw"，并补 §6 材料（否则仍是 C 的一部分） | 最低（除 C 的材料工作外） | **只解决表述，不解决合规**。若不做 C 的 relink 材料，仍是 2 的不合规 |

**给决策者的最小事实包**：
- 若 LUMINA 对外**不宣传 clean-room**，且发布物**提供了 relink 材料**，现状（静态 LibRaw）在法律上是站得住的。
- 若 LUMINA 要对外宣传 clean-room，或要参与需要"来源清白"的投标/审计，则必须走 A 或 D。
- 若 LUMINA 只内部使用、不分发二进制，则 LGPL 义务不触发（但这一点通常不成立）。

### 9.4 其他红线

| 红线 | 说明 |
|---|---|
| 不得拷贝本仓库代码 | 即使 MIT/Apache 允许，跨语言移植会继承其架构债（§7），且我们无法审计移植质量。**只取思想。** |
| 不得拷贝其字体文件进 git | 他们自己都不这么做（`AGENTS.md:65`） |
| 不得使用 ArtCraft 品牌资产 | 非开源 |
| 语料不入库 | 二进制测试文件走 pin + 下载脚本 |
| 不得声称"复刻 Photoshop 行为"来自逆向其二进制 | 他们的 clean-room 定义是：只研究**行为与外观**，实现来自**公开规范 + 论文 + 观察**（`AGENTS.md:65`）。LUMINA 的 R1/R2 报告若基于反编译，则不能同时声称 clean-room |

---

## 10. 一页总结

**他们做对的三件事**（值得 LUMINA 直接吸收）：
1. **规则 = 检查**。`AGENTS.md` 的每条纪律都挂着一个 clippy lint / xtask 子命令 / 测试 / CI job，没有一条靠自觉。
2. **度量先行**。菜单 parity 用 floor 钉住、perf 用 64 个场景 + 预算、PSD 用三套 corpus + 地板、scorecard 全部由数据生成且 CI 防手改。**"我们有多好"是可审计的数字，不是感觉。**
3. **CPU 是 oracle，GPU 是优化**。误差 ≤1/255 的对拍把最难查的一类 bug 变成可测项；不支持就退回 CPU，产品不会打不开图。

**他们做错/付了代价的三件事**（LUMINA 必须避开）：
1. **撤销 = 全文档快照**。优雅但昂贵：P1–P8 全部超标 5–20 倍。我们若照抄，属性面板的每次拖动都会卡。
2. **提交不分片**。P12 直接把 GPU 打崩（`pc_compose_uniforms` invalid）。必须按 (字节, 工作量, pass 数) 三轴切 submit，并有 device-lost 恢复。
3. **文本/字体路径同步阻塞**。选 Type 工具冻结 60 秒的 bug 挂了一个多月。字体扫描必须异步 + 首屏不阻塞。

**给 LUMINA v1.2.0 的落点建议**（按投入产出排序）：
1. **本周可做**：把"PS/ACR 菜单项 → 命令 id"表建起来 + parity floor 断言 + `parity-checklist.md` 生成器。（1 张表 + 1 个脚本 + 1 个测试）
2. **本迭代可做**：PSD 合并图 oracle 对拍器（拿现有 PSD 语料跑，立刻知道我们的合成准确率）。
3. **本迭代应做**：命令注册表 + 分层检查脚本 + scorecard 生成器（三件套是后续一切度量的地基）。
4. **需要单独立项**：§9.3 的 LibRaw 处置（法务 + 打包 + 对外表述三线并行），不要拖到发版前。

---

*报告基于 README/AGENTS.md/docs 与 26 个 crate 源码的实际阅读，代码规模、测试数、依赖关系、行号均为本机实测。未修改 refs 目录或 LUMINA 源码中的任何文件。*
