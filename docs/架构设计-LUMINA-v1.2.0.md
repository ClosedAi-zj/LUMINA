# LUMINA v1.2.0 · 内核重构架构设计（NodeGraph / 16bit 线性 / 单一 Plan）

> 架构师：高见远（Bob）· 2026-10-24 · 基线 `13626ef`（引擎 1.1.1 / 协议 1.0 / XMP SchemaVersion 1）
> 唯一口径：`docs/v1.2.0/v1.2.0-启动-对标路线与范围冻结.md`（下称「启动文档」）+ `docs/v1.2.0/R6-UI重构设计方向.md`（含 v2 修订）+ `design/v1.2.0/mockup-v2.html`
> 本文只回答「怎么建」。范围、里程碑、人日总量（204）以启动文档为准；本文在文件级展开并给出实现决策。
> 用户已拍板不得再议：**NodeGraph 粒度 = 参数组** · **色彩科学路线 B（自有，验收=对相机 JPEG 中位 ΔE≤3）** · **LibRaw 维持静态链接 + 补齐 LGPL-2.1 §6 可重链接材料**。

---

## 0. 一页结论

v1.2.0 不新增功能，**换裁判**：把「三套互不相等的渲染实现」收敛为「**一个 NodeGraph + 一份 Plan + 两个执行后端（CPU oracle / GPU）**」，并把整条链路从 8bit sRGB 提到 **16bit 半对数 scene-linear**。

四条不可协商的架构约束：

| # | 约束 | 落地检查 |
|---|---|---|
| C1 | **XMP 是唯一真相源**，PSD 只做单向派生（禁止回读参数） | 代码评审 + `psd_writer` 无读路径 |
| C2 | **不用 PS 式像素图层树**，用 NodeGraph（节点=参数组+可选蒙版+可选混合） | 8 节点类型枚举，与 UI 右栏 8 组 1:1 |
| C3 | **每参数三格齐全**（预览生效/导出生效/像素断言），缺一格不上线 | R7；CI 断言 93/93 |
| C4 | **单次重采样**：resize 属几何阶段，只在输出尺寸确定后执行一次 | `RenderPlan` 只允许一个 `outW/outH` 出口 |

---

## 1. 现状证据（已逐行核码，非估算）

| 缺陷 | 证据 | 后果 |
|---|---|---|
| **A 预览≠导出** | `engine/src/export/exporter.cpp:719-742` 仅读 `exposure/contrast/saturation` + `hsl.*`；`app/src/modules/develop/webgl/renderer.ts:54-69` 用 12 基础参数 + 曲线 LUT + HSL + 暗角/颗粒/分离色调 + 蒙版 | 导出 **27/93**（29%），预览 **39/93**（42%）。高光/阴影/白平衡/曲线/清晰度/去雾/蒙版/裁剪/镜头**静默丢弃** → 违反 R3 |
| **B 三套实现** | WebGL（`webgl/`）、Canvas 回退（`develop/cpu-render.ts`，0 处 mask 引用）、引擎导出（`exporter.cpp`，0 处 mask 引用） | 参数结构不共享、无对拍测试；蒙版在导出与软渲染**完全不存在** |
| **C 8bit 天花板** | `engine/src/raw/raw_decoder.cpp:58-59` `output_bps=8 / output_color=1`；`imaging::Image{pixels: vector<uint8_t>}`（`imaging.h:13-17`）；`resizeImage` 为**盒式滤波** | 线性空间数学不成立、暗部条带、缩小画质劣化 |
| **D 顺序错** | `exporter.cpp` 顺序 = 解码 → **resize** → 调参 → 色彩空间 → 水印（708 行 resize 早于 728 行 applyToneApprox）；`shaders.ts:129-135` 白平衡排在对比之后 | resize 先做再调参 = 两次重采样 + 顺序反；WB 在 gamma 域平移 = 数学错误 |
| **E 数据硬伤** | `schema.sql:23-27` `photos.file_path TEXT UNIQUE`（路径=身份）；参数只在 sidecar（库内仅 `has_edits` 布尔）；`edit/history.h` 纯内存（无表无 DAO） | 移动文件夹即断身份；无法按调整筛选/原子备份；重启丢全部历史 |

---

## 2. Image 表示（决策 D1）

### 2.1 内部权威格式：`Image16` = `uint16_t` 交错 RGB + **半对数编码**

| 方案 | 24MP（6000×4000）内存 | 相对精度 | CPU 可行性（MinGW 8.1） | 结论 |
|---|---|---|---|---|
| `uint8_t[3]`（现状） | 72 MB | 1/255 全域 | — | 淘汰 |
| `uint16_t[3]` **线性定点** | **144 MB** | 暗部 0.002 处 1.3%（条带） | 好 | 淘汰（暗部不合格） |
| **`uint16_t[3]` 半对数（选定）** | **144 MB** | **≈0.05%（每档 2048 码，全域恒定）** | 好（纯整数位运算解码） | ✅ |
| `half[3]`（fp16） | 144 MB | 0.1%（10bit 尾数） | ⚠️ GCC 8.1/x86 **无 `_Float16`**，须手写/cvtph 内建（F16C 依赖） | 备选 |
| `float[3]`（f32） | 288 MB | 1.2e-7 | 好 | 仅算子内部 tile 用 |

**决策 D1**：内部权威格式 = **`Image16`（`std::vector<uint16_t>`，交错 RGB，**半对数 scene-linear**）**，编码：
```
encode(v) = clamp(round((log2(max(v, 2^-12)) + 12) * 2048), 0, 65535)   // [2^-12, 16.0) → 16bit
decode(u) = exp2(u / 2048.0 - 12.0)
```
理由：① 与 uint16 同价（144MB@24MP，比 f32 省 50%）；② **相对精度恒定**，根治线性定点在深阴影的条带；③ 避开 GCC 8.1 无 fp16 的坑；④ GPU 用 `R16UI`/`R16F` 纹理，shader 每像素 3 次 `exp2` 可忽略；⑤ 落盘/缓存/跨进程序列化同一格式，无二次转换。

### 2.2 域分界

| 域 | 语义 | 编码 | 载体 |
|---|---|---|---|
| **scene-linear**（S0–S2） | 传感器相对光量，可 >1.0（高光余量到 16.0） | 半对数 log16 | `Image16` |
| **display-referred**（S3 起） | 经 tone map 后 [0,1] | 仍是 `Image16`（保留精度，避免 8bit 中间态） | `Image16` |
| **output**（S7） | 最终写出 | 8bit（JPG/PNG/TIFF 8）或 16bit 线性（TIFF 16/PSD 16） | `Image16` → 编码器 |

**分界点唯一**：S2 末尾的 `ToneMap` 算子把 scene-linear 映射到 [0,1] 显示域（含高光滚降/黑场），**此前一切算子用线性光语义，此后一切算子用感知域语义**。GPU shader 内部用 `float` 计算，输入输出仍是 log16（`exp2`/`log2` 各一次）。

### 2.3 色彩空间标签随图像传递

```cpp
// engine/src/imaging/image_buffer.h
enum class ColorSpaceTag { SceneLinearCamera, SceneLinearWorking, EncodedSRGB, EncodedAdobeRGB, EncodedDisplayP3 };
struct ImageBuf {
  int w = 0, h = 0;
  ColorSpaceTag space = ColorSpaceTag::SceneLinearCamera;
  float blackPoint = 0.f, whitePoint = 1.f;   // 解码时的黑/白电平（供可重现性）
  int32_t iccProfileId = -1;                   // M4：icc 注册表句柄；-1 = 未绑定
  int32_t logStep = 2048;                      // 半对数档宽（格式版本化，禁止运行时改）
  std::vector<uint16_t> data;                  // w*h*3, 交错
};
```
**规则**：任何算子入口 `assert(buf.space == 该算子期望域)`；跨域转换必须经 `plan/color_convert`（唯一实现），禁止在算子内临时 `pow(2.2)`。

### 2.4 8bit → 16bit 兼容路径（旧 sidecar 语义不变）

