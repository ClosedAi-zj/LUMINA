# R4 · lightcraft 白盒逆向工程报告

> **对象**：`D:/work/project/pycharm/p/refs/lightcraft/`（上游 <https://github.com/storytold/lightcraft>，浅克隆 `23f5480`）
> **许可**：MIT OR Apache-2.0（`LICENSE-MIT`、`LICENSE-APACHE`、`NOTICE`）
> **分析方法**：只读源码与文档；未反汇编任何 Adobe 二进制；未运行其测试
> **证据约定**：`file:line` 为本地仓库相对路径；无源码证据的结论标 **〔推断〕**
> **用途**：LUMINA v1.2.0 架构决策输入（R4）

---

## 0. 摘要（先看这十条）

1. **规模实测**：0.13.0，**736 个 `.rs` / 321,978 行 / 29 个 crate + 3 个 app + xtask**。其中 `engine` 92.7k + `ui-egui` 82.8k 占 **55%**；真正的像素算法（`raw` 38.3k + `pipeline` 12.6k + `gpu` 7.0k）只占 **18%**。
2. **管线的核心设计不是"顺序"，而是"一个 `Plan` 同时喂 CPU 和 GPU"**：`pipeline::plan()` 一次性算出有效设置、几何帧、输出尺寸、缓存键，GPU 侧复用同一个结构（`crates/pipeline/src/lib.rs:360-459`）。这消除了双实现漂移，是我们最该抄的一条。
3. **真实管线是 5 段**：geometry → scene-linear → tone map → colour → display。全流程 **f32 场景线性 Rec.2020**，输出前才降到 8-bit；**没有半精度**（半精度只出现在降噪产物缓存 `.lcdn`）。
4. **"每像素阶段"才是主体**：13 个有序步骤压在一个循环里（`finish.rs:398-666`），曝光、局部色调、清晰度、纹理、锐化、降噪增益、校准、色调映射、色彩、暗角、色域映射、编码、曲线、颗粒按固定次序叠加。顺序错了颜色就错，而这份顺序是**逐条调出来的**。
5. **GPU 不是"可选加速"，而是"随时可整体退回"的第二实现**：提交切片 ≤16M 调用、每阶段错误作用域、读回 60s 超时、全黑结果自动重渲、崩溃哨兵文件。等价性判据 **mean|Δ|<0.5 LSB、max|Δ|≤3 LSB**，实测 max 1 LSB。
6. **Catalog 不是 SQLite**：是 **JSON Lines 追加式操作日志 + 周期快照**（`journal.rs:1-40`），36 个 `Op` 变体覆盖全部状态，格式版本 9，后台压缩，撕裂尾部可恢复。这与 LR 的 `.lrcat`(SQLite) 是**两条路**，不是同一件事。
7. **纯 Rust RAW 是它最大的战略资产**：1,446 个 Adobe 机型中 **551 (38%) 已在真文件上校验**，按摄影师加权 **~94% 可解 / ~85% 在校验过的机身**（`docs/raw-parity.md:15-27`）。代码量 `crates/raw/src` 31.4k 行，文档给出的工时口径是 **解码 120–220 h + 颜色/镜头 135–260 h**（`raw-parity.md:266-293`）。
8. **色彩是它最大的短板，也是最好的教学案例**：内置 profile 文档称 5 个、实测 8 个，对面是 Adobe 的 1,500 个；ΔE 中位数 5.1，被拒的拟合 18.5。它用**从相机自带 JPEG 反拟合 look（矩阵 + HueSat 表 + 曲线）** 来填这个坑（`crates/engine/src/camera_preview.rs:1-10`）。
9. **AI 全部 opt-in、权重零分发**：SAM 3 权重 3.4 GB（Meta 自有许可）、降噪权重默认没有（RawNIND 是 GPL-3.0，单独下载）、人脸是 YuNet 232 KB(MIT)。模型不进仓库、SHA-256 校验、镜像可配（`docs/ai-masks.md:85-120`、`docs/denoise.md:15-45`）。
10. **它主动放弃了 LibRaw / rawspeed / lensfun / darktable 一行代码**（`AGENTS.md:60`、`NOTICE:44-50`）。这既是它能宣称 MIT/Apache 的前提，也解释了为什么它解码工时那么贵。**这一条与我们当前静态链接 LibRaw 0.21.0 直接冲突**，第 10 节给选项。

---

## 1. 项目定位与规模

### 1.1 定位

一个 **clean-room、纯 Rust、跨平台（macOS/Windows/Linux + WASM）** 的照片库 + 非破坏性 RAW 显影器，显式对标 Lightroom Classic 15.5 / Lightroom 9.6。与 `pdfcraft`(Acrobat)、`photocraft`(Photoshop) 是同一作者（ArtCraft Team）的兄弟项目，共享 `craftrules` 工程规范（`AGENTS.md:3`）。

### 1.2 实测规模（HEAD `23f5480`，2026-10-24 统计）

| 指标 | 实测 | 证据 |
|---|---:|---|
| 版本 | 0.13.0 | `Cargo.toml:6` |
| `.rs` 文件 | 736 | `find . -name "*.rs"` |
| 总行数 | 321,978 | 同上 |
| crate（库） | 29 | `ls crates/` |
| app / 工具 | 3 + xtask | `ls apps/`、`Cargo.toml:[workspace]` |
| 引擎命令 `cmd!` | 371 个 id | `crates/engine/src/cmd/**/*.rs` |
| UI 专有命令 | 210 条 `UI_COMMANDS` | `crates/ui-egui/src/menus.rs:71+` |
| develop 数值滑杆 | 126 条 `CONTROLS` + 10 条点颜色索引模板 | `crates/develop/src/controls.rs:141-296` |
| `#[test]` 函数 | 4,559 | 全仓 |
| `crate` 根禁 panic lint | 31 个文件 | `grep deny(clippy::unwrap_used` |
| `#![forbid(unsafe_code)]` | 32 个文件 | 全仓 |
| UI / GPU | egui 0.36 / eframe 0.36 / wgpu 30 | `Cargo.toml:88-97` |
| 推理 | candle 0.9.2（pin 死） | `Cargo.toml:88-89` |

**行数分布（前 8）**：`engine` 92.7k → `ui-egui` 82.8k → `raw` 38.3k → `catalog` 13.5k → `pipeline` 12.6k → `codecs` 10.8k → `gpu` 7.0k → `meta` 6.6k。测试目录（`tests/`）单独 13.5k 行、227 个文件；生产代码 255.4k 行。

### 1.3 文档声明 vs 代码实测（一个必须记住的教训）

| 指标 | 文档声明 | 出处 | HEAD 实测 | 漂移 |
|---|---:|---|---:|---:|
| Rust 行数 / 库 crate | ≈202,000 / 24 | `docs/target-app-parity.md:36`、`architecture.md:7` | 321,978 / 29 | +59% / +5 |
| 引擎 / UI 命令 | 226 / 143 | `target-app-parity.md:35` | 371 / 210 | +64% / +47% |
| develop 控制 / 测试 | 122+10 / 2,561 | 同上 | 136+10 / 4,559 | +14 / +78% |
| 内置 profile / raw 行数 | 5 / 22.3k | `raw-parity.md:25`、`architecture.md:17` | 8 / 31.4k | +3 / +41% |

> **含义**：他们的"量化进度文档"体系（ROADMAP / target-app-parity / gaps）写得很扎实，但**散文里的数字仍然会腐烂**；只有被脚本验证的东西（`parity` xtask 校验 id 与路径存在性）才可信。我们做同类文档时，必须把数字放进**可被 CI 重算的产物**。另注：`layers.rs:60` 仍列着不存在的 `export` crate —— 工具表也会漂。

---

## 2. crate 依赖图

分层由 `xtask/src/layers.rs:39-80` 硬编码并纳入 CI（`cargo xtask layers`）：低层不得依赖 egui/eframe/winit/rfd，且只能依赖更低层。