| 项 | 规则 |
|---|---|
| 参数语义 | **完全不变**（`exposure` 仍是 EV，`contrast` 仍是 -100..100）→ 旧 sidecar 直接可读，`lumina:SchemaVersion=1` 无需改参数值 |
| 像素结果 | **会变**（这是修复，不是回归）→ 发布说明明确「既有编辑的导出结果将更接近预览」，并附 `docs/v1.2.0/视觉差异报告`（M2 出口产物） |
| 8bit 边界 | 只在 S7 输出：`encodeOutput(buf, bps, space, dither=true)`；8bit 输出加 **TPDF 抖动 ±0.5LSB**，消除 16→8 量化条带 |
| 旧缓存 | 旧 `{home}/cache/{thumbs,previews}` 缓存按 `cacheVersion` 目录隔离（`thumbs/v2/`），旧目录可直接删（启动时提示占用体积，不自动删） |
| 兼容开关 | `settings.render.legacy8bit=false`（默认新管线）；置 true 仅供 A/B 比对，不进发布路径 |

---

## 3. NodeGraph 数据结构（决策 D2–D4）

### 3.1 8 类节点（与 R6 右栏 8 组严格 1:1，不得增减改名）

| # | 节点类型 | 覆盖参数组 | 求值阶段（见 §6） | 蒙版 | 混合 |
|---|---|---|---|---|---|
| 1 | `basic` | 白平衡(2) + 基础(12) = **14 叶** | S0(仅 WB 增益) / S2 | ✅ | ✅ |
| 2 | `curve` | 复合曲线 + 分通道(3) + 参数曲线(7) = **11 叶** | S4 | ✅ | ✅ |
| 3 | `color` | HSL 8 带(24) + 颜色分级(8) = **32 叶** | S4 | ✅ | ✅ |
| 4 | `detail` | 锐化(4) + 降噪(3) = **7 叶** | NR→S1.5 / 锐化→S5 | ✅ | ✅ |
| 5 | `lens` | 配置文件(1) + 畸变(1) = **2 叶**（色差参数预留，不计 93） | S1 | ❌ | ❌ |
| 6 | `geometry` | 裁剪(5) + 拉直(1) + 透视(7) = **13 叶** | S1 | ❌ | ❌ |
| 7 | `effects` | 裁后暗角(3) + 颗粒(3) = **6 叶** | S6 | ✅ | ✅ |
| 8 | `calibration` | 阴影/红/绿/蓝原色(7) = **7 叶** | S2（tone map 之前） | ✅ | ✅ |
| — | （蒙版） | `masks[]` 挂在任意可蒙版节点上 = **1 叶** | 随宿主节点 | — | — |
| | **合计** | **14+11+32+7+2+13+6+7+1 = 93** | | | |

> 决策 D2：**面板与节点 1:1，但「分离色调」不单独成面板**——`colorGrade` 的 8 个叶（`SplitToningShadow*` / `ColorGradeMidtone*` / `SplitToningHighlight*` / `ColorGradeBlend/Balance`）全部归 `color` 节点渲染；R6 中「效果·分离色调」由 `color` 节点承担，`effects` 面板只渲染暗角+颗粒 6 项。理由：**同一参数只能有一个控件**，否则双控件 = 双真相源。

### 3.2 边语义：有序线性链 + 阶段约束（不是自由 DAG）

- 图 = `nodes[]` 有序数组；**主链单输入单输出**（LR 语义：管线顺序是产品本身）。
- 每个节点有 `stage`（由 type 推导，见 §6），**渲染顺序 = (stage, order) 稳定排序**；同 stage 内按 `order` 升序。
- 蒙版节点 = 宿主节点 `mask` 字段非空：求值该节点到独立缓冲，再以 `blend/opacity` 合成回主链（见 §3.3）。
- **DAG 预留但不启用**：`inputs` 字段保留（v1.2 恒为 `[{nodeId:"<prev>"}]`），v1.3 若做「一个曲线同时喂两个分支」再放开；本期禁止分叉（UI 无节点编辑器，C2）。
- **决策 D3**：同一 type 可多实例（例：`basic` 全局 1 个 + `basic` 带蒙版 2 个）。这是「曲线只作用于某蒙版」的表达方式——该蒙版节点只带 `curve` 参数，宿主 type=`curve`。

### 3.3 蒙版与混合的挂载

```jsonc
{ "id": "n3", "type": "basic", "enabled": true, "opacity": 100,
  "blendMode": "normal",                     // normal|multiply|screen|overlay|softLight
  "mask": { "maskId": "m1", "invert": false, "feather": 0 },
  "params": { "exposure": 0.35, "highlights": -18 } }
```
求值伪码（CPU/GPU 共用同一 Plan 语义）：
```
base = prev(node[i-1])
if (node.mask == null)  next = Op[node.type](base, node.params)
else { layer = Op[node.type](base, node.params);
       a = MaskField(node.mask);            // 0..1，S1 之后坐标（裁后归一化）
       next = mix(base, Blend(layer, base, node.blendMode), a * node.opacity/100) }
```
**蒙版坐标约定**：一律 **裁后归一化坐标**（`uv ∈ [0,1]` 基于 `Plan.outW/outH`），与 LR 一致——这样裁剪改变时蒙版不漂移（现存 bug：蒙版用原图坐标，裁剪后错位）。

### 3.4 `lumina:NodeGraph` JSON Schema（写进 sidecar）

```jsonc
"lumina:NodeGraph": {
  "schemaVersion": 2,
  "nodes": [
    { "id": "n1", "type": "basic", "order": 0, "enabled": true,
      "opacity": 100, "blendMode": "normal", "mask": null,
      "params": { "whiteBalance":"custom", "temperature":5200, "tint":4,
                  "exposure":0.15, "contrast":12, "...": "…" } },
    { "id": "n2", "type": "curve", "order": 0, "enabled": true,
      "params": { "toneCurveRGB": [{"x":0,"y":0},{"x":255,"y":250}],
                  "toneCurveR": null, "toneCurveG": null, "toneCurveB": null,
                  "parametric": { "highlights":0,"lights":0,"darks":0,"shadows":0,
                                  "shadowSplit":25,"lightSplit":50,"midSplit":75 } } }
  ],
  "masks": [ { "id":"m1", "type":"radial", "enabled":true, "opacity":100,
               "geometry": {"cx":0.5,"cy":0.3,"radius":0.35,"ratio":1.6},
               "range": null, "strokes": null } ]
}
```
- 参数值单位与 `EditParams` **完全一致**（见 §4 表），因此投影到 `crs:` 是**同值直写**，无换算。
- 数组顺序即语义（`order` 仅用于同 stage 稳定排序，缺省按数组下标）。

### 3.5 SchemaVersion 1 → 2 迁移

| 步骤 | 规则 |
|---|---|
| 触发 | 启动后台迁移线程（v1.1.1 已落地，不阻塞握手），逐 sidecar 扫描 |
| v1 有 `crs:` 无 `lumina:NodeGraph` | **构造默认图**：按 §4 表把 `crs:` 值填进 8 个全局节点（缺失字段取默认值），`masks` 从 `lumina:Masks` 提升为图级 `masks[]` 并回挂到（新建的）对应蒙版节点 |
| v1 有 `lumina:AdjustmentStack` | **映射**：每个 stack 层 → 一个带蒙版的节点（`params` 取其 `params`，`opacity/blendMode` 直承）。**映射后废弃该字段**（写空数组 + `lumina:AdjustmentStackDeprecated=true`），避免双真相源 |
| 写回 | 只增 `lumina:NodeGraph` + `SchemaVersion=2`；**`crs:` 全量保留并按图投影重写同值**（字节保留式写，未涉及字段逐字节不变） |
| 幂等 | 二次运行：图已存在则跳过；补投影=同值写入 → 字节稳定（round-trip 用例扩展为 v1→v2→v1 可逆） |
| 版本更高 | `SchemaVersion>2` → `1006 schemaTooNew` + 提示「部分参数可能不显示」，不猜测丢弃 |
| 并发 | 迁移写 sidecar 复用 `XmpService` 的 per-photo 写队列 + 全局写锁（v1.1.1 同机制），与编辑会话/AI 参数写互斥 |

---

## 4. 93 参数叶完整映射表（R7 地基，逐叶列全）

> 列义：**#** 序号 · **叶** JSON 路径（`EditParams`/节点 `params`）· **节点** · **crs: 字段** · **阶段** · **现预/现导** = 当前 v1.1.1 是否生效（●生效 ○静默丢弃）。目标：M1 出口全部 ●/●。

### 4.1 `basic` 节点（14 叶）