```
L6  apps/lightcraft(桌面)   apps/lightcraft-cli   apps/lightcraft-web(WASM)   xtask   testkit(dev-only)
                                                                    │
L5  ui-egui(82.8k, egui) ──────────────┐        mcp(3.2k, 手写 JSON-RPC)
                                        │              │
L4  engine(92.7k) ──────────────────────┴──────────────┘   ← 会话/命令/历史/渲染导出/导入
        │            │          │           │
L3  catalog(13.5k)  gpu(7.0k)  preview(2.0k)  merge(4.6k)  segment(2.4k)  lensdb(1.1k)
        │            │          │
L2  pipeline(12.6k) ← CPU oracle，所有其他渲染器对齐它
        │
L1  raw(38.3k)  codecs(10.8k)  meta(6.6k)  develop(2.8k)  denoise(3.3k)  denoise-core(2.3k)
    faces(6.3k)  scenes(1.0k)  heif(2.1k)  inpaint(0.8k)  geocode(0.4k)  slideshow(1.0k)  color? 
        │
L0  geom(1.8k)  color(0.9k)  raster(1.4k)  tiff(2.4k)  fetch(1.6k)  sysmem(0.08k,唯一 unsafe)
```

| crate | 一句话职责 | 关键证据 |
|---|---|---|
| `geom` / `color` / `raster` | 矩阵与单应；色彩空间与传递函数、OkLab、CCT；`Rgb32f`/`Plane`/`Rgba8`、Lanczos-3/Mitchell、box-Gaussian、形态学、直方图 | `raster/src/lib.rs:1-10` |
| `tiff` / `fetch` / `sysmem` | TIFF/IFD 读写；固定版本 HTTPS 模型下载（断点/校验）；唯一 FFI `malloc_zone_pressure_relief` | `sysmem/src/lib.rs:1-20` |
| `raw` | 纯 Rust RAW 解码 + demosaic + DNG 色彩模型 + opcode | `raw/src/lib.rs:1-24` |
| `codecs` / `meta` | JPEG/PNG/TIFF/WebP/AVIF/JXL；EXIF/XMP/IPTC | L1 |
| `develop` | 非破坏编辑模型 + 控制符表 + 预设 + 分段掩膜 | `develop/src/controls.rs:1-6` |
| `denoise-core` / `denoise` | 模型清单与缓存格式（无推理依赖）；纯 Rust ONNX 子集执行器 + WGSL 执行器 + 远端 tile 协议 | `docs/denoise.md:70-96` |
| `faces` | YuNet 检测 + SFace/AuraFace 识别 + 对齐 | `faces/src/known.rs:1-20` |
| `heif` / `inpaint` / `scenes` / `slideshow` / `geocode` / `lensdb` | HEIC；PatchMatch 补全（移植 PhotoCraft）；程序化样张；幻灯片；地理编码；自有镜头库骨架 | `NOTICE:32-37`、`layers.rs:53` |
| `pipeline` | 显影管线 **CPU 参考实现（oracle）** | `pipeline/src/lib.rs:1-22` |
| `catalog` | 照片库模型 + op 日志持久化 + 查询/规则/关键词/堆栈 | `catalog/src/lib.rs:1-14` |
| `gpu` | 同一管线的 wgpu/WGSL 实现 + 回退 | `gpu/src/lib.rs:1-18` |
| `preview` | 内容哈希、LRU、磁盘缩略图缓存、作业池 | `preview/src/lib.rs:1-10` |
| `merge` / `segment` | HDR / 全景 / HDR 全景；SAM 3 纯 Rust 推理（candle） | `docs/merge.md:3-4`、`segment/src/lib.rs:1-27` |
| `engine` | 会话、命令注册表、历史、渲染/导出作业、导入、LR 目录导入、人脸索引、视图模型 | `architecture.md:20` |
| `ui-egui` / `mcp` | Lightroom 风格 egui 界面（可替换）；MCP 服务器（stdio，手写 JSON-RPC） | `mcp/src/lib.rs:1-15` |

> **注意一处工具漂移**：`layers.rs:60` 仍列着 `("export", Class::Layer(3))`，但 `crates/export` 不存在，导出在 `engine::export`。规则引擎只校验存在的包，所以这条死条目一直没被发现。

### 2.1 从依赖图能读出的三个事实

1. **`pipeline` 在 L2、`gpu`/`catalog` 在 L3**：GPU 只能依赖 pipeline，反之不行。架构上保证"GPU 是第二实现，CPU 永远是裁判"。
2. **`develop`（编辑模型）在 L1，比管线更低**：设置结构不依赖渲染，catalog / XMP / 预设 / CLI 都能独立读写设置。
3. **算法层之上全是工程层**：`engine`(92.7k) + `ui-egui`(82.8k) 合计 175k 行，解决的是"会话/命令/缓存/并发/导入/兼容"——排优先级的强信号。

---

## 3. 渲染管线（本报告最重要的一节）

### 3.1 五段式：真实顺序

`crates/pipeline/src/lib.rs:7-17` 的模块文档就是契约，`render_impl`（`:538-631`）是唯一实现：

| # | 阶段 | 做什么 | 空间 | 证据 |
|---:|---|---|---|---|
| 1 | **geometry** | 用户方向、镜头校正（畸变/CA/暗角）、透视、裁剪+拉直、翻转；**一次重采样**到输出分辨率；然后 defringe | 源（已 EXIF 定向）→ 输出网格；仍是相机线性/R2020 线性 | `lib.rs:8-9`、`geometry.rs:1-6` |
| 2 | **scene-linear** | 局部色调映射 → 白平衡 → defringe → 污点修复 → 去红眼 → 降噪 → 预计算空间平面（对数亮度/基底层/清晰度带/纹理带/暗通道/色度模糊/掩膜/airlight） | 线性、场景参考（曝光前） | `lib.rs:10-11`、`:470-478`、`local.rs:551-606` |
| 3 | **tone map** | 亮度上的 filmic 曲线（对比度/白场/黑场）+ 高光去饱和 | 场景亮度 → 显示线性亮度 | `lib.rs:12`、`tone.rs:1-9`、`finish.rs:533-545` |
| 4 | **colour** | 自然饱和度、饱和度、色彩混合器、点颜色、颜色分级、黑白（OkLCh） | 显示线性（仍在输出主色域前） | `lib.rs:13`、`colorops.rs:189+` |
| 5 | **display** | 色域映射到输出空间 → 编码 → 曲线（参数+点）→ 暗角 → 颗粒 → (LUT) | 线性→编码域 | `lib.rs:14`、`finish.rs:584-665` |

### 3.2 每像素阶段的完整次序（13 步）

这是整份报告里**最不可"重新设计"**的部分：`finish_with`（`finish.rs:360-670`）在一个 `for_rows` 循环里按序作用：

| 序 | 步骤 | 域 | 备注 / 证据 |
|---:|---|---|---|
| 1 | 曝光增益 `gain` | 线性 | `raw.map(|v| v*gain)`，`finish.rs:406` |
| 2 | 掩膜项求和（17 个局部项 + 颜色叠加） | — | alpha 加权和，`:410-426` |
| 3 | 局部 Moiré / 颜色降噪（色度趋向其模糊） | 线性 | `:428-435` |
| 4 | 局部 defringe（沿边去紫/绿边） | 线性 | `:436-446` |
| 5 | 去雾（暗通道 + airlight，正负两路） | 线性 | `:448-461` |
| 6 | 局部曝光 / 色温 / 色调 | 线性 | 先乘后按亮度归一，`:463-473` |
| 7 | 局部对数亮度色调（高光/阴影/白/黑/对比/清晰度/纹理/锐化/降噪）→ 一个增益 | log2 亮度 | 全部累加进 `delta`，最后一次 `exp2`，`:475-526` |
| 8 | **校准（阴影色调 + 相机校准三色）** | **线性，映射前** | `:528-531` ← 顺序关键 |
| 9 | 亮度色调映射 + 高光去饱和 + 峰值裁切 | 场景→显示 | `:533-545` |
| 10 | 色彩工具（OkLab）+ 掩膜颜色叠加（OkLab） | 显示线性 | `:547-552` |
| 11 | 暗角（裁后、显示线性，三种样式） | 显示线性 | `:554-582` |
| 12 | 软打样 + 色域映射（向亮度去饱和） | 输出主色域 | `:584-609` |
| 13 | sRGB 编码（LUT）→ 曲线（参数+点，可 Refine Saturation）→ 颗粒 → 3D LUT | 编码域 | `:611-665` |

> **三处容易被做错的地方**：(a) 校准必须在色调映射**之前**（`:528` 在 `:533` 前）；(b) 暗角必须在**裁剪后坐标**（用 `fp.view` 还原全帧，`:394-396`、`:555-561`）；(c) 曲线必须在**编码之后**，否则黑白场行为不对。

### 3.3 空间平面：半径全是"长边比例"

`plane_sigmas`（`local.rs:516-545`）把每个会让像素读邻居的调整，换成一个与分辨率无关的半径：

| 平面 | 半径 | 滤波器 | 触发条件 | 证据 |
|---|---|---|---|---|
| `base`（高光/阴影基底） | 1.5% 长边，Draft 上限 24 px | 快速导向滤波，ε=0.35 EV² | 高光/阴影（全局或掩膜）非 0 | `:528-531` |
| `clarity` | 1.2% 长边 | 快速导向滤波，ε=0.8 | 清晰度非 0 | `:532` |
| `texture` | 0.18% 长边（≥0.6 px） | Gaussian | 纹理/锐化/局部纹理/锐度/局部降噪/defringe | `:534-540` |
| `dark`（去雾） | 2% 长边 | Gaussian + airlight 百分位 | 去雾非 0 | `:541` |
| `chroma` | 0.4% 长边（`CHROMA_SIGMA`） | Gaussian | 局部 Moiré / 局部颜色降噪 | `:496`、`:542` |

`prepare`（`local.rs:551-606`）用 `par_join` 把 base / clarity / texture / dark 四路并行算，并把每个结果连同**它的键**（半径 f32 位 + 处理版本 + 中值半径）存进 `Planes`。V2 处理版本把 base 改成"对 log 亮度先做可分离中值（0.15% 长边，≤24px）再做导向滤波"，让细枝/线条保留对比（`docs/process-versions.md:13`）——这是他们唯一一次"改看法"。

### 3.4 缓存策略：三把键 + 容量 2

| 键 | 覆盖什么 | 失效即重算 | 证据 |
|---|---|---|---|
| `geo` | 几何帧 + 输出尺寸（`hash(format!("{frame:?}"), w, h)`） | 重采样 | `lib.rs:438` |
| `lin_key` | `geo` + 白平衡 + 污点 + 红眼 + 光学 + 局部色调 + 降噪参数 + `detail_scale` | 白平衡/修复/降噪块 | `:442-457` |
| `pre_nr_key` | `lin_key` 去掉降噪 | 仅"草稿复用宽平面"用 | `:376-378`、`:588` |

`StageCache` 容量固定 **2**（同时保留一个草稿尺寸和一个全尺寸渲染，`lib.rs:258`），每个平面还按**自己的半径**单独打键（`Plane` 内 `Option<(u32, Arc<Plane>)>`）。所以拖色调/色彩/曝光 → 只跑每像素阶段 + 读回；拖清晰度 → 跳过重采样与降噪（`lib.rs:241-248`）。实测：曝光拖动 **3.9 ms**（1920×1280，GPU 热）、**22–42 ms**（CPU 热）（`docs/gpu-pipeline.md:271`）。

### 3.5 GPU 调度：把它当"随时会炸的加速器"

| 机制 | 做法 | 为什么 | 证据 |
|---|---|---|---|
| 缓冲而非纹理 | 全部 `array<f32>`，与 CPU 交错布局一致 | 上传/读回即 memcpy，无格式转换 | `gpu-pipeline.md:154-155` |
| 每阶段提交 + 错误作用域 | 每阶段 submit、每 16M 调用强制 flush、每像素阶段分 ~4MP 带；OOM/validation/internal 三层 scope + device-lost 回调 | 避免 iGPU 看门狗（TDR ~2s）；错误必须"败掉这一帧" | `:186-194` |
| 读回保底 + 限额 | 60s 超时即失败；建缓冲即比对 `min(max_storage_binding, max_buffer)`（24MP RGB f32 = 275 MiB） | 旧代码会永远等；超限**这一帧**回落而非炸设备 | `:195-204` |
| 结果自检 | 输出先清零、每像素写 alpha=255；缺 alpha→**永久停 GPU**；全黑(n≥4096)→本帧重渲 CPU | 把"静默错误"变成"可判定" | `:205-209` |
| 崩溃哨兵 + 后端顺序 | 建设备前写 `gpu-init.marker`，成功即删，下次启动仍在则关 GPU；Windows 默认只用 DX12，探测 DX12→Vulkan→GL→glow | 原生崩溃无法 catch；Intel iGPU Vulkan 驱动会不可捕获崩溃 | `:83-109` |
| 退出 | 设备永不 drop；`begin_shutdown`→停池→`wait_idle`，2s 兜底 | NVIDIA Linux 退出期 SIGSEGV | `:72-81` |

**等价性判据（可直接搬）**：8-bit sRGB 上 **mean|Δ| < 0.5 LSB 且 max|Δ| ≤ 3 LSB**（`gpu-pipeline.md:239-241`）；实测 Metal M4 Pro max 1 LSB、mean ≤0.0003，135 组用例在 AMD RADV 上也通过（`:241-257`）。

### 3.6 精度落点（纠正一个常见误判）

| 位置 | 精度 | 证据 |
|---|---|---|
| RAW 传感器 | u16 | `RawImage::normalized` 白电平归一 |
| 解码后相机 RGB | **f32**（`Rgb32f`） | `raster/src/lib.rs:58` |
| 管线全程（含平面） | **f32**（`Plane = Image<f32>`） | 同上 `:60` |
| GPU 缓冲 | **f32**（`array<f32>`） | `gpu-pipeline.md:154` |
| 降噪产物缓存 `.lcdn` | **half-float（f16）压缩** | `docs/denoise.md:51-52` |
| 输出 | u8 / u16 / f32（HDR 渲染 `OutputDepth::F32Hdr`） | `lib.rs:140-141`、`:599-605` |

> **结论**：它**没有**用半精度中间缓冲。全程 f32 + 显示域才降位，是它能在 CPU/GPU 之间做到 1 LSB 的前提；半精度只用于落盘的降噪缓存。我们若为省内存引入 f16 中间态，等价性判据会直接失效。

### 3.7 与 Lightroom 管线逐条对照

LR 侧依据公开资料（DNG 规范、Adobe Camera Raw 团队公开说明、社区共识），**未读任何 Adobe 二进制**；标 〔推断〕处为合理推测。

| LR（Process 2012）步骤 | lightcraft 的做法 | 判定 |
|---|---|---|
| demosaic → 相机 RGB 线性 | AHD/PPG/bilinear，自研 X-Trans | **对齐**（质量未对 LR 测过，`raw-parity.md:197`） |
| 白平衡（相机空间、按白点重算矩阵） | `local::wb_matrix_for` 在选定白点重算相机矩阵（DNG ch.6） | **对齐**（`:59-61`） |
| 相机→工作空间（ProPhoto 线性） | 相机→**线性 Rec.2020 D65** | **等价偏离**（主色域不同，数学等价类） |
| 相机 profile / look（DCP） | 文件矩阵 + 8 内置池 + 52 光谱矩阵 + 逐文件 JPEG 拟合；**外部 .dcp 永不加载** | **近似**（clean-room 强制） |
| 镜头校正 → 变换(Upright) → 裁剪/拉直 | `Warp`：畸变/CA/暗角/透视 一次重采样 + 裁剪/拉直/翻转 | **对齐**（顺序一致；无 LCP 库） |
| 高光恢复 / 去雾 | 中性裁切 + 色度扩散（`highlight.rs`）；暗通道 + airlight（`:448-461`） | **近似 / 对齐思路** |
| 曝光 | 作为增益**推迟到每像素阶段**；空间阶段跑在未曝光图上 | **等价优化**（log 亮度滤波平移不变，`lib.rs:19-22`） |
| 对比度/高光/阴影/白/黑、清晰度、纹理 | `base`(导向滤波基底) + `texture` 带 → 累加 delta → 一次增益，中间调加权 | **近似**（LR 局部算子未公开） |
| 色调曲线（参数 + 点） | **在编码之后**作用于各通道 | **对齐** |
| HSL / 混合器 / 点颜色 / 分级 / 黑白 | OkLab / OkLCh 上做 | **近似**（LR 用自有色相模型） |
| 锐化 / 亮度降噪 / 颜色降噪 | 对数亮度带 + 引导滤波 | **近似**（无边缘感知的多尺度细节层） |
| 暗角（裁后）/ 颗粒 | 裁后坐标 + 三种样式；噪声 + 亮度调制 | **对齐 / 近似** |
| 输出色彩空间转换 + 编码 | 色域映射（向亮度去饱和）+ LUT 编码 + ICC 显示 profile | **对齐** |
| AI 降噪 / Raw Details / 超分 | 独立 tile 模型（opt-in）；Raw Details 与超分**无** | **偏离 / 缺失** |