| # | 叶 | crs: 字段 | 阶段 | 现预 | 现导 |
|---|---|---|---|---|:--:|:--:|
| 1 | `whiteBalance`（asShot/custom） | `crs:WhiteBalance` | S0 | ● | ○ |
| 2 | `temperature` | `crs:Temperature` | S0 | ● | ○ |
| 3 | `tint` | `crs:Tint` | S0 | ● | ○ |
| 4 | `exposure` | `crs:Exposure2012` | S2 | ● | ● |
| 5 | `contrast` | `crs:Contrast2012` | S2 | ● | ● |
| 6 | `highlights` | `crs:Highlights2012` | S2 | ● | ○ |
| 7 | `shadows` | `crs:Shadows2012` | S2 | ● | ○ |
| 8 | `whites` | `crs:Whites2012` | S2 | ● | ○ |
| 9 | `blacks` | `crs:Blacks2012` | S2 | ● | ○ |
| 10 | `clarity` | `crs:Clarity2012` | S2 | ● | ○ |
| 11 | `texture` | `crs:Texture` | S2 | ○ | ○ |
| 12 | `dehaze` | `crs:Dehaze` | S2 | ● | ○ |
| 13 | `vibrance` | `crs:Vibrance` | S2 | ● | ○ |
| 14 | `saturation` | `crs:Saturation` | S2 | ● | ● |

### 4.2 `curve` 节点（11 叶）

| # | 叶 | crs: 字段 | 阶段 | 现预 | 现导 |
|---|---|---|---|---|:--:|:--:|
| 15 | `toneCurveRGB`（点数组） | `crs:ToneCurvePV2012` | S4 | ● | ○ |
| 16 | `toneCurveR` | `crs:ToneCurvePV2012Red` | S4 | ○ | ○ |
| 17 | `toneCurveG` | `crs:ToneCurvePV2012Green` | S4 | ○ | ○ |
| 18 | `toneCurveB` | `crs:ToneCurvePV2012Blue` | S4 | ○ | ○ |
| 19 | `parametric.highlights` | `crs:ParametricHighlights` | S4 | ○ | ○ |
| 20 | `parametric.lights` | `crs:ParametricLights` | S4 | ○ | ○ |
| 21 | `parametric.darks` | `crs:ParametricDarks` | S4 | ○ | ○ |
| 22 | `parametric.shadows` | `crs:ParametricShadows` | S4 | ○ | ○ |
| 23 | `parametric.shadowSplit` | `crs:ParametricShadowSplit` | S4 | ○ | ○ |
| 24 | `parametric.lightSplit` | `crs:ParametricLightSplit` | S4 | ○ | ○ |
| 25 | `parametric.midSplit` | `crs:ParametricMidSplit` | S4 | ○ | ○ |

### 4.3 `color` 节点（32 叶）

| # | 叶 | crs: 字段 | 阶段 | 现预 | 现导 |
|---|---|---|---|---|:--:|:--:|
| 26 | `hsl.red.hue` | `crs:HueAdjustmentRed` | S4 | ● | ● |
| 27 | `hsl.red.sat` | `crs:SaturationAdjustmentRed` | S4 | ● | ● |
| 28 | `hsl.red.lum` | `crs:LuminanceAdjustmentRed` | S4 | ● | ● |
| 29 | `hsl.orange.hue` | `crs:HueAdjustmentOrange` | S4 | ● | ● |
| 30 | `hsl.orange.sat` | `crs:SaturationAdjustmentOrange` | S4 | ● | ● |
| 31 | `hsl.orange.lum` | `crs:LuminanceAdjustmentOrange` | S4 | ● | ● |
| 32 | `hsl.yellow.hue` | `crs:HueAdjustmentYellow` | S4 | ● | ● |
| 33 | `hsl.yellow.sat` | `crs:SaturationAdjustmentYellow` | S4 | ● | ● |
| 34 | `hsl.yellow.lum` | `crs:LuminanceAdjustmentYellow` | S4 | ● | ● |
| 35 | `hsl.green.hue` | `crs:HueAdjustmentGreen` | S4 | ● | ● |
| 36 | `hsl.green.sat` | `crs:SaturationAdjustmentGreen` | S4 | ● | ● |
| 37 | `hsl.green.lum` | `crs:LuminanceAdjustmentGreen` | S4 | ● | ● |
| 38 | `hsl.aqua.hue` | `crs:HueAdjustmentAqua` | S4 | ● | ● |
| 39 | `hsl.aqua.sat` | `crs:SaturationAdjustmentAqua` | S4 | ● | ● |
| 40 | `hsl.aqua.lum` | `crs:LuminanceAdjustmentAqua` | S4 | ● | ● |
| 41 | `hsl.blue.hue` | `crs:HueAdjustmentBlue` | S4 | ● | ● |
| 42 | `hsl.blue.sat` | `crs:SaturationAdjustmentBlue` | S4 | ● | ● |
| 43 | `hsl.blue.lum` | `crs:LuminanceAdjustmentBlue` | S4 | ● | ● |
| 44 | `hsl.purple.hue` | `crs:HueAdjustmentPurple` | S4 | ● | ● |
| 45 | `hsl.purple.sat` | `crs:SaturationAdjustmentPurple` | S4 | ● | ● |
| 46 | `hsl.purple.lum` | `crs:LuminanceAdjustmentPurple` | S4 | ● | ● |
| 47 | `hsl.magenta.hue` | `crs:HueAdjustmentMagenta` | S4 | ● | ● |
| 48 | `hsl.magenta.sat` | `crs:SaturationAdjustmentMagenta` | S4 | ● | ● |
| 49 | `hsl.magenta.lum` | `crs:LuminanceAdjustmentMagenta` | S4 | ● | ● |
| 50 | `colorGrade.shadowHue` | `crs:SplitToningShadowHue` | S4 | ○ | ○ |
| 51 | `colorGrade.shadowSat` | `crs:SplitToningShadowSaturation` | S4 | ○ | ○ |
| 52 | `colorGrade.midtoneHue` | `crs:ColorGradeMidtoneHue` | S4 | ○ | ○ |
| 53 | `colorGrade.midtoneSat` | `crs:ColorGradeMidtoneSaturation` | S4 | ○ | ○ |
| 54 | `colorGrade.highlightHue` | `crs:SplitToningHighlightHue` | S4 | ○ | ○ |
| 55 | `colorGrade.highlightSat` | `crs:SplitToningHighlightSaturation` | S4 | ○ | ○ |
| 56 | `colorGrade.blend` | `crs:ColorGradeBlend` | S4 | ○ | ○ |
| 57 | `colorGrade.balance` | `crs:ColorGradeBalance` | S4 | ○ | ○ |

> 26–49 为 **8 色带 × 3 属性 = 24 叶**，逐带逐属性一一对应，无缺项（表内按带合并为一行以便阅读，实现与 CI 清单按 24 条独立叶计）。

### 4.4 `calibration` 节点（7 叶，S2 = tone map 之前）

| # | 叶 | crs: 字段 | 现预 | 现导 |
|---|---|---|---|:--:|:--:|
| 58 | `calibration.shadowTint` | `crs:CalibrationShadowTint` | ○ | ○ |
| 59 | `calibration.redHue` | `crs:CalibrationRedHue` | ○ | ○ |
| 60 | `calibration.redSat` | `crs:CalibrationRedSat` | ○ | ○ |
| 61 | `calibration.greenHue` | `crs:CalibrationGreenHue` | ○ | ○ |
| 62 | `calibration.greenSat` | `crs:CalibrationGreenSat` | ○ | ○ |
| 63 | `calibration.blueHue` | `crs:CalibrationBlueHue` | ○ | ○ |
| 64 | `calibration.blueSat` | `crs:CalibrationBlueSat` | ○ | ○ |

### 4.5 `detail` 节点（7 叶）

| # | 叶 | crs: 字段 | 阶段 | 现预 | 现导 |
|---|---|---|---|---|:--:|:--:|
| 65 | `detail.sharpness` | `crs:Sharpness` | S5 | ○ | ○ |
| 66 | `detail.sharpenRadius` | `crs:SharpenRadius` | S5 | ○ | ○ |
| 67 | `detail.sharpenDetail` | `crs:SharpenDetail` | S5 | ○ | ○ |
| 68 | `detail.sharpenMasking` | `crs:SharpenMasking` | S5 | ○ | ○ |
| 69 | `detail.luminanceSmoothing` | `crs:LuminanceSmoothing` | S1.5 | ● | ○ |
| 70 | `detail.colorNR` | `crs:ColorNoiseReduction` | S1.5 | ● | ○ |
| 71 | `detail.colorNRDetail` | `crs:ColorNoiseReductionDetail` | S1.5 | ○ | ○ |

> `luminanceSmoothing`/`colorNR` 在预览「生效」是因为 AI/NLM 卡走了缓存 TIFF（`denoise_service`），**不是 GPU 管线内算子**；M1 必须把它变成真算子（否则降噪在滑块上不实时）。

### 4.6 `lens` 节点（2 叶）

| # | 叶 | crs: 字段 | 阶段 | 现预 | 现导 |
|---|---|---|---|---|:--:|:--:|
| 72 | `optics.lensProfileEnable` | `crs:LensProfileEnable`（bool） | S1 | ○ | ○ |
| 73 | `optics.manualDistortion` | `crs:LensManualDistortionAmount` | S1 | ○ | ○ |

### 4.7 `effects` 节点（6 叶）

| # | 叶 | crs: 字段 | 阶段 | 现预 | 现导 |
|---|---|---|---|---|:--:|:--:|
| 74 | `optics.vignetteAmount` | `crs:PostCropVignetteAmount` | S6 | ● | ○ |
| 75 | `optics.vignetteMidpoint` | `crs:PostCropVignetteMidpoint` | S6 | ○ | ○ |
| 76 | `optics.vignetteFeather` | `crs:PostCropVignetteFeather` | S6 | ○ | ○ |
| 77 | `optics.grainAmount` | `crs:GrainAmount` | S6 | ● | ○ |
| 78 | `optics.grainFrequency` | `crs:GrainFrequency` | S6 | ○ | ○ |
| 79 | `optics.grainSize` | `crs:GrainSize` | S6 | ○ | ○ |

### 4.8 `geometry` 节点（13 叶）

| # | 叶 | crs: 字段 | 阶段 | 现预 | 现导 |
|---|---|---|---|---|:--:|:--:|
| 80 | `geometry.hasCrop` | `crs:HasCrop`（bool） | S1 | ○ | ○ |
| 81 | `geometry.cropTop` | `crs:CropTop` | S1 | ○ | ○ |
| 82 | `geometry.cropBottom` | `crs:CropBottom` | S1 | ○ | ○ |
| 83 | `geometry.cropLeft` | `crs:CropLeft` | S1 | ○ | ○ |
| 84 | `geometry.cropRight` | `crs:CropRight` | S1 | ○ | ○ |
| 85 | `geometry.cropAngle` | `crs:CropAngle` | S1 | ○ | ○ |
| 86 | `geometry.perspective.vertical` | `crs:PerspectiveVertical` | S1 | ○ | ○ |
| 87 | `geometry.perspective.horizontal` | `crs:PerspectiveHorizontal` | S1 | ○ | ○ |
| 88 | `geometry.perspective.rotate` | `crs:PerspectiveRotate` | S1 | ○ | ○ |
| 89 | `geometry.perspective.scale` | `crs:PerspectiveScale` | S1 | ○ | ○ |
| 90 | `geometry.perspective.aspect` | `crs:PerspectiveAspect` | S1 | ○ | ○ |
| 91 | `geometry.perspective.x` | `crs:PerspectiveX` | S1 | ○ | ○ |
| 92 | `geometry.perspective.y` | `crs:PerspectiveY` | S1 | ○ | ○ |

### 4.9 蒙版（1 叶）

| # | 叶 | crs: 字段 | 现预 | 现导 |
|---|---|---|---|:--:|:--:|
| 93 | `masks[]`（type/opacity/amount/geometry/strokes/range/local 9 子参数） | **无一一对应** → 见 §4.10 | ● 仅 6 槽位 | ○ 完全丢弃 |

### 4.10 无 `crs:` 对应物的降级说明（R7 例外清单）

| 项 | 现状 | 降级/处置 |
|---|---|---|
| `masks[]` 整块 | 只写 `lumina:Masks`，LR 读不到 | M2 增**双写** `crs:MaskGroupBasedCorrections`（渐变/径向/画笔）+ `crs:Mask*RangeMask*`（范围蒙版），实现 LR 双向；无法表达的部分（`aiSky`/`depth`）**保留在 lumina:Masks 并在 UI 标注「仅 LUMINA 可读」** |
| 节点 `opacity` / `blendMode` / `enabled` | 无 crs 对应 | 仅存 `lumina:NodeGraph`；投影 crs: 时**丢弃**（等价于 100%/normal/true）。导出 PSD 时物化为层的不透明度/混合/可见性（单向派生） |
| 节点 `mask.invert` / `feather` | 无 crs 对应 | 同上；`invert` 在双写时用 `crs:MaskInverted` 近似 |
| 多实例同 type（如 2 个 basic） | 无 crs 对应 | 投影时**按 stage 顺序累加**到同一 crs 字段（加法语义合法：曝光 EV 相加、对比相加）；非线性项（曲线）取最后一次非空值并在 UI 提示「LR 侧仅可见最后一个曲线」 |
| `removals[]`（修复笔画） | `lumina:Removals` | 本版**只透传不实现**（R1 保留字段）；M3 后接 `crs:RetouchAreas` |
| `adjustmentStack[]` | `lumina:AdjustmentStack` | **废弃**：迁移为 NodeGraph 节点（§3.5），字段写空 + 标记 deprecated |
| `processVersion` | `crs:ProcessVersion` | 保持 `"15.4"`；NodeGraph 引入不改 PV（参数语义未变） |

---

## 5. Plan 求值（决策 D5）

### 5.1 单一 Plan 结构

```cpp
// engine/src/plan/render_plan.h
struct PlanNode { std::string id; NodeType type; int stage; int order;
                  bool enabled; int opacity; BlendMode blend;
                  std::string maskId; uint64_t paramHash; };
struct RenderPlan {
  // 源与几何（S1 的输入输出）
  int64_t imageId, fileId; uint64_t sourceHash;      // 源文件 mtime+size+md5 前缀
  ResampleFilter filter; int64_t iccIn, iccOut;      // M4
  RectF cropNorm;                       // 裁后归一化 [0,1]
  double straightenDeg, lensDistortion, persp[7];
  int outW, outH;                       // 唯一定义输出尺寸（C4）
  // 节点
  std::vector<PlanNode> nodes;          // 已按 (stage, order) 排好序
  MaskPlan masks;                       // 打包后的蒙版数据（与 GPU 纹理布局一致）
  // 域与输出
  ColorSpaceTag workingSpace, outputSpace; int outBps;
  bool forPreview; double previewScale;  // 预览放大/缩小
  // 三键缓存
  CacheKey kGeom, kTone, kDisplay;
};
```
**生成规则**：`PlanBuilder::build(imageId, nodeGraph, intent)` — `intent ∈ {Preview(maxEdge), Export(spec), Thumb(maxEdge), Proof(icc)}`。CPU oracle 与 GPU 渲染器**都只接受 `RenderPlan`**，不直接读 `EditParams`（编译期禁止：`webgl/renderer.ts` 与 `imaging/pipeline_cpu.cpp` 均不 import `EditParams` 的字段常量表）。

### 5.2 缓存键设计（三键分级）

| 键 | 组成 | 失效触发 | 存储 |
|---|---|---|---|
| `kGeom` | `sourceHash + cropNorm + straighten + lensDistortion + persp[7] + outW/outH + filter + workingSpace` | 裁剪/变换/镜头/输出尺寸/源文件变 | 磁盘 `{home}/cache/geom/{kGeom}.lum16`（可选）+ 内存 LRU |
| `kTone` | `kGeom + hash(basic 14 叶) + hash(calibration 7 叶) + hash(detail.NR 3 叶) + exposureBias` | 上述任一参数变 | 内存 LRU（上限 512MB，按 kGeom 分组） |
| `kDisplay` | `kTone + hash(curve 11 + color 32 + effects 6 + detail.sharp 4) + outputSpace + outBps + opacity/blend/enabled 位图` | 显示域参数或节点开关/混合变 | 内存 LRU（上限 512MB） |

**失效策略**：改 `basic.exposure` → 只废 `kTone`/`kDisplay`（`kGeom` 命中，**不重新解码不重新裁剪**）；改 `opacity`/`enabled`/`blendMode` → 只废 `kDisplay`（见 §10 性能预算，这是根治 photocraft 276ms 的关键）；改裁剪 → 全废。