> **最重要的一点**：两者**顺序大体一致**，差异集中在"局部算子怎么写"和"相机色彩从哪来"。这说明**管线骨架是可以照着搭的**，而**调参曲线不能照抄**——它们全是逐条调出来的（`gaps.md:20` 自认"tuned by eye"）。

### 3.8 与 LUMINA 现状的对比（必须正视）

LUMINA 目前有**三套渲染实现**，各自独立、靠人工"口径对齐"：

| 实现 | 位置 | 精度 / 空间 | 覆盖的调整 |
|---|---|---|---|
| WebGL 预览 | `app/src/modules/develop/webgl/shaders.ts`（`precision highp float`，`:24`） | f32 着色器；`pow(2.2)` 近似 sRGB↔线性（`:64-65`） | `applyParams` → HSL 8 色带 → 掩膜 → 曲线 LUT |
| Canvas CPU 回退 | `app/src/modules/develop/webgl/cpu-render.ts` | JS 浮点，逐像素；>1600px 先降采样（`:215`） | 同上（注释自述"口径同引擎 applyHslApprox"） |
| 引擎导出 | `engine/src/export/exporter.cpp:700-744` + `imaging.cpp:121-250` | **8-bit `uint8_t`**（`imaging.h:12-16`），`applyToneApprox` / `applyHslApprox` | 曝光/对比/饱和度 + 8 色带 HSL 近似 |

| 维度 | lightcraft | LUMINA |
|---|---|---|
| 像素域 | 场景线性 f32 Rec.2020 | 预览：`pow(2.2)` 线性化；导出：**8-bit sRGB 直通** |
| 调整落点 | 解码后、缩放到输出前 | **先 resize，再调参**（`exporter.cpp:700-720`） |
| 实现数量 | 2（CPU oracle + GPU），共用 `Plan`/`FinishParams`，有等价性测试 | **3**，无共享参数结构，无等价性测试 |
| 色彩模型 | OkLab/OkLCh、双照度 DNG 色彩 | RGB↔HSL 圆环（`imaging.cpp:149-215`） |
| 缓存 | 三键阶段缓存 + 平面级键 | 无阶段缓存 |
| 解码 | 自研 31.4k 行 | LibRaw 0.21.0 静态链接 |

> **判断**：问题不是"顺序抄不抄"，而是**三套实现已经会给出三种结果**——预览是线性化的、导出是 8-bit 直通的、CPU 回退又是另一份代码。lightcraft 用一整节（`gpu-pipeline.md:239-257`）证明它的两套实现在 **1 LSB** 内一致；我们连"哪套是裁判"都还没定。**这正是 A2（单一 Plan + CPU oracle）要解决的问题，也是本报告最该被采纳的一条。**

---

## 4. Catalog 设计

### 4.1 它不是数据库，是操作日志

| 项 | lightcraft | Lightroom `.lrcat` |
|---|---|---|
| 存储引擎 | **自研**：`catalog.snap`（JSON 快照）+ `catalog.log`（JSON Lines） | SQLite 3 |
| 表结构 | **无表**：36 个 `Op` 变体 + 内存 `Catalog` 结构 | 数十张表（Adobe_images、AgLibraryKeyword…） |
| 写入 | 追加 op（带 seq + CRC-32，fsync 后返回） | SQL 事务 |
| 恢复 | 撕裂尾丢弃并截断；坏记录→`catalog.log.damaged-<seq>` + 重写快照 | WAL 回放 / SQLite 自带 |
| 压缩 | 后台线程写快照，完成后原子替换日志；策略 2000 条 / 16 MiB | VACUUM |
| 版本 | 9，只能升不能降；旧版拒绝新版 | schema 版本 |
| 撤销 | `apply` 返回**逆 op**，由 engine 保存 | 历史表 |

证据：`crates/catalog/src/journal.rs:1-40`（文件布局与恢复规则）、`:79`（`VERSION = 9`）、`:86-91`（快照策略）、`crates/catalog/src/lib.rs:78-120`（`Op` 枚举，36 变体）。

### 4.2 为什么这样设计（三条理由，都可迁移）

① **op 即撤销即回放即差异**：`apply` 返回逆操作（`lib.rs:1-8`），撤销、持久化、确定性重放共用一条路径，property test 保证"重放复现状态"；② **人类可读**：日志是 JSON 行，崩溃现场可直接看，在"agent 自治开发"模式下极大降低调试成本；③ **顺序 append**，无随机写放大，SSD 友好。

### 4.3 85k 图库 1.7–4.7 s 是怎么做到的

文档只给结论（`gaps.md:43`、`target-app-parity.md:55`），机制需从代码读：

| 手段 | 说明 | 证据 |
|---|---|---|
| 快照为主 | 打开只读一个 JSON 快照 + 少量尾日志 | `journal.rs:1-12` |
| 派生索引惰性 + 增量 | stack/album 反查表首次用到才 `OnceLock` 建，之后由 op 增量维护；**不落盘** | `catalog/src/index.rs:1-9`、`:77-100` |
| 查询不做 N 次扫描 | 100k 场景下"在堆栈中"/"在某相册"类智能相册从 0.6–2.7 s 降到 ~14 ms | `gaps.md:43` |
| 可测的规模基线 | 100k 照片 / 10k 堆栈 / 20k 相册的 ignored 基准 | `catalog/src/tests_scale.rs:1-92` |
| 导入只读头 | 导入只 `probe_info`，不反马赛克；仅 NEF 需前 128 行取遮罩列 | `gpu-pipeline.md:315-316` |

### 4.4 与 XMP 的边界（这块设计得比 LR 更干净）

| 规则 | 内容 | 证据 |
|---|---|---|
| 真相源 | **catalog 是真相**，XMP 是交换格式 | `xmp-interop.md:3` |
| 自有命名空间 | `lc:settings` 存**完整 `DevelopSettings` JSON**（含掩膜/污点/裁剪/处理版本），精确往返 | `:17-27` |
| 只读 `crs:` | 读 Lightroom 的 `crs:` 字段但**永不写** | `:82-84` |
| 合并而非替换 | 写 sidecar 时把对方命名空间**逐字节保留**，自己的属性放进独立 `rdf:Description` | `:80-88` |
| 所有权按 URI | 前缀叫 `lc:` 但 URI 不是它的，就仍是外来属性 | `:87-88` |
| 读后不回写 | `photo.readMetadataFromFile` 即使开了自动写也不写 | `:9` |
| 失败保底 | 无法解析的 sidecar 先备份 `.bak-<date>` 再替换；权限问题则放弃保存 | `:89-90` |

> **可抄度极高**：这一套（`lc:` 精确 + `crs:` 只读 + 外来字节保留）是互操作的正确姿势，而且**与语言无关**，C++ 侧照抄设计即可。

### 4.5 我们对 Catalog 的判断

我们已用 SQLite（`engine/src/db`）。**不建议改成 op log**：85k 级别的开库对 SQLite 也不是瓶颈，而迁移会牵动全部查询与 IPC 协议。可抄的是：**(a) 派生索引惰性构建 + 增量维护；(b) 导入只读文件头；(c) 100k 规模基准进 CI**。

---

## 5. 纯 Rust RAW 解码（对我们最有战略价值）

### 5.1 覆盖面

| 容器 | 已支持 | 缺口 |
|---|---|---|
| **CR3** | CRX `0x100`（4 平面 Bayer，无损 / C-RAW 水平 tile）、`0x200`（单 tile 14-bit C-RAW + 自适应 QP） | 垂直 tile C-RAW、partial subband、多 tile 0x200、低频自适应增益、sRAW/mRAW、HEIF/PQ、dual-pixel（`docs/cr3.md:48-66`） |
| **CR2 / NEF** | 无损 JPEG 切片、sRAW/mRAW(YCbCr)；未压缩、无损、lossy type 1/2、split 后 lossy | CRW（2000–2004）；压缩标签下的 packed 14-bit、**HE/HE★（需 JPEG XS）** |
| **ARW** | 未压缩、ARW2、LJ92(L/M/S)、packed 12-bit、**ARW6（LLVC/RDD 34）** | A200–A390 的 12-bit 压缩、A100、ARQ 像素偏移 |
| **RAF / ORF** | RAF：未压缩 / 无损 / 有损，Bayer + X-Trans，GFX 14/16-bit；ORF：未压缩 16-bit / 12-bit packed / 双场 / 12.8bpp | 无 raw IFD 的老 FinePix；**压缩 ORF（2008 年后所有 Olympus/OM）← 最大缺口** |
| **RW2 / PEF / SRW** | RW2 格式 2/4/5/6/7/8 + 16-bit；PEF 未压缩/Huffman/packed 12；SRW 未压缩/packed 12 | PEF 像素偏移；SRW 压缩 32770/32772/32773 |
| **DNG** | 未压缩 / LJ92 / 有损 JPEG / Deflate(含 float) / LinearRaw / tile+strip / **JPEG XL(DNG 1.7)** / ProRAW 语义掩膜 | GPR(VC-5) |
| 其他 | ERF、MRW、MEF、SR2、3FR/FFF(部分)、MOS(识别) | IIQ、KDC、X3F、DCR、GPR、TIFF raw |