---

## 6. 管线顺序（决策 D6：按 LR 重排）

| 阶段 | 语义 | 节点 | 域/格式 | 执行位置 | 备注 |
|---|---|---|---|---|---|
| **S0 解码** | CFA → scene-linear；**白平衡增益在此施加** | `basic`(WBTemp/Tint) | log16，`SceneLinearCamera` | CPU（LibRaw 16bit 线性输出） | 白平衡用**通道增益**（源自 `temperature/tint` → 相机 RGB 增益），不是 gamma 域平移 |
| **S1 几何** | 镜头畸变校正 → 透视/拉直（合成单一 3×3 单应）→ 裁剪 → **唯一一次 resize** | `lens`, `geometry` | 同上 | CPU（预览可 GPU） | 见 §7 |
| **S1.5 线性降噪** | 亮度/色度 NR | `detail`(NR 3) | 同上 | CPU/GPU | 线性域降噪数学正确 |
| **S2 scene-linear 色调** | 曝光/对比/高低光/白黑/清晰度/纹理/去雾/饱和/自然饱和 + **校准** | `basic`(12) + `calibration`(7) | scene-linear | CPU oracle + GPU | 校准必须在此（tone map 之前） |
| **S3 tone map** | scene-linear → [0,1] 显示域（高光滚降 + 黑场） | （算子，非节点） | log16，display 域 | CPU/GPU | **唯一域分界** |
| **S4 颜色** | 曲线（复合/分通道/参数）→ HSL 8 带 → 颜色分级 | `curve`(11), `color`(32) | display 域 | CPU oracle + GPU | 曲线在编码之后 |
| **S5 细节** | 锐化（半径/细节/蒙版） | `detail`(锐化 4) | display 域 | CPU/GPU | 锐化必须在缩放后（S1 已定尺寸） |
| **S6 效果** | 裁后暗角 + 颗粒 | `effects`(6) | display 域，**裁后归一化坐标** | CPU/GPU | 暗角用裁后坐标（不是原图坐标） |
| **S7 显示/输出** | 输出色彩空间（ICC）→ 8/16bit 编码 + 抖动 → 水印 | — | 8/16bit | CPU | 水印是交付层，不入 Plan 缓存键 |

**四个易错点（强制断言，写进单测）**：
1. **白平衡最前**：S0 之后断言 `buf.space == SceneLinearCamera` 且 WB 增益已应用（`whitePoint` 记录）——禁止 S2 内再做色温平移（现状 `shaders.ts:129-135` 即此错）。
2. **校准在 tone map 之前**：S2 内 `calibration` 的算子 `stage=2`，S3 的 `stage=3`，排序断言保证。
3. **暗角用裁后坐标**：S6 的 `uv` 由 `outW/outH` 归一化，单测（同参数 + 不同裁剪 → 暗角中心始终在画面中心）。
4. **曲线在编码之后**：S4 断言 `buf.space ∈ {EncodedSRGB, …}`；若在 S2 调用曲线算子 → 编译期（`OpTable` 的 stage 校验）直接失败。

---

## 7. 几何阶段：单次重采样（决策 D7）

**现状缺陷**：`exporter.cpp:700-719` 先 `resizeImage`（盒式滤波）再 `applyToneApprox` → 两次重采样 + 插值劣化 + 锐化/暗角作用在错误尺度上。

**新流程（S1 内，一次重采样）**：
```
1) 畸变校正 + 透视 + 拉直 → 合成映射 M（3×3 单应；畸变用 opencv 风格布朗模型先做网格重映射）
2) 裁剪：由 cropNorm × 源尺寸 得有效矩形（在 M 的目标坐标系）
3) 输出网格：targetW×targetH（由 intent 决定：预览 maxEdge / 导出 longEdge / 缩略图）
4) 一次性逆向映射采样：dst(x,y) = src(M⁻¹·(x,y))，逐 tile 在 f32 平面完成
```
**重采样算法（`ResampleFilter`）**：

| 场景 | 算法 | 理由 |
|---|---|---|
| 缩小比例 > 2× | **Lanczos3 + 面积预滤波** | 抗摩尔纹；盒式（现状）会产生锯齿与混叠 |
| 缩小 ≤2× / 放大 | **Catmull-Rom（bicubic）** | 锐度可控、无振铃过冲（相比 Lanczos 更适合摄影交付） |
| 交互预览（拖动中） | **双线性**（降级）+ 松手后 120ms 内重算高质量 | 保 16ms 帧预算 |
| 缩略图（≤320px） | **面积平均（box 但面积加权，非现状的等权采样）** | 与缩略图用途匹配，最快 |

**性能**：6K→2K Lanczos3 单线程 ≈ 180ms（tile 并行 4 线程 ≈ 55ms）；仅当 `kGeom` 未命中时发生。

---

## 8. 三处数据硬伤（决策 D8–D10）

### 8.1 四级表：图像 ↔ 文件 ↔ 文件夹 ↔ 根（D8）

```sql
-- migrations/002_four_level.sql（schema_version 2）
CREATE TABLE roots2(id INTEGER PRIMARY KEY, path TEXT UNIQUE NOT NULL, label TEXT);
CREATE TABLE folders2(id INTEGER PRIMARY KEY, root_id INT NOT NULL REFERENCES roots2(id),
                      rel_path TEXT NOT NULL, name TEXT, missing INT DEFAULT 0,
                      UNIQUE(root_id, rel_path));                    -- 文件夹身份 = 根 + 相对路径
CREATE TABLE files2(id INTEGER PRIMARY KEY, folder_id INT NOT NULL REFERENCES folders2(id),
                    name TEXT NOT NULL, format TEXT, file_size INT, md5 TEXT, mtime INT,
                    missing INT DEFAULT 0, health TEXT DEFAULT 'unchecked',
                    UNIQUE(folder_id, name));                        -- 文件身份 = 文件夹 + 文件名
CREATE TABLE images2(id INTEGER PRIMARY KEY, primary_file_id INT NOT NULL REFERENCES files2(id),
                    format_display TEXT, width INT, height INT, captured_at INT, imported_at INT,
                    camera TEXT, lens TEXT, iso INT, aperture TEXT, shutter TEXT, focal_length TEXT,
                    gps_lat REAL, gps_lng REAL, gps_alt REAL, location_id INT,
                    rating INT DEFAULT 0, pick TEXT DEFAULT 'none', color_label TEXT,
                    keywords TEXT, description TEXT, seq_id INT, pair_kind TEXT DEFAULT 'single',
                    deleted INT DEFAULT 0);
CREATE TABLE image_files(image_id INT NOT NULL REFERENCES images2(id),
                    file_id INT NOT NULL REFERENCES files2(id), role TEXT NOT NULL,
                    PRIMARY KEY(image_id, role));                    -- role: raw|jpg|sidecar|composite
CREATE VIEW photos AS   -- 兼容层：旧 DAO/RPC 全部不变
  SELECT i.id, f.folder_id, fo.root_id, f.name AS file_name,
         fo.rel_path || '/' || f.name AS file_path, f.format, f.file_size, f.md5,
         i.width, i.height, i.captured_at, i.imported_at, i.camera, i.lens, i.iso,
         i.aperture, i.shutter, i.focal_length, i.gps_lat, i.gps_lng, i.gps_alt,
         i.location_id, i.rating, i.pick, i.color_label, i.keywords, i.description,
         s.rel_path || '/' || s.name AS sidecar_path, s.mtime AS sidecar_mtime,
         /* has_edits 见 8.2 */ 0 AS has_edits, f.missing AS is_missing,
         0 AS is_video, i.pair_kind, f.health, i.deleted, i.seq_id
  FROM images2 i JOIN files2 f ON f.id = i.primary_file_id
       JOIN folders2 fo ON fo.id = f.folder_id
       LEFT JOIN image_files sf ON sf.image_id = i.id AND sf.role = 'sidecar'
       LEFT JOIN files2 s ON s.id = sf.file_id;
```
- **迁移脚本幂等**：`INSERT OR IGNORE` 从 `photos` 灌入（`roots`→`roots2` 等）；首次运行后保留旧表（只读）一个版本，`schema_version=3` 时删。
- **关键收益**：移动/重命名文件夹只改 `folders2.rel_path`，`images2.id` 不变 → sidecar/评分/历史/相册引用全部不断裂；RAW+JPG 配对由 `image_files(role)` 天然表达（替代现状 `pair_kind` 字符串）。
- **DAO 不改签名**：`PhotoDao::query/getById/upsert` 返回结构不变，内部改读 `photos` 视图；新接口 `ImageDao` 供 v1.3 使用。