数字：**1,446** Adobe 机型 → **551 (38%) 真文件校验**、**656 (45%) 规则上应可解**、**201 (14%) 仅预览**、**38 (3%) 不支持**；按摄影师加权 **~94% 可解 / ~85% 在校验机身**（`raw-parity.md:15-27`）。

### 5.2 代码量

| 模块 | 行数 | 说明 |
|---|---:|---|
| `crates/raw/src` 合计 | **31,432** | 含注释与内联测试 |
| `lib.rs` | 1,564 | 路由 + `RawImage` + `probe/decode` |
| `vendor/arw.rs` + `arw_lens.rs` | 3,038 | Sony 主容器 + 镜头数据解析 |
| `ljpeg.rs` / `opcodes.rs` | 2,480 | 无损 JPEG（CR2/DNG/NEF 共用）/ DNG opcode |
| `vendor/rw2.rs` / `crx.rs` / `nef*.rs` / `arw6*.rs` | 5,822 | Panasonic 8 格式 / Canon CRX / Nikon / Sony LLVC |
| `orf/pef/cr2/sr2/srw/raf/rafc/erf/mrw/mef` | ~5,000 | 其余厂商容器与压缩变体 |
| `highlight.rs` / `jxl.rs` / `demosaic/` | 1,367 + | 高光重建 / DNG 1.7 JPEG XL / AHD·PPG·X-Trans |
| `color.rs` + `profile.rs` + `spectral*.rs` | ~1,600 | DNG 色彩模型 + 52 组光谱矩阵 |

按厂商分摊，**平均每个容器 1–1.5k 行**（含边界、错误、注释），这是可估算的关键比率。

### 5.3 他们为什么拒绝现成库

| 拒绝对象 | 显式理由 | 证据 |
|---|---|---|
| LibRaw / rawspeed / rawloader / rawler | GPL/LGPL 传染，且"pure Rust, no C/C++"是硬约束 | `AGENTS.md:60-61` |
| darktable / RawTherapee / ART / dcraw 衍生 | 同上 | 同上 |
| lensfun | 同上 + 无 LCP 数据库 | 同上 |
| Adobe 的 DCP / LCP / 相机矩阵 / 图标 / 字体 | clean-room：**只允许观察 UI 行为与布局** | `AGENTS.md:60`、`:69-77` |
| 但**允许**黑盒 oracle | DNGLab / rawpy 仅取参考像素，不读源码、不链接、不随产品分发 | `cr3.md:113-118`、`raf-compression.md:46-48` |
| 也允许 Apache/CC-BY 数据 | rawtoaces-data 光谱数据 + CIE 1931 CMF（已署名） | `NOTICE:19-25` |

**四条工程原则支撑这个选择**：① 全部纯 Rust，WASM 可编；② 无 C 运行时（连 SQLite 都自写只读 B-tree 读 `.lrcat`，`lightroom-catalog-import.md:60-61`）；③ 崩溃即违约（never-crash）；④ 每个不支持变体返回**具名 `Unsupported` 字符串**，UI 直接展示原因（`cr3.md:41-46`）。

### 5.4 达到 83% 用了多少代价

git 历史只有 1 个 commit（`--depth 1`），无法统计真实工时；但文档给了**按 PR 校准的工时口径**（`raw-parity.md:266-271`）：

| 特性 | 工时 | 证据 |
|---|---:|---|
| RW2 全格式 | 6.5 h | `raw-parity.md:269` |
| 压缩 RAF | 8.6 h | 同上 |
| CR3 CRX | 10 h | 同上 |
| NEF split 后 lossy | 4.7 h | 同上 |
| ORF 12.8 bpp | 1.7 h | 同上 |
| HEIC | 7 h | 同上 |
| **剩余解码清单合计** | **120–220 h** | `:293` |
| 颜色标定 + 镜头库 + 保真套件 | 135–260 h | `:290-293` |
| **RAW 总缺口** | **255–480 h** | `:293` |

> 这些是"Opus 5.5 agent 小时"的自报口径，**不能直接换算成人日**，但比例可靠：**每新增一个厂商容器约 5–10 h；每补一个"压缩变体"约 5–15 h；每补一个机型的颜色/电平 bug 约 1–4 h**。累积到 551 个校验机型、31k 行代码，量级在**数百小时**。

验证方法论（比代码更值得学）：

| 手段 | 做法 | 证据 |
|---|---|---|
| CC0 语料 | raw.pixls.us 全部 CC0 文件，不进仓库，`cargo xtask corpus --download` | `AGENTS.md:115` |
| 指纹定桩 | 每个样本 pin 输入 SHA-256 + **传感器像素 FNV-1a 哈希** | `cr3.md:90-110` |
| 黑盒 oracle | 外部解码器只输出参考像素，不读源码不链接 | `cr3.md:113-118` |
| 变体即契约 | 每种拒绝都有精确错误串，合成测试逐条断言 | `cr3.md:41-46` |
| 与相机 JPEG 比对 | 高通相关 r 0.93–0.99，均值相关 0.97–0.995 | `cr3.md:214-222` |

### 5.5 对 LUMINA 的直接意义（三个选项与代价）

现状：`D:/LUMINA/thirdparty/libraw/` **LibRaw 0.21.0**，`engine/src/raw/raw_decoder.cpp` 261 行封装。LibRaw 许可是 **LGPL-2.1 或 CDDL-1.0 双许可**（由使用者选），与 lightcraft 的 MIT/Apache 不兼容——但**对我们自己的分发**是否合规，取决于我们怎么链接与分发，不是照抄它的红线。

| 选项 | 做法 | 前置条件 | 代价 | 风险 |
|---|---|---|---|---|
| **A. 保持 LibRaw（推荐 v1.2）** | 不动解码器，只把管线搬到线性 f32 | ① 完成 LGPL 合规动作：动态链接或提供可重链接的目标文件；② `NOTICE` 列出 LibRaw 与版本；③ 明确不宣称"自研解码" | **0 解码工时**；合规整理 1–3 人日 | 无法 WASM；LibRaw 崩溃会带崩进程，必须进程隔离或加固 |
| **B. 混合：自研高频 + LibRaw 兜底** | 自研 CR3/ARW/NEF/RW2/RAF（覆盖加权 ~90%），其余回落 LibRaw | ① 建 CC0 语料 + 指纹 + 黑盒 oracle 流程（**这是最贵的一步，不是解码本身**）；② 定义"能力矩阵"JSON（格式 × 变体 × 状态）；③ 每个不支持变体返回具名错误而非静默降级 | **每容器 1.5–2.5k 行 + 5–10 h** ；5 个容器约 **8–12k 行 / 60–120 h**；语料与验证框架另 **20–40 h** | 两套解码器结果不一致，颜色/电平回归难查；必须建立"逐机型基线图" |
| **C. 全自研** | 完全弃用 LibRaw | B 的全部 + 覆盖到 90%+ 机型 + 压缩 ORF / JPEG XS / 各家压缩变体 | 参考 lightcraft：**解码 120–220 h + 颜色/镜头 135–260 h**，且需要真机语料（人或社区） | 长期维护：每个新机型一个新 bug；颜色标定需要镜头/色卡实拍 |

**不建议替我们决定，但建议把顺序定下来**：先做 A 的合规整理 + 建立 B 的**语料与验证框架**（这步无论选哪条路都要做，且能立刻提升回归质量），再按"用户实际抱怨最多的 3 个格式"决定是否启动 B。