### 8.2 库内 `develop_settings` 缓存 + sidecar 一致性（D9）

```sql
CREATE TABLE develop_settings(
  image_id INTEGER PRIMARY KEY REFERENCES images2(id),
  params_json TEXT NOT NULL,        -- NodeGraph 全量 JSON（与 sidecar 同构）
  params_hash TEXT NOT NULL,        -- sha1(canonical json)，缓存键与变更检测
  pv TEXT, schema_version INT,
  sidecar_mtime INT,                -- 写入时的 sidecar mtime（一致性锚点）
  node_count INT, updated_at INT, source TEXT DEFAULT 'xmp');
CREATE INDEX idx_dev_edited ON develop_settings(image_id) WHERE params_hash <> '';
```
**一致性规则（XMP 唯一真相源，C1）**：
| 场景 | 行为 |
|---|---|
| 打开/查询 | `sidecar_mtime > develop_settings.sidecar_mtime` → **重读 sidecar 覆盖缓存**（记 `debug` 日志，不打扰用户） |
| 保存 | **先原子写 sidecar**（temp+rename）→ 成功后 upsert 缓存；写缓存失败只记 warning（缓存可自愈，**不得因此回滚 sidecar**） |
| sidecar 缺失 | 缓存标记 `source='db-stale'` + UI 显式提示「参数缓存无对应 sidecar」，提供「重建 sidecar」 |
| 冲突（外部改 sidecar 且 mtime 变） | 走既有 `1005 XMP_EXTERNAL_CONFLICT` 三选（重读/覆盖/另存），不新增语义 |
| 用途 | `library.queryPhotos{edited:true}` 走 `idx_dev_edited`（不再依赖 `has_edits` 布尔）；DB 备份天然含全部参数 |
| 迁移 | 启动后台任务：对 `has_edits=1` 的图，解析 sidecar 回填缓存（进度进 job，可取消，零静默失败） |

### 8.3 history 落盘（D10）

```sql
CREATE TABLE history_entries(
  id INTEGER PRIMARY KEY, image_id INT NOT NULL REFERENCES images2(id),
  seq INT NOT NULL,                 -- 单调递增，同 image 内唯一
  op_json TEXT NOT NULL,            -- 见下「op 编码」
  label TEXT,                       -- UI 显示名（"曝光 +0.15"）
  created_at INT NOT NULL, session_id TEXT,
  UNIQUE(image_id, seq));
CREATE INDEX idx_hist_image ON history_entries(image_id, seq);
CREATE TABLE history_state(image_id INTEGER PRIMARY KEY, current_seq INT, base_json TEXT);
```
**op 编码（禁止全文档 clone）**：每条 op 存**节点级参数 diff**：

```jsonc
{ "t":"param", "node":"n1", "path":"exposure", "from":0.10, "to":0.15 }
{ "t":"param", "node":"n3", "path":"mask.geometry.radius", "from":0.30, "to":0.35 }
{ "t":"node",  "op":"add"|"remove"|"reorder", "nodeId":"n4", "snapshot":{...} }   // 结构性操作才带该节点全量
{ "t":"meta",  "path":"enabled", "node":"n3", "from":true, "to":false }
{ "t":"bulk",  "ops":[...] }                                                       // 批量（预设/同步）打包为一条
```
单条 op 典型 80–160B（对比全文档 3–8KB，**压缩 30–50×**）。
- 撤销 = 反向应用 diff；`history_state.base_json` 存「最近快照或初始参数」，重放 = `base + Σdiff[0..current]`。
- **容量**：每图保留最近 **500 步**（超出按 50 步折叠为 checkpoint 写入 `base_json`，丢弃更早 diff）；LR 实测 52,913 步全局，我们的口径是「单图 500 步 / 全库不设上限」（存储 ≈ 80KB/图，1.8 万图约 1.4GB，可接受）。
- **与快照/虚拟副本的边界（不得混用）**：
  | 概念 | 语义 | 载体 | 写入频率 |
  |---|---|---|---|
  | history | 操作流（可撤销） | `history_entries`（diff） | 每次提交 |
  | snapshot | 用户显式命名的**完整参数文档** | 既有 `snapshots` 表 + sidecar `lumina:Snapshot` | 低频手动 |
  | virtual copy | **独立可编辑副本**（独立参数） | `virtual_copies` → 迁移为 `images2` 新行（`image_files.role='vcopy'`） | 手动创建 |
  - 恢复快照 = 追加一条 `{t:"snapshot-restore", snapshotId}` op（不删历史）；虚拟副本切换 = 切 `image_id`（历史各自独立）。
- **性能预算**：单条 op 落盘 ≤1ms（单条 INSERT，WAL）；**改不透明度/切可见性 ≤16ms**（只废 `kDisplay` + 追加 1 条 op；photocraft 因全文档 clone 为 276ms/178ms，我们低一个数量级）。

---

## 9. PSD 出口（M3）与 LCMS2（M4）接口位

### 9.1 PSD 写出（单向派生，C1）

| 项 | 设计 |
|---|---|
| 层来源 | `RenderPlan.nodes` 中 `enabled` 的节点 → 每节点一层：层名 = 面板名（中文，附 ASCII 别名）、`blendMode` 映射、`opacity`、`enabled→hidden` |
| 蒙版层 | 带 `mask` 的节点额外写**层蒙版通道**（灰度 8bit，来自同一 `MaskField`） |
| 合并底图 | 完整 Plan 渲染结果（**与同参数 TIFF 逐像素相等**，M3 验收②） |
| 每层快照 | 层 `lumi` tagged block 内写该节点的 `lumina:` 快照 JSON（`{nodeId,type,params,maskRef}`）——**只写不读** |
| 未知块 | 读入既有 PSD 时保留未知 tagged block 原样（本期只写不读，接口 `psd::UnknownBlocks` 先留，字节往返断言进 CI） |
| 位深 | 8/16bit（16bit 需 `depth=16` + RLE 边界用例） |
| ICC | M4 后嵌入真实 profile（`prints`/`sRGB`/`AdobeRGB`）；M3 阶段先嵌内置常量 profile |
| 禁止 | **无 PSD→参数回读路径**（`psd_writer` 只写；如未来要读，须先解决双真相源） |

### 9.2 LCMS2 接口位（M4）

```cpp
// engine/src/icc/icc_transform.h
struct IccTransform {                                   // RAII，句柄池 LRU 32
  static Result<IccTransform> create(int32_t srcId, int32_t dstId,
                                     Intent intent /*Perceptual|Relative|Saturation*/,
                                     bool bpc);
  void apply(const ImageBuf& in, ImageBuf* out) const;   // display 域，f32 内部
};
```
接入点三处：① **S7 输出**（`outputSpace` → ICC，任一导出/预览落盘）；② **软打样**（模拟目标 ICC 到屏幕）；③ **PSD 层 ICC 嵌入**。
**失败语义（禁止静默）**：`IccRegistry::get(id)` 失败 → `7051 PROFILE_MISSING` + UI banner「色彩配置缺失，已按矩阵近似输出（点击修复）」；**绝不静默回退**，且近似路径必须在导出报告里留 `colorPath:"matrix-approx"`。

---

## 10. 量化目标与性能预算（决策 D11）

| 判据 | 目标 | 测量方式 | 现状 |
|---|---|---|---|
| 参数叶覆盖 | **93/93**（预览● + 导出● + 断言存在） | CI 解析 §4 表 → 断言每个叶在 `plan/ops/*.cpp` 与 `webgl/ops/*` 中出现 | 27 导 / 39 预 |
| CPU vs GPU 等价 | `mean|Δ| < 0.5`、`max ≤ 3 LSB` | 200 组参数矩阵 × 5 张图，S7 输出比对（8bit） | 无（不可比） |
| 全文档 clone 禁令 | 无 `cloneDocument()` 调用点 | grep 断言 + CR 检查 | — |
| **改不透明度** | **≤16ms** p95 | 台账：`kDisplay` 失效 + 1 op 落盘 + 单 pass 重绘 | photocraft 276ms（全 clone） |
| **切可见性** | **≤16ms** p95 | 同上（`enabled` 位进 `kDisplay` 键） | photocraft 178ms |
| 连续滑块 | ≤16ms p95（`kTone` 命中） | 16bit log16 编解码 + tile 算子 | 现 ~8ms（8bit 便宜） |
| 拉伸/拖拽（几何） | ≤33ms p95（`kGeom` 未命中，双线性预览） | 松手 120ms 后高质量重算 | 无几何阶段 |
| 导出 6K（单张） | ≤8s p50（解码 4.6s + 16bit 管线 ~2-3s） | `bench/export_6k` | 2.7s@8bit（但只 27 叶） |
| 内存峰值（6K 导出） | ≤900MB | RSS 采样 | ~300MB |
| history 落盘 | ≤1ms/op | SQLite WAL 单 INSERT | 无（内存） |

**实现要点**：stage 级失效（§5.2）+ **tile 流式求值**（512×512，峰值 = tiles 数 × tile 内存，而非整图 × N 缓冲）+ 预览走 `previewScale`（最多 2048 长边），使 16bit 不带来内存爆炸。

---

## 11. 任务分解（M0–M4 · 204 人日口径）

> 依赖用 `←` 表示；验收条件按启动文档 §5.1 出口条件细化。人日含自测与文档，不含联调返工。

### M0 地基（35 人日）

| ID | 任务 | 文件 | 人日 | 依赖 | 验收 |
|---|---|---|---:|---|---|
| T0.1 | `ImageBuf` + log16 编解码 + 单测（往返误差 ≤0.05%） | `engine/src/imaging/image_buffer.{h,cpp}`、`tests/test_image16.cpp` | 5 | — | 65536 值全域往返 max rel err < 0.1% |
| T0.2 | RAW 16bit 线性输出（`output_bps=16`、gamma 线性、关自动提亮） | `engine/src/raw/raw_decoder.cpp`、`tests/test_raw16.cpp` | 5 | T0.1 | 解 S5M2 RW2 得 16bit 线性，`whitePoint` 记录正确，与 8bit 版视觉可比 |
| T0.3 | 算子表骨架 + `OpTable` stage 校验 + CPU oracle 入口 | `engine/src/plan/ops/*.cpp`、`engine/src/plan/op_table.{h,cpp}` | 6 | T0.1 | stage 错序编译/运行期报错 |
| T0.4 | 几何阶段：单应合成 + 一次重采样（Lanczos3/Catmull-Rom/box） | `engine/src/plan/geometry.{h,cpp}`、`imaging/resample.{h,cpp}` | 7 | T0.1 | 裁剪+拉直+缩放后仅一次采样（断言采样次数=1）；与参考实现 PSNR>45dB |
| T0.5 | Plan 结构 + PlanBuilder + 三键缓存骨架 | `engine/src/plan/render_plan.{h,cpp}`、`plan/cache_key.{h,cpp}` | 5 | T0.3 | 换 exposure 不废 kGeom（单测命中计数） |
| T0.6 | parity harness（CPU vs GPU，mean/max Δ） | `engine/tests/parity/{harness.cpp,params.json}`、`app/src/modules/develop/parity/*` | 5 | T0.3,T0.5 | 输出 CI 报表；地基阶段先跑 12 基础参数 |
| T0.7 | 8bit 兼容层 + 缓存版本隔离 + 抖动 | `imaging/output_encode.{h,cpp}`、`core/config.cpp`(cache v2) | 2 | T0.1 | 旧 sidecar 读取无变更；8bit 输出带 TPDF |
| | **小计** | | **35** | | |

### M1 求值器统一（45 人日）

| ID | 任务 | 文件 | 人日 | 依赖 | 验收 |
|---|---|---|---:|---|---|
| T1.1 | `NodeGraph` 数据结构 + JSON 编解码 + 默认图构造 | `engine/src/graph/node_graph.{h,cpp}`、`graph/serialize.{h,cpp}` | 6 | T0.5 | round-trip 字节稳定；8 类型全覆盖 |
| T1.2 | `lumina:NodeGraph` 写/读 + SchemaVersion 1→2 迁移 + `adjustmentStack` 映射 | `xmp/xmp_mapper.cpp`、`xmp/migrator.{h,cpp}`、`rpc/version.h` | 7 | T1.1 | v1→v2→v1 可逆；`crs:` 字节保留；`crs:` 投影同值 |
| T1.3 | 93 叶算子表：CPU oracle 全实现（basic/curve/color/calibration/detail/effects/lens/geometry） | `plan/ops/{basic,curve,color,calibration,detail,effects}.cpp`、`plan/ops/mask.cpp` | 12 | T1.1,T0.3 | 每叶有单测（值域/单调性/边界） |
| T1.4 | GPU 侧统一算子（shader 重写：线性域 + 域分界 + 8 节点） | `app/src/modules/develop/webgl/{shaders.ts,renderer.ts,ops/*.ts}` | 10 | T1.3 | 每叶 GPU 实现与 CPU 同参可比值 |
| T1.5 | 销毁三套实现：导出改走 Plan；`cpu-render.ts` 改走同一 Plan 的 CPU 后端 | `export/exporter.cpp`、`app/src/modules/develop/cpu-render.ts` → 删除，改用 `planClient` | 5 | T1.3 | 导出=预览像素断言（CI） |
| T1.6 | 93/93 覆盖报表（CI 产物）+ 三格断言 | `engine/tests/coverage/leaf_matrix.json`、`scripts/ci/leaf_coverage.py` | 3 | T1.3,T1.4,T1.5 | CI 输出 93 行 × 3 列全 ●，缺一即失败 |
| T1.7 | UI 面板接线到节点（8 组 → 节点实例）+ 删除假控件占位 | `app/src/modules/develop/panels/*`、`store/developStore.ts` | 2 | T1.4 | 控件改动 → 节点 params 变更 → 预览像素变化（逐控件像素断言） |
| | **小计** | | **45** | | |

### M2 LR 闭环（60 人日）

| ID | 任务 | 文件 | 人日 | 依赖 | 验收 |
|---|---|---|---:|---|---|
| T2.1 | 管线顺序重排 + 四易错点断言（WB 首/校准前/暗角裁后/曲线编码后） | `plan/pipeline.cpp`、`tests/test_pipeline_order.cpp` | 6 | T1.3 | 四点单测 + 同参数与 LR 目视对齐 |
| T2.2 | 蒙版数学正确化 + 槽位解限（数据纹理 + 门控循环）+ 范围蒙版落地 | `plan/ops/mask.cpp`、`webgl/ops/mask.ts`、`develop/panels/MaskPanel.tsx` | 12 | T2.1 | 12 蒙版与参考实现 mean\|Δ\|≤2/255；范围蒙版不再回退画笔 |
| T2.3 | 曲线全谱（复合/分通道/参数曲线） | `plan/ops/curve.cpp`、`webgl/ops/curve.ts`、`panels/CurveEditor.tsx` | 8 | T2.1 | 四类曲线逐叶断言；参数曲线四分区边界正确 |
| T2.4 | 几何交互（裁剪/拉直/透视/比例）与 `kGeom` 联动 | `panels/TransformBar.tsx`、`plan/geometry.cpp` | 9 | T0.4,T2.1 | 裁剪后蒙版不漂移；缩放只一次采样 |
| T2.5 | 镜头校正（参数化，无 LCP） | `plan/ops/lens.cpp`、`panels/LensPanel.tsx` | 5 | T2.4 | 畸变/暗角可校正；无 LCP 依赖 |
| T2.6 | 四级表迁移 + `ImageDao` + `photos` 兼容视图 | `db/migrations/002_four_level.sql`、`db/image_dao.{h,cpp}`、`database.cpp` | 8 | — | 移动文件夹后 image.id 不变（迁移+回放测试）；旧 RPC 全绿 |
| T2.7 | `develop_settings` 缓存 + 一致性规则 + 回填任务 | `db/develop_settings_dao.{h,cpp}`、`edit/edit_session.cpp` | 5 | T2.6 | sidecar 优先规则三用例；`edited:true` 走索引 |
| T2.8 | history 落盘（diff op + 折叠 + 撤销重放） | `edit/history.{h,cpp}`、`db/history_dao.{h,cpp}`、`migrations/003_history.sql` | 7 | T2.6 | 重启后撤销链完整；单 op ≤1ms；500 步折叠正确 |
| | **小计** | | **60** | | |