**若要走 B，必须提前建的四个东西**（lightcraft 的经验，缺一个就会翻车）：
1. **能力矩阵**（格式 × 变体 × verified/unverified/preview/unsupported）作为单一事实源，脚本重算，不许手写散文。
2. **指纹基线测试**：输入 SHA-256 + 传感器像素 FNV-1a，锁住自己的输出。
3. **具名 `Unsupported`**：每种拒绝一条固定文案，UI 原样展示。
4. **黑盒 oracle 纪律**：只用外部解码器取像素，不读源码、不链接、不随产品分发；流程写进 CONTRIBUTING。

---

## 6. 色彩科学

### 6.1 五个（实际八个）profile vs 一千五百个

| 项 | Adobe | lightcraft | 证据 |
|---|---|---|---|
| 默认 profile | 每机型 Adobe Standard（bundle 内 ~1,500 个名字） | 文件自带矩阵 → 否则 **8 个内置池**（ILCA-99M2/ILCE-7CR/ILCE-7M4/NIKON_D5300/NIKON_Z_6_2/NIKON_Z_8/X-H2S/X-T4）→ 否则**逐文件拟合相机 JPEG** → 否则 **52 组光谱矩阵**（含同传感器别名）→ 否则中性矩阵 | `raw-parity.md:217`、`assets/camera-profiles/` |
| Camera Matching | 481 机型 | 无（用逐文件拟合模仿相机自身 look） | `:218` |
| 创意 profile | Adobe Color/Portrait/… | 自有 Color/Neutral/Vivid/Landscape/Portrait/Monochrome | `:219` |
| 镜头 profile | 3,635 个 | **0**（只用文件内嵌校正） | `:245` |

### 6.2 ΔE ~5 意味着什么

| 测量 | 数值 | 含义 | 证据 |
|---|---|---|---|
| Sony 批量（87 机身）拟合后 | 中位 **5.1**，ΔL* +0.2 | 与**相机 JPEG** 的差；肉眼可辨但"像同一张照片" | `raw-parity.md:226` |
| 拟合被拒 → 中性矩阵 | **18.5**，ΔL* −11，chroma ×0.39 | 明显偏色偏暗，用户会立刻发现 | 同上 |
| RW2 174 文件 | 中位 5.0（140 接受） | 同上 | `:227` |
| RAF 池化 profile | 1.62 / 2.34（X-H2S/X-T4） | 有真标定时可达专业级 | `:228` |
| 对 LR 本人（少数） | E-1 ΔE00 **11.25 → 8.30**；Sony DRO ΔE76 **7.5 → 3.3** | 与 Adobe 的差距仍以"几到十几"计 | `:229-230` |
| ProRAW 开 gain map | ΔE00 **19**（所以默认关） | 局部色调映射算错会灾难性偏色 | `:230` |

> **含义**：ΔE≈5 是"业余可用、专业不可用"的分界。而 ΔE 18 的那 10% 机型，等于换门失败。Lightroom 的护城河不在管线，在**这 1,500 个 profile + 3,635 个镜头 profile 的测量数据**。**我们短期内不可能追上，也不该假装追。**

### 6.3 他们的替代路线（唯一可行的低成本方案）

`camera_preview.rs` 对没有相机矩阵的 raw（ARW/NEF/RW2/RAF/CR2/CR3/PEF/SRW/ORF）**用文件自带 JPEG 反拟合**：在 96 px 代理上局部配准（rank transform + NCC + 邻域一致性）→ 拟合全局 3×3 矩阵 → 残差用 **HueSat 表**（等价 DNG `ProfileHueSatMap`）修正 → 色调与色度曲线独立拟合（`CameraTone` 32 节点 + 8 节点色度曲线），且**放在 finish 阶段**而非烤进传感器像素，以保留曝光与高光余量（`tone.rs:24-30`）→ **接受闸门**：留出像素色差超阈值则整体拒绝、退回中性矩阵并上报 → `LOOK_VERSION`（当前 12）在结果变化时必须递增（`camera_preview.rs:24-46`）。

> 这是"没有标定数据时最诚实也最有效"的做法：**承认不准，量化不准，拒绝明显不准的结果**。

### 6.4 `tint-direction.md` 这类细节有多重要

`docs/tint-direction.md` 全文只有 27 行，但信息密度极高：Tint 为负加绿、为正加品红；他们曾把符号搞反，导致**已保存的非零 Tint 全部反向渲染**，Lightroom 导入的 `crs:Tint` 也一直是反的。

| 教训 | 内容 |
|---|---|
| 符号约定必须写进文档 + 回归测试 | `tint_negative_is_green_and_positive_is_magenta` 渲染 18% 中性块断言方向 |
| 必须同时覆盖三条路径 | 显示参考源、相对白平衡 RAW、带 As Shot 参考的标定 RAW |
| 反演必须同向 | `temp_tint_to_xy` 描述**被移除的光源**，所以 magenta 需要更绿的光源（Duv=+tint） |
| 改符号 = 破坏既有编辑 | 他们选择"不重写已存值，只改渲染" + 提升渲染缓存版本 |

> **重要性判断**：这类"猜 Adobe 实现"的符号/方向/范围细节，**单个看起来是琐事，累积起来就是"颜色不对"的全部来源**。lightcraft 的 92 个 open core-path issue 里最大一类正是 raw 颜色（`gaps.md:18`）。我们做同类功能时，**每条方向性约定都要有合成回归测试**，成本几行，收益是避免一次全局返工。

---

## 7. AI 能力工程化

| 能力 | 模型 | 体量 | 后端 | 许可 | 入口 |
|---|---|---|---|---|---|
| AI 降噪 | RawNIND UtNet2（Bayer） | 512-cell tile / 64 重叠 | 纯 Rust CPU（faer GEMM）+ WGSL GPU；可远端 | 权重 **GPL-3.0**，单独下载 | `denoise.toggle` / `enhance.denoise` |
| 对象/描述掩膜 | **SAM 3**（ViT-32 层，1008² 输入） | **3.4 GB** | macOS Metal / 其他 CPU（candle 0.9.2）；可远端 worker | Meta SAM License（非 OSI） | `mask.*` Object/Describe |
| 人脸检测 | YuNet 2023mar | **232,589 B** | CPU/GPU | **MIT**（OpenCV Zoo） | 人脸索引 |
| 人脸识别 | SFace / AuraFace | 未在文档给体积 | 同上 | 需查清单 | `face.*` |

### 7.1 降噪（最完整的工程样例）

| 环节 | 做法 | 数值 | 证据 |
|---|---|---|---|
| 契约 | `bayerToRgb`：归一化、减黑、未白平衡 RGGB `[1,4,T,T]` → 相机 RGB `[1,3,2T,2T]` | tile=512，重叠=64，名义增益 1e6 | `denoise.md:42-45` |
| 执行器 | 自写**有界 ONNX 子集**读取器 + `faer-core 0.17.1` 纯 Rust GEMM（无 BLAS / 无 C 依赖 / 无内建线程池） | CPU 单 tile 1148 ms vs 旧 Tract 1463 ms；整图 5.185 s vs 9.534 s | `:70-82`、`:106-110` |
| GPU | 自写 WGSL 卷积/池化；与 CPU 比对后决定；错误回落 | 单 tile 15.1 ms；整图 0.582 s | 同上 |
| 缓存 | `<library>/denoise/<key>.lcdn`，按源内容+模型+算法版本键控，**压缩半精度**，默认上限 **20 GB**，最旧淘汰 | 0 时走原路径，不重复尝试失败文件 | `:47-63` |
| 公平性 | 队列优先当前/选中；输入时暂停；worker 数自适应 | 不阻塞 UI | `:60-61` |
| 导出 | 有效 Amount>0 时**同步产出**，拿不到就报错，**绝不静默导出未降噪图** | — | `:65-68` |
| 远端 | `denoise-serve`：一行 JSON + half-float 洗牌 + deflate 的 payload；先测速再决定是否外发 | M3 本机 116 ms/tile，RTX 3060Ti 经 Tailscale 345 ms → 不外发 | `:124-167` |

### 7.2 SAM 3

- **编码一次、点击多次**：1008² 的 32 层 ViT 每张照片跑一次（M4 Pro ~3 s，含一次性 ~6 s 内核编译+加载）；之后每次点击 ~35 ms，每次文本提示 ~0.6 s（`ai-masks.md:35-42`）。
- **掩膜不依赖模型**：结果存 288×288 logits（量化压缩 10–30 KB）进掩膜对象，渲染/导出任意分辨率采样，换机也能用，WASM 也能渲染（`:44-47`）。
- **精修补丁**：最后一次点击后 ~1 s 起后台对局部放大重算（~5 s），每选区最多 4 个高清补丁，小物体边缘细 5–10 倍（`:30-33`）。
- **内存纪律**：闲置 10 分钟卸载（模型占数 GB）（`:40-42`）。
- **分发**：权重不进仓库；下载前必须 `acknowledged: true`；镜像可配（环境变量 / 配置文件 / 内置列表，当前内置列表为空）；逐文件 `.part` + 尺寸/SHA-256 校验（`:85-120`）。

### 7.3 三条可迁移的原则

1. **权重永不进仓库、永不随发行包分发**：全部 opt-in + SHA-256 + 许可确认 + 断点续传。
2. **模型不可用时功能优雅降级**：没有 SAM3，其它一切照常，掩膜仍在；没有降噪模型，手工降噪仍在。
3. **模型输出的"小表示"入库**（288² logits），而不是每次重新推。这一条让 AI 结果变成**可编辑、可导出、可迁移的普通数据**。

---

## 8. 工程纪律

### 8.1 clean-room 规则（可直接照抄成我们的 CONTRIBUTING）

| 规则 | 内容 |
|---|---|
| 可观察 | 已安装 Adobe 应用的 **UI 布局与行为**（只读，绝不导入/编辑/评级/删除用户库） |
| 禁读 | Adobe 应用包内任何内容（名称/清单除外）；DCP/LCP/图标/预设/字体/相机矩阵 |
| 禁抄 | GPL/LGPL/AGPL 代码：darktable、RawTherapee、ART、LibRaw、rawspeed、rawloader、rawler、lensfun、dcraw 衍生 |
| 资产 | 每件资产必须在 `assets/ATTRIBUTION.md` 有记录，`cargo xtask assets` 强制；出现 `.dcp/.lcp/.lrtemplate` 直接失败 |
| 例外也禁 | 连 Adobe 开源的 Source Sans/Serif、Adobe Fonts 都不用（唯一例外：Noto Sans CJK，维护者决定并记录） |

证据：`AGENTS.md:59-97`、`:69-97`。

### 8.2 never-crash（比功能优先）

| 规则 | 落地 | 证据 |
|---|---|---|
| 非测试代码禁 panic 家族 | 每个 crate 根 `#![deny(clippy::unwrap_used, expect_used, panic, unimplemented, todo, unreachable)]` | `AGENTS.md:54-57`；31 个文件 |
| `unsafe` 只在一处 | `sysmem` 一个 FFI；其他 32 个文件 `#![forbid(unsafe_code)]` | `AGENTS.md:42-45` |
| 输入即敌意 | `get()` 代替索引、饱和运算、限幅分配、字符边界切片、递归限深 | `AGENTS.md:46-50` |
| 最后防线 | panic hook + `catch_unwind` 包住命令分发与导入导出 | `AGENTS.md:51-52` |
| 证据 | 每个崩溃修复必须附一个"修复前会 panic"的合成回归 | `AGENTS.md:53` |

### 8.3 质量门与自动度量

| 门 | 内容 | 证据 |
|---|---|---|
| `cargo xtask ci` | fmt → clippy `-D warnings` → HEIF 构建 → 测试 → parity → layers → assets → WASM | `architecture.md:61-64` |
| `layers` / `parity` | 依赖方向硬校验（含层内次序）；校验 checklist 里每个 `cmd:<id>`/`ctl:<id>`/反引号路径（可带 `:line`）**真实存在**，`--write` 刷新汇总，另查 gaps 表条目唯一 | `layers.rs:39-95`、`xtask/src/parity.rs:1-20` |
| 测试组织 | 每 crate 一个集成测试二进制 `tests/it/main.rs`，一文件一模块；状态型测试单独二进制 | `architecture.md:70-74` |
| 跳过要可见 | 缺 GPU/语料/字体/模型时 `testkit::skip!(need, why)`，CI 汇总 `target/ci/report.json` + `summary.md`（不含主机名/用户名/绝对路径） | `architecture.md:76-84` |
| 性能预算与基准 | 滑杆 ≤16 ms（草稿）/loupe ≤60 ms（~2.5MP）/24MP 导出 ≤1 s；`cargo xtask bench` 追加 `history.jsonl`，CPU 回退 >20% 报警 | `AGENTS.md:123-127` |
| 版本即契约 | catalog 格式、`LOOK_VERSION`、渲染缓存版本，改行为必须递增 | `journal.rs:52-77`、`camera_preview.rs:24-46` |

### 8.4 搬得动 / 搬不动（对 C++/Electron 项目）

| 项 | 搬？ | 理由 |
|---|---|---|
| clean-room 规则文本 / 具名 Unsupported | ✅ 直接搬 | 与语言无关；关键是"错误串即契约" |
| never-crash 清单 | ✅ 搬规则、换工具 | C++ 改"禁裸 `new`/禁上线 `assert`/禁 `[]`" + ASan/UBSan + fuzz |
| parity / 分层自动校验 | ✅ | 我们有 JSON-RPC 方法表；CMake target 依赖 + 脚本扫 `#include` |
| `testkit::skip!` + 机器报告 | ✅ 强烈建议 | 我们 GPU/模型/语料测试现在应是静默跳过，等于没测 |
| f32 全程 + CPU oracle | ✅ 最高优先 | 见第 9 节 A1/A2 |
| 每 crate 一个测试二进制 | ⚠️ 部分 | C++ 链接成本高，改"每模块一个可执行 + ctest" |
| 文档"最后更新"行 + 版本递增 | ✅ | 成本极低，防止文档腐烂 |

---

## 9. 对 LUMINA 的可借鉴清单（三档）

### A. 架构思想（建议 v1.2 就动手）

| # | 条目 | 为什么 | 我们的落点 |
|---:|---|---|---|
| A1 | **场景线性 f32 管线 + 输出前才降位** | 所有色调/色彩/局部调整都依赖线性域；8-bit 显示域无法正确做曝光/高光/色域 | 新建 `engine/src/render/`，`Image32F` 类型，导出不再走 `imaging.cpp` 的 8-bit 近似 |
| A2 | **单一 `Plan` 同时喂 CPU/GPU，并指定 CPU 为 oracle** | 我们有 3 套实现（WebGL/Canvas CPU/引擎导出）且无等价性测试；参数、缓存键、输出尺寸只算一次 | 一个 `RenderPlan` + `FinishParams`，三套执行器读同一份；加 1 LSB 等价性测试 |
| A3 | **三键阶段缓存 + 平面级半径键** | 拖滑杆只重跑每像素段，是"手感"的全部来源 | 先做 `geo`/`lin` 两级；平面级后置 |
| A4 | **调整在输出网格上做，resize 是几何阶段的一部分** | 现在 LUMINA 先 resize 再调参（`exporter.cpp:700-720`），缩放算法与调整顺序都错了 | 把 resize 移进 geometry，一次重采样 |
| A5 | **处理版本（Process Version）冻结旧观感** | 允许以后调算法而不毁既有编辑 | 设置里加 `process` 字段，默认 legacy |
| A6 | **命令即一切（id/label/menu/shortcut/params/enabled/run）** | UI/CLI/MCP/RPC 共用一条分发，测试可只测命令层 | 我们已有 RPC 方法表，补齐"菜单/快捷键/enabled"三元组即可复用 |
| A7 | **catalog 是真相、XMP 是交换、外来属性逐字节保留** | 互操作唯一正确姿势 | 改 `engine/src/xmp/`：只写自有命名空间，`crs:` 只读 |
| A8 | **具名 Unsupported + 能力矩阵作为单一事实源** | 降级要可见、可测、可统计 | 生成 `capabilities.json`，CI 校验 |
| A9 | **测试跳过要可见（skip 报告）** | 静默跳过等于没测 | 加 `ctest` skip 汇总 + 机器信息 |
| A10 | **预算式内存分配**（引擎缓存 50% / 在飞行解码 25% / GPU 池 12.5%） | 大图库下唯一可控的方式 | 先做解码与缩略图两级 |

### B. 算法思路（可抄思路，不可抄代码）