### M3 PS 出口（35 人日）

| ID | 任务 | 文件 | 人日 | 依赖 | 验收 |
|---|---|---|---:|---|---|
| T3.1 | PSD writer 骨架（header/colorMode/图像数据 RLE，8/16bit） | `engine/src/psd/{psd_writer,psd_stream,rle}.{h,cpp}` | 8 | T1.5 | psd-tools 可解析；8/16bit 尺寸/位深正确 |
| T3.2 | 图层与蒙版通道写出（节点→层、opacity/blend/hidden、蒙版灰度通道） | `psd/psd_layers.{h,cpp}` | 9 | T3.1 | psd-tools 层数/层名/位深/hidden 一致 |
| T3.3 | 每层 `lumina:` 快照 + ICC 嵌入 + 未知块透传接口 | `psd/psd_meta.{h,cpp}`、`icc/icc_registry.cpp` | 6 | T3.2 | 层快照可读出；未知块字节往返一致（接口级） |
| T3.4 | 合并底图对拍（PSD merged composite = 我们的 Plan 渲染） | `tests/test_psd_oracle.cpp`、`scripts/ci/psd_parity.py` | 6 | T3.2 | 与同参 TIFF 逐像素相等；地板值只升不降 |
| T3.5 | PS 目验与修复（用户侧人工项） | — | 3 | T3.3 | Photoshop 打开无修复警告 |
| T3.6 | 导出器接入（`export.createJob` psd 生效，5xxx 拒绝移除） | `export/exporter.cpp`、`rpc/methods.cpp` | 3 | T3.4 | 四格式全通；无伪 PSD |
| | **小计** | | **35** | | |

### M4 色彩与打印（29 人日）

| ID | 任务 | 文件 | 人日 | 依赖 | 验收 |
|---|---|---|---:|---|---|
| T4.1 | LCMS2 静态接入 + `IccRegistry` + transform LRU | `thirdparty/lcms2`、`engine/src/icc/*` | 6 | T1.5 | sRGB↔AdobeRGB 往返 ΔE00 <0.5；句柄 LRU 命中率埋点 |
| T4.2 | S7 输出接 ICC + 嵌入 AdobeRGB + 8/16bit 全通 | `imaging/output_encode.cpp`、`export/exporter.cpp` | 5 | T4.1 | ICC 往返无损；导出 ICC 标签可被 exiftool 读出 |
| T4.3 | 软打样（目标 ICC → 预览）+ ΔE 报告 + 失败语义 | `print/soft_proof.{h,cpp}`、`ui/modules/print/*` | 8 | T4.2 | ΔE00 中位 ≤2.0；缺 ICC → `7051` + banner（禁静默） |
| T4.4 | 输出 ICC 与 PSD 层 ICC 统一 + 文档 | `icc/icc_builtin.{h,cpp}`、docs | 4 | T4.2 | 三处（S7/软打样/PSD）用同一 profile 注册表 |
| T4.5 | 色彩科学 B 验收（对相机 JPEG 中位 ΔE≤3）+ 基线与回归 | `tests/color/delta_e_baseline.cpp`、`scripts/ci/deltae.py` | 6 | T4.2 | 对 S5M2 相机 JPEG 中位 ΔE≤3；报表进 CI（只升不降） |
| | **小计** | | **29** | | |

**人日校核**：35+45+60+35+29 = **204** ✅（与启动文档一致，未修正分配）。
**关键路径**：T0.1→T0.3→T1.1→T1.3→T1.4→T1.6（M0/M1 串行 27 人日不可并行）→ T2.1→T2.2（蒙版）与 T2.6→T2.8（数据）可双人并行。

---

## 12. 工程纪律落成（CI 可执行）

| 规则 | 检查脚本 | 阻断级别 |
|---|---|---|
| 93 叶覆盖 | `scripts/ci/leaf_coverage.py`（解析 §4 表 vs `plan/ops/*.cpp` + `webgl/ops/*`） | **deny**（缺一即 MR 不可合） |
| 分层不穿透 | `scripts/ci/layering.py`（`target_link_libraries` 依赖方向：`plan` 不得依赖 `export`/`rpc`） | deny |
| 无全文档 clone | `scripts/ci/no_clone.py`（grep `cloneDocument\|params_json 全量复制` 白名单外命中） | deny |
| parity 不倒退 | `scripts/ci/parity.py`（地板值 mean0.5/max3，波动 >0.1 报警） | warn→deny（M2 后） |
| 未知数据不丢 | `tests/test_psd_unknown.cpp` 字节往返 | deny |
| 性能不退化 | `scripts/ci/perf_guard.py`（§10 表阈值，超 20% 失败） | warn→deny（M1 后） |
| 零静默 | `scripts/ci/silent_catch.py`（catch 块无 UI 提示/无 log 计数） | deny |

---

## 13. 待确认（附默认值，不阻塞）

| # | 事项 | 默认值 |
|---|---|---|
| Q1 | log16 档宽 2048 是否够（vs 1024/4096） | **2048**（0.05% 精度，1 档 12–16 stops）；格式版本化，变更需迁移缓存 |
| Q2 | 预览最大边长 | **2048px**（4K 屏够用；`intent=Preview` 统一） |
| Q3 | `mask` 的 `feather` 是否入本轮 | **入**（0–100 直径 2px–30% 画面），与 LR 一致 |
| Q4 | 多实例同 type 的 crs: 投影加法是否会对 LR 用户造成意外 | **加法**（曝光 EV/对比/饱和可加；曲线取最后非空 + UI 提示） |
| Q5 | history 折叠阈值 | **单图 500 步 / 每 50 步 checkpoint** |
| Q6 | `develop_settings` 写失败是否阻断保存 | **不阻断**（sidecar 是真相源），记 warning + UI 状态点 |
| Q7 | PSD 层名语言 | **中文 + ASCII 别名**（`基础 Basic`），兼容 PS 层名搜索 |
| Q8 | 软打样 ΔE 承诺值 | **中位 ≤2.0 / p95 ≤4.0**（LCMS2 到位后）；缺 ICC 不承诺 |
| Q9 | 色彩科学 B 的参考集 | 用户 S5M2 相机 JPEG 200 张（含高感/大光比各 1/3），固定清单进仓库 |
| Q10 | 几何阶段的畸变模型 | **Brown-Conrady k1/k2/k3**（参数化；LCP 不做） |

---

## 14. 风险最高的三个设计点（自评）

1. **CPU/GPU 等价性（§10 mean\|Δ\|<0.5、max≤3LSB）是最容易"看起来做了但过不了"的一项。** 难点不在算法，而在**数值纪律**：GPU shader 无 16bit 中间精度（`mediump` 必须禁）、HSL 往返的色相环绕、曲线 LUT 的 8bit 量化、蒙版混合的运算顺序。缓解：**oracle-first**（CPU 先实现并被单测覆盖，GPU 只允许逼近）；统一「中间结果量化到 log16 再比对」；`precision highp float` 硬断言 + 禁 `mediump`；曲线 LUT 提升到 1024 项（8bit LUT 本身就是 max>3LSB 的隐患）。
2. **色彩科学路线 B 的 ΔE≤3 目标可能不可达**（对标项目 20 万行 Rust 对相机 JPEG 也只有 5.0–5.1）。缓解：**M0 末就做基线测量**（T0.2 后跑 `deltae.py` 拿现状数字），若基线已 >6，则必须提前决策降级口径（改为「ΔE 不退化」或调整参考集），**不能等到 M2 收口才发现**。这是本版唯一可能推翻里程碑的风险。
3. **16bit 迁移带来的"既有编辑结果全变"**（1.8 万张库 + 4,347 张已调整）。技术上正确（修 bug），但用户感知为"我的照片变了"。缓解：发布前产出 `视觉差异报告`（同参数 8bit vs 16bit 并排 + ΔE 分布），UI 首次启动给出一次性说明（可关闭，不静默）；并把 `legacy8bit` 开关保留一个版本做自证。

---

*本文与启动文档冲突时以启动文档为准；参数语义与 `crs:` 字段以 `specs/XMP-字段映射表.md` 为准。本文冻结后，§4 的 93 叶表即成为 R7 的验收基线，任何新增参数必须先在本表登记。*