| # | 条目 | 要点 | 风险 |
|---:|---|---|---|
| B1 | 高光/阴影用**导向滤波基底 + 细节带**，而非亮度蒙版 | `base`(ε=0.35EV², 1.5% 长边) 与 `texture` 带分别控制区域对比与细节 | 参数需自调，不能照搬系数 |
| B2 | 清晰度 = 导向滤波带上的中间调加权细节增益 | `mid = exp(-(base/3.2)²)` 让清晰度只作用中间调 | 同上 |
| B3 | 去雾 = 暗通道 + airlight 百分位 | 正负双向、`t` 下限 0.12 | 天空区易过冲 |
| B4 | 色彩工具在 **OkLab/OkLCh** 上做 | 感知均匀，避免 HSL 的蓝色断层 | 需自己实现 OkLab 矩阵（公开公式） |
| B5 | 曝光**推迟**到每像素阶段（空间滤波平移不变） | 省一次全图重算 | 需保证所有空间平面都在未曝光图上算 |
| B6 | 色域映射 = 向亮度**去饱和**直到入域 | 比硬裁切自然 | 高质量需 ICC 配合 |
| B7 | HDR 融合 = 亮度序加权均值 + 近裁切权重惩罚 + deghost 一致性掩膜 | 权重含"1px 内最大通道" | 对齐与 deghost 是难点 |
| B8 | 全景 = MOPS 特征 + RANSAC 单应 + 最大生成树链 + LM 束调整 + 多频段融合 | 成熟流水线 | 工作量大，v1.2 不宜 |
| B9 | 无标定时用**相机自带 JPEG 反拟合 look** | 矩阵 + HueSat 表 + 色调曲线 + 接受闸门 | 必须实现"拒绝"，否则更差 |
| B10 | 降噪 tile 契约（4 通道 mosaic 进，3 通道 RGB 出） | 让模型与管线解耦 | 需先有线性管线 |

### C. 明确不要抄（含理由）

| # | 条目 | 为什么不要 |
|---:|---|---|
| C1 | **自研全部 RAW 解码** | 数百小时 + 持续机型维护；我们的用户量级不需要；LibRaw 已覆盖 90%+ |
| C2 | **用 op log 替代 SQLite** | 迁移成本高、收益在 85k 以上才显现；我们已有 SQLite 查询/索引 |
| C3 | **自研 SAM 3 推理** | 2.4k 行 + candle 依赖 + 3.4 GB 权重 + Meta 非 OSI 许可；我们直接调用外部模型服务更划算 |
| C4 | **自研 ONNX 执行器** | 他们是为了"纯 Rust + 无 C"的自我约束；我们用 onnxruntime 完全合理 |
| C5 | **每像素单线程 `for_rows` 的写法** | 他们靠 rayon 行并行；我们该直接用 OpenMP/TBB 或 GPU |
| C6 | **没有镜头 profile 数据库就上线** | lightcraft 自己把它列为 B 级 gap（`gaps.md:25`）；用户会立刻发现畸变没修 |
| C7 | **整份 322k 行的架构** | 55% 在 engine/UI；我们 5 人规模不能照抄这个重心分布 |
| C8 | **GPU 平移/透视 warp 用 CPU 掩膜** | 他们是精度妥协（`gpu-pipeline.md:7-22`）；我们画质要求没那么极端时可简化 |
| C9 | **他们的色调曲线数值/系数** | 全是逐条"调出来的"（`gaps.md:20`），照抄既不准也不合法（无授权依据） |

---

## 10. 许可与红线

### 10.1 lightcraft 自身的许可边界

| 部分 | 许可 | 备注 |
|---|---|---|
| 主体 | **MIT OR Apache-2.0** | `LICENSE-MIT`、`LICENSE-APACHE` |
| `crates/segment` | **仅 Apache-2.0** | 因移植 HF Transformers（Apache-2.0），不能选 MIT（`NOTICE:38-43`） |
| `crates/inpaint` | MIT OR Apache-2.0 | 移植自 PhotoCraft（同作者），逐文件标注（`NOTICE:32-37`） |
| `crates/sysmem` | MIT/Apache + 一个 unsafe FFI | 仅 macOS libSystem |
| `crates/raw/src/spectral_table.rs` | 数据来自 **rawtoaces-data (Apache-2.0)** + **CIE 1931 CMF (CC-BY-4.0)**，矩阵由他们拟合 | 已署名（`NOTICE:19-25`） |
| `docs/brand/` | **商标，非开源** | 不得重绘/改色/衍生（`AGENTS.md:78`） |
| SAM 3 权重 | Meta SAM License（非 OSI） | 不进仓库、不随包分发 |
| RawNIND 权重 | **GPL-3.0** | 单独下载，用户自担义务 |

### 10.2 他们拒绝的 GPL 来源（原文清单）

> "Never copy GPL/LGPL/AGPL code (darktable, RawTherapee, ART, **LibRaw**, rawspeed, rawloader, rawler, lensfun, dcraw-derived GPL code…)" —— `AGENTS.md:60`

`NOTICE:49-50` 再次声明："no darktable, RawSpeed, **LibRaw** or other GPL decoder/runtime code is included."

### 10.3 与我们 LibRaw 依赖的冲突（必须写清，不要含糊）

| 事实 | 说明 |
|---|---|
| 我们的现状 | `thirdparty/libraw` **0.21.0**，静态链接进 `engine` |
| LibRaw 许可 | **LGPL-2.1 或 CDDL-1.0**（双许可，使用者选一）——**不是 GPL**，lightcraft 原文把 LibRaw 归入"GPL/LGPL"一类整体拒绝 |
| 与 MIT/Apache 的关系 | 不兼容：我们**不能**把自己的二进制整体标成 MIT/Apache-only 再静态链 LGPL |
| LGPL-2.1 静态链接的合规路径 | ① 提供可重新链接的目标文件（`.o`/`.a` + 说明）；② 或改动态链接（DLL）；③ 附许可全文与修改说明；④ 不得限制用户替换该库 |
| CDDL-1.0 路径 | 文件级 copyleft，静态链接同样需要满足"可替换"精神；与 GPL 的兼容性另有争议，不建议混谈 |
| 我们能宣称什么 | 可以说"使用 LibRaw 0.21.0 解码"，**不能**说"自研 RAW 解码器"；lightcraft 的 83%/38% 数字**不是我们的成绩** |
| 我们不能抄什么 | 它的解码实现（我们是 C++，它是 Rust；且其实现是在"不看 LibRaw"的前提下写的，我们照抄会同时破坏它的 clean-room 声明与我们的合规叙事） |

**红线条目（建议写进我们的发布检查单）**：
1. 发行包必须附 `NOTICE`，列出 LibRaw 版本、许可、获取源码的地址。
2. 静态链接时必须提供可重链接材料，或改动态链接。
3. 不得把 lightcraft 的解码覆盖率/ΔE 数字引用为 LUMINA 的能力。
4. 不得把 lightcraft 的源码（含算法实现）直接翻译进我们的代码库——它是 MIT/Apache，法律上允许，但会让我们失去"独立实现"的叙事，且引入一个我们无法维护的分支。
5. 若未来要发 WASM/Web 版，LibRaw 静态链是硬阻塞——那时才真正需要 B/C 路线。

---

## 11. 结论（给决策用）

1. **能立刻做、收益最大的三件事**：把渲染搬到线性 f32（A1+A4）；引入处理版本（A5）；把"测试跳过"变成可见报告（A9）。这三项的性价比远高于任何功能开发。
2. **最该抄的一条设计**：单一 `Plan` 同时驱动全部渲染实现，CPU 定为 oracle，配三键阶段缓存与 1 LSB 等价性测试。它同时解决"三套实现给出三种结果"和"拖滑杆卡"两个问题。
3. **最该避开的一个坑**：**让多套渲染实现"靠人工对齐口径"共存**。我们已经有三套（WebGL 预览 / Canvas CPU 回退 / 引擎 8-bit 导出），彼此没有等价性测试；lightcraft 只用两套，却用 `Plan` + `FinishParams` 共享参数并以 **1 LSB** 为界证明一致。顺序本身可以后补，**"谁是裁判"必须马上定**。
4. **RAW 路线**：v1.2 保持 LibRaw，先把合规做干净（NOTICE + 可重链接/动态链接），同时建立"能力矩阵 + CC0 语料 + 指纹基线 + 黑盒 oracle"四件套。这四件套是任何自研路线的前置，也是当下就能提升回归质量的投资。
5. **不要追色彩**：1,500 个 profile 与 3,635 个镜头 profile 是数据护城河，短期不可追。可以学它的"逐文件拟合 + 接受闸门"，但要明确告诉用户"相机色彩是估计值"。
