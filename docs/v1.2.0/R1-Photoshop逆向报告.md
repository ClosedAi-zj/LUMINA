# R1 · Adobe Photoshop 架构与功能逆向社会报告

> 版本：v1 ｜ 日期：2026-10-24 ｜ 作者：逆向工程分析（re-photoshop）
> 用途：LUMINA v1.2.0「全面对标 PS + Lightroom」的架构输入之一
> **本文不是 Adobe 官方文档，不代表 Adobe 的任何描述或承诺。**

---

## 1. 逆向口径

### 1.1 目标版本

| 项 | 值 | 来源 |
|---|---|---|
| 主程序 | Adobe Photoshop 2026 **27.11.0**（build 20260929.r.26，macOS universal） | `target-app-parity.md:46` |
| 配套 RAW 引擎 | **Camera Raw 18.7** | `target-app-parity.md:47` |
| 观察方式 | **未启动程序**，纯静态检查安装包 | `target-app-parity.md:46-48` |
| 交叉验证 | GitHub 用户报告 + Adobe 文档/发行说明 | `target-app-parity.md:56-57` |

### 1.2 证据类型分级（全文统一使用）

| 级别 | 含义 | 典型来源 | 可信度 |
|---|---|---|---|
| **measured（实测）** | 对安装包做静态枚举或对文件语料做逐像素比对得到的**计数/布尔值** | 菜单 631 项、工具 68 个、面板 35 个、格式 31 种、`.kys` 67 个工具 | 高，但只说明"存在"，不说明"好用" |
| **estimated（估计）** | 由作者按权重打分得出的百分比/工时 | "ready for real work ~45%"、"~2,300–4,100 agent-hours" | 中，方法透明但含主观权重 |
| **knowledge（知识）** | 本文作者依 Photoshop 领域经验补充的机制解释，refs 中无对应条目 | 混合模式数学式、图层样式参数结构、PSD 块语义 | 中低，**凡此类均显式标注** |

**纪律**：本文每个量化数字后面都跟 `path:line` 或 `知识/估计`；没有第三条路。凡是 refs 里查不到精确值的，一律写区间或写"知识"，不编造小数。

### 1.3 证据链的一个重要性质

refs 中的 `photocraft/docs/*` 是**第三方软件（PhotoCraft）为对标 PS 而写的 parity 文档**，不是对 PS 的反编译。因此：

- 它给出的 PS 侧数字（68 工具 / 631 菜单 / 35 面板 / 31 格式）来自**安装包静态枚举**，可信；
- 它给出的 PS 侧**行为**（例如"PS 的图层合成主要在 CPU"）是观察结论，属于 estimate，本文按 estimate 对待；
- 真正的"数值真值"来自 **PSD 的合并预览**：PS 写出的 PSD 里带着它自己合成的结果，任何自研合成器都可以拿它当免费 oracle（`architecture.md:394-396`）。这是本报告方法论上最重要的一条（见 §12）。

---

## 2. 应用架构总览

### 2.1 文档模型：画布 + 通道 + 位深 + 色彩模式

PS 的一切都挂在一个**文档（Document）**上，文档由四个正交维度定义：

| 维度 | 取值 | 对实现的影响 |
|---|---|---|
| 画布 | 宽 × 高（像素）、分辨率（PPI）、像素长宽比 | 决定 tile 网格、内存上限、PSB 切换阈值 |
| 通道 | 颜色通道（RGB 3 / CMYK 4 / Lab 3 / Gray 1）+ Alpha 通道（= 选区/蒙版）+ 专色通道 | 通道数决定合成器的输入宽度；Alpha 通道与选区**同一实体** |
| 位深 | 1 / 8 / 16 / 32（float） | 32-bit 是所有调整不裁剪、HDR 输出的前提（`file-format-parity.md:24`） |
| 色彩模式 | Bitmap / Grayscale / Duotone / Indexed / RGB / CMYK / Lab / Multichannel | 模式决定可用功能子集（Indexed 无图层、Bitmap 无灰度） |

PSD 容器要能表达这四维的全部组合，但头部只有一个 26 字节的固定域（签名 4 + 版本 2 + 保留 6 + 通道数 2 + 高 4 + 宽 4 + 位深 2 + 模式 2，`知识`），其余全部落在块里（见 §11）。

### 2.2 为什么 PS 是"多文档多色彩模式"而不是"RAW 中心"

这是本报告对 LUMINA 最关键的一节。

| 问题 | Lightroom / LUMINA 的答案 | Photoshop 的答案 |
|---|---|---|
| 系统的一等公民是什么 | **一张照片**（RAW 文件 + 参数集） | **一个文档**（像素画布） |
| 参数存在哪里 | 外部（XMP sidecar / 目录数据库） | 内部（图层结构 + 调整图层 + 智能滤镜） |
| 源文件是否被记住 | 是，永久绑定，可随时重解码 | **否**。RAW 只是 31 种输入格式之一，打开后源文件即被遗忘（除非做成智能对象） |
| 是否存在"库" | 是，目录是产品的核心 | **没有**。只有"最近打开"和 Bridge（另一个程序） |
| 一张照片能否有多个版本 | 是（虚拟副本 + 快照 + 历史） | 只能靠 Layer Comps / 另存副本 / 智能对象拷贝 |
| 非破坏性能到哪一层 | 到**解码层**（WB/曝光/曲线全在 RAW 解码里） | 到**图层/滤镜层**；解码层只能靠 Camera Raw 智能滤镜勉强回退 |

**推断（knowledge + 佐证）**：PS 之所以不是 RAW 中心，是因为它的文档模型诞生在 RAW 工作流之前十余年，且必须同时服务印刷（CMYK）、合成（多来源）、3D/视频。把"来源"抽象成"打开/置入/智能对象"是它唯一能兼容全部场景的选择——代价就是**它天生没有"这张照片的原始参数"这个概念**。

证据侧写：Camera Raw 在 PS 里是**一个独立宿主（host）**，既能独立打开，也能作为**滤镜**挂在图层上（`file-format-parity.md:96-98` 记"Camera Raw as a PSD smart filter"只有部分参数被映射）。

> **对 LUMINA 的取舍**：不要试图把 PS 的"文档中心"整体搬过来。LUMINA 的竞争力恰恰是 PS 没有的那一层——**记住源文件与参数**。正确姿势是：**保留 RAW 中心作为真相层，把 PS 的图层/滤镜模型作为"渲染层"引入**（详见 §5、§14）。

---

## 3. 图层系统（最核心）

### 3.1 图层类型全谱

| 类型 | 内容本质 | 是否含像素 | PS 中的 PSD 承载（`architecture.md:386-395`） |
|---|---|---|---|
| 像素层（Pixel） | 位图 + 可选图层蒙版 + 矢量蒙版 | 是 | 图层记录 + 通道数据（RLE/ZIP） |
| 文字层（Type） | 一段文本 + 字符/段落属性 + 变换 + Warp | 否（延迟栅格化） | `TySh` + `Txt2` + `EngineData`（PostScript 式文本引擎数据） |
| 形状层（Shape） | 矢量路径 + 填充/描边属性 | 否 | `vmsk` / `vsms` / `vogk`（路径 + 矢量蒙版） |
| 调整层（Adjustment） | 一个纯参数节点（曲线/色阶/HSL…） | 否 | 各调整专用 key：`curv` `levl` `hue2` `blnc` `selc` `mixr` `grdm` `phfl` `brit` `thrs` `post` `nvrt` `expA` `vibA` `clrL` `blwh` |
| 填充层（Fill） | 纯色 / 渐变 / 图案 | 否 | `SoCo` `GdFl` `PtFl` / `Patt` |
| 智能对象（Smart Object） | **外部或内嵌的另一个文档** + 变换四边形 + Warp + 滤镜栈 | 否（缓存像素） | `SoLd`（新版）/ `PlLd`（CS2 旧版）/ `lnk2`（内嵌源） |
| 组（Group） | 子图层列表 + 穿透/隔离语义 + 组蒙版 | 否 | `lsct`（标识组/剪贴基底）+ `lyid`（稳定 ID） |
| 视频层（Video） | 帧序列 + 时间轴 | 是（帧） | 视频层专用块（本文不展开） |
| 3D 层（3D） | 场景图 | 否 | 3D 专用块（本文不展开） |

**结构性洞察**：真正"有像素"的只有像素层、视频层、以及智能对象的缓存。其余全是**参数节点**。PS 的整个图层栈本质上是一棵**异构节点树**，每个节点声明"我如何产生一层 RGBA"。这棵树 → 一个线性指令流（planner）→ 后端执行，是 PS 合成器的核心架构，`architecture.md:325-338` 对这条链路有清晰的反向描述（`PushGroup` / `BeginClipBase` / `DrawTiles` / `Adjust` / `Effect` / `Fill` 六类指令）。

### 3.2 混合模式全表

记 **a = backdrop（下层/背景）**，**b = source（本层/前景）**，均已归一化到 0–1，α 单独处理。PS 的模式分 6 组，共 **27 个**图层可用模式 + 组专用的 Pass Through（`architecture.md:570` 记 "all 27 blend modes"；分组与公式为 `知识`）。

| 组 | 模式 | 公式（分通道） | 备注 |
|---|---|---|---|
| **Normal** | Normal | `b` | 未预乘的 source-over |
| | Dissolve | `b`，α 按像素位置随机二值化 | 只对非整数 α 有意义 |
| **Darken** | Darken | `min(a,b)` | |
| | Multiply | `a·b` | |
| | Color Burn | `1 − (1−b)/a`（a=0 → 0） | |
| | Linear Burn | `a+b−1`，裁剪 | |
| | Darker Color | 比较整像素（不逐通道） | 非逐通道模式 |
| **Lighten** | Lighten | `max(a,b)` | |
| | Screen | `1−(1−a)(1−b)` | |
| | Color Dodge | `b/(1−a)`（a=1 → 1） | |
| | Linear Dodge (Add) | `a+b`，裁剪 | |
| | Lighter Color | 比较整像素 | 非逐通道 |
| **Contrast** | Overlay | `a≤0.5 ? 2ab : 1−2(1−a)(1−b)` | Hard Light 的参数互换 |
| | Soft Light | 分段式（W3C 近似） | **PS 的实现与 W3C 有微小差异**；PS 另有 legacy 合成开关会改变透明区结果（`知识`） |
| | Hard Light | `b≤0.5 ? 2ab : 1−2(1−a)(1−b)` | |
| | Vivid Light | 按 b 分段做 Burn/Dodge | |
| | Linear Light | `a + 2b − 1` | |
| | Pin Light | `b≤0.5 ? min(a,2b) : max(a,2b−1)` | |
| | Hard Mix | `(a+b ≥ 1) ? 1 : 0` | 二值化，边界敏感 |
| **Inversion** | Difference | `|a−b|` | |
| | Exclusion | `a+b−2ab` | |
| | Subtract | `b−a`，裁剪 | 方向与 Difference 不同，易写反 |
| | Divide | `b/a` | |
| **Component** | Hue / Saturation / Color / Luminosity | 在 HSL 空间重组分量 | **不可分离**，必须整像素处理；Luminosity 的 luma 权重是 PS 自有定义（`file-format-parity.md:3` 记 Gradient Map 用 W3C luminosity） |

三种特殊"模式"不在 27 之内但必须实现：

- **Pass Through（穿透）**：仅组可用，见 §3.6。
- **Clear / Behind**：画笔与图层的特殊合成模式（Clear 用 `~` 临时切换，`brush-parity.md:77`）。
- **Dissolve 的 α 处理**：`smart-filters.md:53-55` 记录 PhotoCraft 的 Dissolve 尚未做逐像素覆盖采样，说明这是一个容易漏的语义点。

### 3.3 Blend If（混合颜色带）

这是 PS 最被低估的机制，也是**"局部蒙版"最优雅的实现**。

结构（`知识`，参数形状与 `control-protocol.md:71` 的 Layer Style 参数可互证）：

```
Blend If:  [This Layer ▾]  ─┬─ 灰度条（4 个三角：黑起、黑止、白起、白止）
                            ├─ R 条
                            ├─ G 条
                            └─ B 条
           [Underlying Layer ▾] ── 同样 4 通道
```

每个通道 4 个手柄 → 上下界各有一个**可劈开的双三角**（split handle）。语义：对该通道取值落在 `[黑止, 白起]` 内的像素，本层完全可见；落在过渡带内按 smoothstep 线性衰减；落在带外完全透明。**它本质上是"按底层/本层的通道值动态生成 α"**，而不是真的有一张蒙版。

> 工程含义：Blend If 是**每个像素 8 次比较 + 4 次插值**，可以完全在合成器里做，不需要额外蒙版纹理。`gpu-composite-reuse.md:16` 记录了它被放到 GPU 上执行的约束——**只读本单元之前的 backdrop**，因此可以在栈前缀检查点处安全复用。这正是它便宜的原因，也是它容易做错的原因（读错 backdrop 就全错）。

### 3.4 不透明度 vs 填充不透明度

| 属性 | 作用于 | 公式（概念） | 典型误用 |
|---|---|---|---|
| Opacity（不透明度） | 图层的**最终结果**（含图层样式、含混合结果） | `out = mix(backdrop, blend(backdrop, layer), opacity)` | 调低后发现投影也变淡 |
| Fill（填充） | 只作用于图层的**像素内容**，图层样式仍按 100% 参与 | `out = mix(backdrop, blend(backdrop, layer·fill), opacity)`，样式用 opacity | 调低后发现投影不变淡 |

这个区分是 PS 用户判断"这个软件是不是真 PS"的第一个试金石，`architecture.md:336` 把 "fill vs opacity" 明确列为合成器必须唯一编码的 PS 语义之一。

### 3.5 剪贴蒙版（Clipping Mask）

- 语法：`图层 A` 作为**基底（base）**，其上若干层被剪贴到 A；A 的 α 成为整组结果的 α。
- 关键细节：被剪贴层的混合模式**作用于 A 的内容**，而不是作用于 A 下方的背景；整组结果再以 A 的混合模式与背景合成。
- 边界情况：如果 A 本身是**穿透组**且组内含调整层，PS 会把它渲染成"隔离"（`file-format-parity.md:189` 记录了这条被实测修正的语义，误差从 50% 像素降到 0）。
- 工程含义：剪贴单元（clipping unit）是**不可分割的合成原子**，检查点只能落在单元边界（`gpu-composite-reuse.md:10-14`）。

### 3.6 组的穿透模式

| 组类型 | 隔离性 | 内部混合模式看得见什么 | 性能 |
|---|---|---|---|
| Pass Through（默认） | **不隔离** | 看得见组**下方**的背景 | 快（少一次离屏） |
| Normal（及任何非 Pass Through） | 隔离 | 只看组内 | 慢（需离屏缓冲） |

Pass Through 的语义陷阱：组内一个 Multiply 层会作用到组外的背景上；一旦给组设置了任何混合模式或非 100% 不透明度，它就自动变成隔离。**这也是"为什么我的调整层突然不起作用"的头号原因**（用户在组上设了 opacity）。

### 3.7 图层样式（Layer Effects）参数结构

9 种效果，每个都是"参数快照 + 独立开关"，整体存为一个 `lfx2`/`lrFX` 描述符块（`architecture.md:391`）。

| 效果 | 核心参数（`知识`） |
|---|---|
| Drop Shadow | 混合模式 / 颜色 / 不透明度 / 角度 / 距离 / 扩展(Spread) / 大小(Size) / 等高线(Contour) / 杂色 / 图层挖空 |
| Inner Shadow | 同上，方向反转，多一个"源/中心" |
| Outer Glow | 混合模式 / 不透明度 / 杂色 / 颜色或渐变 / 方法(柔和·精确) / 扩展 / 大小 / 范围 / 抖动 |
| Inner Glow | 同上 + 源（边缘/中心） |
| Bevel & Emboss | 样式（外斜面/内斜面/浮雕/枕状浮雕/描边浮雕）/ 方法（平滑/雕刻清晰/雕刻柔和）/ 深度 / 方向（上/下）/ 大小 / 柔化 / 角度+高度 / **光泽等高线** / **高光与暗调的混合模式+不透明度+颜色** / **纹理**（图案/缩放/深度/反相/与图层链接） |
| Satin | 混合模式 / 颜色 / 不透明度 / 角度 / 距离 / 大小 / **等高线** / 反相 |
| Color Overlay | 颜色 / 混合模式 / 不透明度 |
| Gradient Overlay | 渐变 / 样式 / 角度 / 缩放 / 混合模式 / 不透明度 / 反相 / 与图层对齐 |
| Pattern Overlay | 图案 / 缩放 / 链接 / 混合模式 / 不透明度 |

**结构性洞察**：Bevel & Emboss 是唯一一个"用等高线把 2D 距离场映射成伪光照"的效果——它没有真 3D，只有一条用户可编辑的 contour 曲线把距离 → 亮度。这解释了为什么 PS 的斜面永远"看起来像画上去的"，也解释了为什么它便宜（纯查表）。

**Advanced Blending（高级混合）** 是与样式同级的另一组参数：Fill Opacity、通道 R/G/B 复选、Knockout（None/Shallow/Deep）、"将内部效果混合成组"、"将剪贴图层混合成组"、"透明形状图层"、"图层蒙版隐藏效果"、"矢量蒙版隐藏效果"。其中 **Knockout** 会从组结果中扣掉自身贡献，是唯一一个需要"反向合成"的语义，`architecture.md:336` 明确把它列为必须由 planner 统一编码的项。

### 3.8 锁定项

四把锁：锁定透明像素 / 锁定图像像素 / 锁定位置 / 锁定全部。前两者影响**写入路径**（画笔能不能画在不透明区），后两者影响**变换与移动路径**。它们不是渲染属性，是操作许可位——这一点在自研实现里最容易被错误地放进合成器。

---

## 4. 选区与蒙版模型

### 4.1 Alpha 通道即选区（同一实体的两面）

PS 里没有独立的"选区对象"。**选区 = 一个 8-bit 灰度通道**，只是暂时挂在"当前选区"这个槽位上：

- 选区 → 通道：Select › Save Selection；
- 通道 → 选区：Ctrl+单击通道 / Select › Load Selection；
- 快速蒙版（Quick Mask）：把选区临时变成一个可视化通道（红色 50% 叠加），画笔涂改的就是 α。

**工程含义（对 LUMINA 极其重要）**：

1. 蒙版不是"另一种东西"，它就是 α 通道；因此"图层蒙版""矢量蒙版""快速蒙版""Alpha 通道""选区"共享一套合成语义。
2. 这意味着**蒙版可以做混合模式、可以做羽化、可以被调整层作用**（`smart-filters.md:29-31` 记录了在蒙版/通道上按 Ctrl+I 时走的是"通道自身的调整语义"，而非当作图层处理）。
3. 因此自研实现里**不要**为"选区"和"蒙版"写两套数据结构——这是 PS 语义的自然结论，也是 `architecture.md:336` 把 mask 与 clip 归到同一层语义的原因。

通道数量限制：8-bit 文档可容纳多个 Alpha（PS 上限远高于实用值），16/32-bit 文档通道数受限（`知识`，具体数值随版本变化，不作为设计依据）。

### 4.2 Select and Mask 的 refine edge 参数集

| 参数 | 语义 | 算法性质（`知识`） |
|---|---|---|
| Radius | 边缘搜索半径 | 在半径内重新估计 α（局部前景/背景色估计） |
| Smart Radius | 半径自适应（毛发/硬边分别处理） | 按局部梯度调整半径 |
| Smooth | 平滑边缘 | 对 α 做保边平滑（类似 guided filter） |
| Feather | 羽化 | 高斯近似；`file-format-parity.md:3` 记"mask feathers blur with their exact variance"——说明它是**可被精确复现的高斯**，不是随便的 box blur |
| Contrast | 边缘对比（硬化） | 对 α 做 S 曲线，围绕 0.5 |
| Shift Edge | 收缩/扩展 | 对 α 做偏移（本质是阈值平移后重羽化） |
| Decontaminate Colors | 去除边缘色污染 | 用估计的前景/背景色重建边缘像素颜色 |
| Output To | 选区 / 图层蒙版 / 新图层 / 新图层带蒙版 / 新文档 / 新文档带蒙版 | 6 种输出，`target-app-parity.md:171` 记 PS 有 6 种 |

Select and Mask 是一个**工作区（workspace）**而不是对话框：有视图模式（洋葱皮/蚂蚁线/叠加/黑底/白底/灰底/黑白/在图层上/显示图层）、Refine Edge Brush、画笔、套索、快速选择、高质量预览开关。`gaps.md:73-79`（G7）把它列为"PS 是工作区、竞品只有对话框"的典型差距，估算 80–140 agent-hours。

### 4.3 羽化 / 扩展 / 收缩的算法性质

| 操作 | PS 的行为 | 可仿造性 |
|---|---|---|
| Feather | 高斯模糊 α；半径 R 与 σ 的关系是**固定的**（不是 R=σ） | 高，但必须校准 σ 系数 |
| Expand / Contract | 对二值化边缘做距离变换（形态学膨胀/腐蚀），**在阈值后的 α 上操作** | 高，但"先阈值再距离"的顺序错了就会得到不同的圆角 |
| Smooth | 保边平滑，非简单均值 | 中 |
| Border | 选区内缩一圈再相减 | 高 |

**陷阱**：Feather 与 Expand 的顺序不可交换（先羽化再扩展 ≠ 先扩展再羽化）。PS 的菜单顺序与内部顺序一致，这是可被行为实验验证的（`知识`）。

---

## 5. 调整与滤镜体系

### 5.1 三层模型

| 层 | 是否破坏像素 | 参数是否可重入 | 有无蒙版 | 有无混合模式 | 能否重排 |
|---|---|---|---|---|---|
| **破坏性滤镜**（Filter 菜单直接应用） | 是 | 否（只能 undo） | 否 | 否 | 否 |
| **调整层**（Layer › New Adjustment Layer） | 否 | 是（双击重开） | 是 | 是 | 是（图层顺序） |
| **智能滤镜**（Smart Filter） | 否 | 是（双击重开） | 是（共享蒙版） | 是（每个滤镜独立） | **是（滤镜栈内可拖动重排）** |

### 5.2 智能滤镜 = 参数可重入的滤镜节点栈（本章重点）

这是 PS 架构里最值得 LUMINA 抄的一个机制。`smart-filters.md:1-31` 用实现者的语言把它的全部语义写清楚了，逐条提炼：

**节点结构**

```
SmartFilterNode {
  filterId     // 滤镜身份（内建 / 插件 / Camera Raw）
  params       // 该滤镜的完整参数快照（无参数滤镜如 Invert 为 null）
  mask         // 共享一张滤镜蒙版（整栈共用，不是每滤镜一张）
  blendMode    // 该滤镜自己的混合模式，独立于图层
  opacity      // 该滤镜自己的不透明度，"1% 表示 0.01"
  enabled      // 每滤镜独立眼睛
}
```

**求值顺序**

- **UI 显示：最新在最上**；**引擎求值：从下往上**（`smart-filters.md:9-10`）。
- 这条"显示顺序与求值顺序相反"的规则极易写反，一旦写反，参数完全相同、结果完全不同。

**可重入（re-entrancy）的三个硬前提**

| 前提 | 说明 | 反例 |
|---|---|---|
| **确定性** | 同参数 + 同输入 → 同输出 | 用随机种子的颗粒滤镜必须固定种子 |
| **缓存键完整** | 缓存键 = (输入内容哈希, 参数, 蒙版, 混合, 顺序) | 只按 filterId 缓存 → 改参数不刷新 |
| **外部依赖可缺失** | 插件缺失时**跳过该节点并保留缓存像素**，不是报错、不是变空 | `plugins.md:30-34` 记录了这条：插件卸载后滤镜被跳过，缓存像素原样保留 |

**重入的失效规则**（`smart-filters.md:23-27`）——这是最容易被忽略的一段：

> 编辑器打开时会捕获"文档快照 + 版本号 + 图层 + 滤镜"四元组；如果期间发生**切换文档 / Undo/Redo / 滤镜被删除或重排 / 有未完成的文档任务**，再点确定必须**重新打开对话框**，而不能把旧参数写回已经变了的对象。

这等价于数据库的**乐观并发控制**（compare-and-swap）。LUMINA 的 XMP 写回也要面对同一个问题（外部修改冲突，错误码 1005，`架构设计-LUMINA-v1.0.0.md:242`），可以直接复用同一套语义。

**部分强度下的合成**（`smart-filters.md:47-52`）

- 滤镜不透明度 < 100% 时，在**预乘（premultiplied）空间**插值输入与输出 → 保留半透明覆盖；
- 非 Normal 模式且滤镜改变了 α 时，用 source-over 合成 → 保住"只在输入有像素处"和"只在输出有像素处"的两类像素；
- 这是 PS 在模糊边缘处理"半透明晕圈"的正确做法，自研时若在直通（straight）α 空间插值会出现黑边。

**Camera Raw 作为滤镜的含义**

- ACR 不是"另一个调整层"，而是一个**参数域极大的滤镜节点**：Process Version / 白平衡 / 曝光对比高光阴影 / 曲线 / HSL / 颜色分级 / 细节（锐化+降噪）/ 镜头校正 / 效果（颗粒+暗角）/ 校准 / 蒙版。
- 作为智能滤镜时，它保存的是**一份解码参数快照 + 当时的输入像素**，而不是原始 RAW 文件——所以你在 PS 里改 ACR 参数是在"重新处理已解码的 16-bit 数据"，不是重新解码 RAW（`知识`；`file-format-parity.md:96-98` 记其 PSD 映射只覆盖了 WB/Light/Presence/Curves/HSL/Grading/Detail/Grain/Vignette 一个子集，反向印证"参数是描述符化的"）。

> **对 LUMINA 的直接结论**：LUMINA 的调整栈应当实现成**同构的节点栈**，而 **RAW 解码参数就是栈底的第一个节点**（type = `camera-raw`）。这样：
> - Lightroom 式的"随时回改 RAW 参数"= 改栈底节点；
> - PS 式的"非破坏滤镜"= 在栈中追加节点；
> - 两者共用一套求值器、一套缓存、一套蒙版语义。
> 这是把"RAW 中心"和"图层中心"统一起来的**唯一不别扭的办法**（详见 §14）。

### 5.3 Filter Gallery 的 look 机制

Filter Gallery 不是 47 个独立滤镜，而是 **47 个"旧式滤镜链 + 参数快照"的组合预设**（数量 47 见 `target-app-parity.md:66`）。每个 look 内部是若干经典滤镜的固定串联（例如 Chrome = 干刷 + 塑料效果 + 铬黄 的链），每个环节带自己的参数（`知识`；`file-format-parity.md:3` 记 "Filter Gallery with Chrome" 被作为 PSD 智能滤镜映射的一个特例，说明整个 look 在 PSD 里是**一个带链式描述符的节点**）。

工程含义：Filter Gallery 的实现成本极低（一套小滤镜 + 一个 look 表），**但"像不像"取决于每个小滤镜的数学细节**。`target-app-parity.md:175` 记"Filter Gallery looks approximate"，说明它是典型的"表面积便宜、保真度昂贵"的功能。

### 5.4 调整层的两条硬约束（`知识`，被 parity 数据反复印证）

1. **多数调整必须在文档色彩空间里做，而不是 RGB 三通道独立做**：Hue/Saturation 要先转 HSL——`file-format-parity.md:3` 记 32-bit Hue/Saturation 的实现是"围绕 lightness 按饱和度因子缩放色度"，这就是在 HSL 里算的证据。
2. **32-bit 文档下调整不得裁剪到 [0,1]**：`file-format-parity.md:186` 记 Brightness/Contrast、Channel Mixer、Photo Filter、Black&White tint 都保留 >1 与 <0 的值。

---

## 6. 变换与内容感知

| 能力 | 技术性质 | PS 的参数维度 | 可仿造难度 |
|---|---|---|---|
| **Free Transform** | 仿射（平移/旋转/缩放/斜切）+ 8 控制点 | 参考点、H/V 缩放、旋转、H/V 斜切、插值方式 | 低 |
| **Warp（网格变形）** | 4×4 Bézier 网格（可细分），双三次重采样 | 网格点数、每个控制点的位置与切线；另有 Cylinder/Arch/Flag/Arc 等**预设弯曲** | 中（几何易、重采样质量难） |
| **Puppet Warp** | 在图层 α 的**三角网格**上，用图钉驱动形变（MLS / ARAP 类） | 图钉位置、图钉旋转、模式（刚性/正常/扭曲）、网格密度、扩展 | 中高 |
| **Perspective Warp** | 四边形网格 + "平面"约束，可自动拉直建筑 | 平面数、网格密度、自动/手动 | 高（要识别平面） |
| **Content-Aware Scale** | **缝切入（seam carving）**，按能量删/插像素列 | 保护通道（肤色/自定）、人像识别、比例 | 中高 |
| **Content-Aware Fill** | **基于块匹配的修复（patch-based synthesis）** | 采样区域（可涂）、适应（色/光/结构）、旋转适应、镜像、缩放、输出到新图层 | **高**（PS 的核心工艺之一） |
| **Content-Aware Move / Extend** | 抠出 → 移动 → 原位置修复 → 边缘融合 | 适应、结构 | 高 |

关键证据：

- Warp 在 PSD 里是**智能对象的一部分**：`file-format-parity.md:24` 记 "smart objects (SoLd/lnk2 with their warp: the bend presets, a custom mesh and, since #4112, Cylinder)" —— 说明 **warp 不是作用在像素上，而是记录在节点的变换属性里**，因此可反复重渲染。这与 §5.2 的"参数可重入"是同一套哲学。
- Content-Aware 的三件套（Fill / Move / Scale）在 parity 文档里被拆到两个地方：Fill 属于"缺工作区"（`gaps.md:73-79`），Scale 属于"已通变换框"（同处）。这说明 PS 把它们实现成**同一引擎的三个入口**（`知识`）。
- 建筑摄影最需要的是 **Perspective Warp + Upright（自动拉直）**，风光最需要的是 **Content-Aware Fill（去杂物）**。这两个的优先级远高于 Puppet Warp。

---

## 7. 画笔引擎

### 7.1 参数全谱（实测维度，`brush-parity.md:19-39`）

| 段（Brush Settings 的 12 个小节） | 参数 | 说明 |
|---|---|---|
| **Brush Tip Shape** | size、angle、roundness、hardness、spacing、flip X/Y | 笔尖的**静态**几何 |
| **Shape Dynamics** | size/angle/roundness 的 jitter、Control（off/pen pressure/tilt/stylus wheel/rotation/velocity/direction/initial direction）、minimum、flip jitter、brush projection | 笔尖的**动态**扰动 |
| **Scattering** | 双轴 scatter、count、count jitter | 一次采样分裂成多个 dab |
| **Texture** | pattern、scale、brightness、contrast、mode（height/texture）、depth、each tip、protect texture | 用图案调制 dab 的 α |
| **Dual Brush** | 第二个笔尖 + 其混合模式 | 两个笔尖相乘/相加 |
| **Color Dynamics** | FG/BG jitter、hue/sat/brightness jitter、purity、per tip | 每个 dab 换色 |
| **Transfer** | opacity jitter、flow jitter、wetness、mix + 各自 Control | α 的随机化 |
| **Brush Pose** | tilt X/Y、rotation、pressure overrides | 覆盖真实笔输入 |
| **杂项** | Noise、Wet Edges、Build-up（喷枪式累积）、Smoothing（amount / Pulled String / Stroke Catch-up / Catch-up on End / Adjust for Zoom） | 只有 Smoothing 是"时间维度"的参数 |

压感映射：PS 的每个 Control 都支持**一条曲线**（不只是线性），且"大小/不透明度/流量"各自独立取曲线（`brush-parity.md:46-47` 记 PhotoCraft 只有全局曲线，per-control 曲线为 missing）。这是画笔"手感"差异的主要来源之一。

### 7.2 各画笔工具的差异（不是"都是画笔"）

| 工具 | 与画笔的根本差异 | 状态（`brush-parity.md:91-106`） |
|---|---|---|
| Brush / Pencil | Pencil 无抗锯齿（aliased），所以 1px 锐利 | done |
| Eraser | 目标 α 的反向混合；Background Eraser 只擦"与采样色相近"的像素 | done |
| Clone Stamp | 从**源点**取样并复制；aligned 模式让源随笔移动 | 部分（源尺寸链接/Alt 目标光标缺） |
| Pattern Stamp | 用图案代替颜色，impressionist 模式按 dab 随机取图案位置 | done |
| **Mixer Brush** | **有"湿"状态**：笔尖携带的颜料量（Wet/Load/Mix/Flow）+ 画布残余颜料的混合 | 部分（拖动时无实时结果） |
| History Brush | 从历史状态取样（时间维度，不是空间维度） | done（Art History Brush 缺） |
| Healing / Spot Healing | 克隆纹理 + 传递**光照梯度**（不是复制颜色） | 部分（平坦区颗粒不匹配） |
| Blur / Sharpen / Smudge | 对已有像素做卷积/位移，不产生新像素 | done |
| Dodge / Burn / Sponge | 只改亮度/饱和度，不改色相 | done |

### 7.3 `.abr` 笔刷格式承载什么

`.abr` 是**笔尖 + 预设**的容器（`brush-parity.md:111` 记 v1/v2/v6 与 v6/v12 两种实现说法；`file-format-parity.md:127` 记 v6/v12）：

1. **笔尖形状**：圆形（只存 hardness/roundness，运行时合成）或**位图笔尖**（灰度 α 图）；
2. **动态参数**：Shape Dynamics / Scattering / Texture / Dual Brush / Color Dynamics / Transfer 的完整参数与各自的 Control 绑定；
3. **工具设置**：模式、不透明度、流量、喷枪、平滑等；
4. **纹理引用**：图案名（图案本身在 `.pat` 里，不内嵌）。

工程含义：`.abr` 的价值是**参数集而非像素**（大多数笔尖是程序化的）。因此"支持 .abr"= "支持这套参数模型的序列化"，而不是"解析一堆图片"。`brush-parity.md:38` 记 "Erodible / Airbrush / Bristle tips" 只到 partial，因为那三种是**程序化毛发/磨损模型**，参数更多。

---

## 8. 文字与矢量

### 8.1 文字

| 面板 | 参数 | 备注 |
|---|---|---|
| **Character** | 字体、字重/样式、字号、行距（leading）、字距（tracking）、字偶（kerning：手动/光学/度量）、水平/垂直缩放、基线偏移、仿粗/仿斜、全部大写/小型大写、上标/下标、下划线/删除线、语言、**抗锯齿方式**（无/锐利/清晰/强/平滑）、颜色 | 抗锯齿方式是**渲染器参数**，不是字体属性 |
| **Paragraph** | 对齐（7 种）、左/右缩进、首行缩进、段前/段后、避头尾、连字符、对齐设置（单词/字母/字形缩放、自动行距）、**书写方向**、**Composer**（单行/多行） | 多行 Composer 是 Knuth 式全局断行，PS 独有 |
| **OpenType** | 标准连字、自由连字、分数、序数、花体、标题、旧式数字、表格数字、风格集、位置形式 | 需要字体本身支持 |
| **文字引擎数据** | PSD 里的 `TySh`（变换 + 文本）+ `Txt2`（段落/字符运行）+ `EngineData`（PostScript 式排版程序） | `architecture.md:392-394`：`engine_data.rs` 是"text EngineData (PostScript-like) parser+writer" |

**关键洞察**：PS 的文字层**从不栅格化存储**——它存的是"文本 + 属性 + 变换 + warp"，每次显示/导出时重新排版。所以：
- 文字层是**参数节点**，与调整层同构；
- 跨机器打开时字体缺失会导致重排（这是 PSD 交换的经典痛点）；
- `target-app-parity.md:176` 记竞品 Type 只有 40% ready，且"type on a path 只是刚性摆放字形"——说明**路径文字需要真正的基线弯曲 + 字形旋转 + 沿路径的度量推进**，不是把字符摆上去。

### 8.2 矢量

| 概念 | PS 的表达 |
|---|---|
| 路径 | 锚点 + 方向手柄（Bézier），开放或闭合；存于 `vmsk`/`vsms` |
| 钢笔工具族 | Pen / Freeform Pen / Curvature Pen / Add Anchor / Delete Anchor / Convert Point（`ui-parity.md:23-52` 记 68 工具里 Pen 族占 6 个） |
| 形状层 | 路径 + 填充（纯色/渐变/图案）+ 描边（宽度/对齐/端点/虚线）+ **Density / Feather**（`target-app-parity.md:177` 记形状可带 Density 与 Feather，且"作为填充的软矢量蒙版"） |
| **路径运算** | 合并形状 / 减去顶层 / 与形状区域相交 / 排除重叠（布尔运算，`vogk` 块记录运算类型）；另有"对齐/分布/组合" |
| **矢量蒙版** | 路径直接作为图层蒙版（无限分辨率，边缘永远是锐利的） |

**结构性洞察**：形状层的 Density/Feather 说明 PS 把矢量也当成"可以做软边的填充"，即矢量与像素在**同一套 α 语义**下统一。这与 §4.1 的"选区=α"是同一条设计主线的第三次出现（选区、蒙版、矢量、通道，全是一个 α）。

---

## 9. 色彩管理

| 项 | PS 的行为 | 落地要点 |
|---|---|---|
| 工作空间 | RGB（sRGB / Adobe RGB / ProPhoto / 自定义）、CMYK、Gray、Spot | 文档打开时若带 ICC 则**保留**，否则按策略（关/保留/转换/询问） |
| ICC 承载 | PSD 的 Image Resource **1039** 存 ICC；**1060** 存 XMP；**1058** 存 EXIF；**1005** 存分辨率（`architecture.md:391-393`） | 导出必须写 1039，否则收到的人看到偏色 |
| 转换意图 | 感知 / 相对比色 / 饱和度 / 绝对比色 + **黑场补偿（BPC）** + 仿色 | BPC 决定暗部是否发灰 |
| 软打样（Soft Proof） | 选择目标 profile + 渲染意图 + **模拟纸白 / 模拟黑墨** + **保留颜色值（Preserve Color Numbers）** | "保留颜色值"= 不转换，只改显示——这是 CMYK 校对的必需开关 |
| 位深 | 8 / 16 / 32（float） | 32-bit 是 HDR 与"调整不裁剪"的前提 |
| HDR / EDR 输出 | 支持（安装包内有 "Precise Color Management for HDR Display" 与 EDR 字符串） | `hardware-parity.md:7` 记录该字符串；`:44` 记竞品 HDR 输出 missing |
| 显示器 profile | 按显示器自动识别 | `hardware-parity.md:40-42`：竞品只在 macOS 启动时读一次 |

**对 LUMINA 的硬约束**：`PRD-LUMINA-v1.2.0.md:159` 已明确"引擎无 LCMS2，AdobeRGB 为线性矩阵近似"——这意味着在接 LCMS2 之前，**任何 PSD 导出（F6）与软打样（F7）的精度承诺都不成立**。这一条不是 PS 逆向的结论，而是逆向对比后暴露的自有短板。

---

## 10. 性能架构

| 机制 | PS 的做法（estimate + 知识） | 可复用结论 |
|---|---|---|
| **分块（tile）** | 画布切成固定块，按需驻留/落盘；超过内存上限溢出到暂存盘（Scratch Disk） | 必须做，且块大小要能容纳"超过 GPU 纹理上限"的文档 |
| **脏矩形（dirty rect）** | 只重算受影响区域；画笔每帧产生一个小矩形 | `gpu-composite-reuse.md:40-44` 记一条关键工程细节：**脏矩形要映射到"整块网格"再缓存**，否则画笔每帧的精确矩形永远不命中缓存 |
| **合成器分工** | **图层合成主要在 CPU**，GPU 用于画布变换、部分滤镜与效果预览 | `hardware-parity.md:21` 记这条（estimate）。含义：**不要为合成器过度押注 GPU**，CPU 参考实现是保真度的锚 |
| **栈前缀复用** | 在一段"完整栈单元"（base + 其全部剪贴层）之后设检查点，缓存已合成的下半部分 | `gpu-composite-reuse.md:10-30`：检查点**不能落在组内/剪贴单元内/调整程序内**；缓存上限 512 MiB；失效判定要**保守**（含色彩模式、位深、profile、全局光、图案） |
| **失效策略** | 保守比对整个图层结构（含蒙版、子节点、效果、派生缓存、像素面） | 宁可多失效，不可少失效——"便宜的错误"胜过"错误的画面" |
| **智能对象延迟渲染** | 只在需要时把内嵌文档渲染成缓存；改内容才重渲 | 这是"智能对象为什么慢"和"为什么能非破坏"的同一答案 |
| **历史（History）** | 状态快照 + 线性步进，可设上限 | 与 tile 的 COW（copy-on-write）共享是关键，否则 150 层会爆内存 |

`target-app-parity.md:81` 给出了这条路线失败的量化代价："3 of 25 budgeted scenarios meet budget；150 层 nudge 崩溃；15000×10000 16-bit 图层移动 p50 2.7 s"。**这说明性能不是"优化"，是架构选择**——一开始没做 tile + 脏矩形 + 前缀复用，后面补不上。

---

## 11. 文件格式与生态

### 11.1 PSD / PSB 结构（逆向自公开格式 + `architecture.md:386-395` 的模块切分）

```
Header (26 B)          signature '8BPS', version 1(PSD)/2(PSB), channels, height, width, depth, color mode
Color Mode Data        Indexed 调色板 / Duotone 参数
Image Resources        1005 分辨率 · 1036 缩略图 · 1039 ICC · 1058 EXIF · 1060 XMP · 切片 · 参考线
Layer & Mask Info      ┬ Layer Info: 层数（PSB 用 8 字节）、每条 Layer Record
                       │   ├ 矩形、通道数、各通道长度
                       │   ├ blend signature '8BIM' + blend key（'norm'/'mul '/…）
                       │   ├ opacity / clipping / flags（可见/锁定/位深）
                       │   ├ mask data（位置、默认色、flags）
                       │   ├ blending ranges（灰/红/绿/蓝各 4 个数 = Blend If 的存储形态）
                       │   └ name（Pascal）+ 'luni'（Unicode 名）+ 'lyid'（稳定 ID）
                       ├ Channel Image Data: 每通道 raw | RLE(PackBits) | ZIP | ZIP+predict
                       └ 附加层信息块（全局 / 逐层）：
                           luni lyid lsct  lfx2/lrFX(效果)  TySh Txt2(文字)
                           SoLd PlLd lnk2(智能对象)  vmsk vsms vogk(矢量)
                           curv levl hue2 blnc selc mixr grdm phfl brit thrs post nvrt expA vibA clrL blwh(调整)
                           SoCo GdFl PtFl Patt(填充)  FEid(智能滤镜蒙版)  filterFX(滤镜栈)
                           layer.xmpMetadata(逐层 XMP)
Image Data             合并预览（PS 自己的合成结果）← ★ 免费 oracle
```

三条对 LUMINA 最有用的观察：

1. **合并预览是格式强制的**，不是可选项。这带来两个后果：(a) 任何写入器必须能产出一张正确的合成图（否则 PS 打开会看到错的缩略图/错误预览）；(b) 反向利用它——**拿它当自研合成器的免费测试集**（`architecture.md:394-396`）。这是本次逆向里"投入产出比最高"的一条方法论。
2. **未知块必须原样回写**。`architecture.md:399` 记 "Unknown or unsupported tagged blocks are kept as raw bytes and re-emitted unchanged"——这是 PSD 往返不破坏 3D/视频等未实现功能的唯一办法，也是"不产出伪 PSD"的技术前提。
3. **PSB 只是把长度字段从 4 字节换成 8 字节**（`file-format-parity.md:25` 记 2 GB 自动切 PSB、4.4 GB PSB 可开）。实现上应一套代码两个模式，而不是两套。

### 11.2 关键格式的支持方向（实测，`file-format-parity.md:24-49`）

| 格式 | PS | 关键约束 |
|---|---|---|
| PSD / PSB | R/W | 1/8/16/32-bit；8 种色彩模式；图层/组/蒙版/矢量蒙版/调整/填充/效果/文字/形状/智能对象/智能滤镜/切片/复合/高级混合/逐层 XMP |
| TIFF / JPEG / PNG | R/W | TIFF 8/16/32f + BigTIFF + **分层 TIFF**（含混合字节序文字块）；JPEG **quality 0–12**（非 0–100）+ Baseline/Optimized/Progressive；PNG 8/16-bit，APNG 只取首帧 |
| WebP / AVIF / HEIF | R/W | WebP 有损/无损；AVIF 竞品读缺失；HEIF 只读 |
| PDF / EPS / DCS | R/W | PS 的 PDF 是**真矢量/文字 PDF**，可多页、可保留图层；EPS/DCS 是印刷前端专属，竞品不支持 |
| EXR / HDR / DPX / Cineon / SVG | R/W | 影视/HDRI 链路；SVG 在 PS 侧只**栅格化**（竞品反而更强，可编辑形状层） |
| Camera Raw | R | 经 ACR 18.7，注册 21 种扩展名（含 CR3；竞品 CR3 只识别不解码） |

### 11.3 `.8BF` 滤镜插件与 PiPL

- `.8BF` 是 PS 的原生插件二进制（Windows `.8BF` / macOS 同结构）。它靠**资源段里的 `PiPL`** 自我描述：插件种类（Filter / Import / Export / Selection / Color Picker / Extension）、名称、分类、参数块、以及（对格式插件）**读/写标志位**。
- 这正是 `file-format-parity.md:6` 能"不启动 PS 就列出 31 种格式"的原因：**PiPL 的读/写标志是机器可读的能力声明**。
- 宿主侧要提供的是：像素缓冲区（`FilterRecord`）、进度回调、参数对话框回调、以及"是否在预览/最终"的标志。插件不直接碰文档，只碰一块 RGBA 内存。
- PS 另有 **UXP（JS，现代面板）** 与 **ExtendScript（JSX，旧脚本）** 两条脚本线。`target-app-parity.md:69` 记竞品生态得分仅 20%，`gaps.md:37`（G16）估 250–450 agent-hours + 人工——**这是典型的"成本远超收益"区**（见 §13）。

### 11.4 Actions / 预设格式

| 格式 | 内容 | 备注 |
|---|---|---|
| `.atn` | ActionSet → Action → `ActionReference` + `ActionDescriptor` 事件流 | **录制下来的命令流**；`file-format-parity.md:134` 记竞品映射 112 个事件、其余原样保留 |
| `.kys` / `.mnu` | 快捷键表（67 工具 + 107 命令）/ 菜单树 | 工具与菜单清单的权威来源；`.mnu` 在本轮取证中**未解析**（`target-app-parity.md:53-55` 自承 27.x 新增项可能漏） |
| `.abr` / `.grd` / `.pat` / `.asl` / `.shc` | 画笔 / 渐变 / 图案 / 图层样式 / 等高线 | 见 §7.3；都是"可重放的参数快照" |
| `.aco` / `.ase` / `.act` / `.cube` / `.3dl` / `.acv` / `.alv` | 色板 / LUT / 曲线与色阶预设 | 与调色链路直接对接 |
| `.dng` 内的 XMP | ACR 参数 | **Lightroom 与 PS 共用的真相层**——也是 LUMINA 最该兼容的一处 |

**结构性观察**：PS 的"生态格式"分两类——**参数型**（.abr/.grd/.pat/.asl/.cube/.atn，都是"可重放的参数快照"）与**资产型**（图案/字体/纹理，是二进制资源）。前者与 §5.2 的节点栈同构，后者不可参数化。LUMINA 若要兼容生态，应优先做**参数型**。

---

## 12. 逆向方法论提炼

"不读源码也能推断商业软件行为"的可复用手段，按可靠性排序：

| # | 手段 | 能推断什么 | 可靠性 | 局限 |
|---|---|---|---|---|
| 1 | **PSD 合并预览当 oracle** | 图层/混合/调整的**数值真值**（逐像素） | ★★★★★ | 只对"PS 自己写出的文件"有效；无法验证 PS 不写的功能 |
| 2 | **安装包目录结构 + Required 插件清单** | 模块切分、插件边界（85 个滤镜条目、12 个格式插件） | ★★★★☆ | 只见"存在"，不见"实现" |
| 3 | **`Info.plist` 的 `CFBundleDocumentTypes`** | 支持的格式与**角色**（Editor/Viewer） | ★★★★☆ | 只说"能开"，不说保真度 |
| 4 | **`.8BF` 的 `PiPL` 资源** | 滤镜/格式插件的**种类与读/写标志** | ★★★★☆ | 参数语义仍需行为实验 |
| 5 | **本地化字符串（`.lproj` / `Locales/`）** | **完整 UI 词汇表 = 功能清单**（含未文档化的选项名） | ★★★★☆ | 只有名字，没有行为；翻译还会暴露语义（如意大利语把 Screen 分成 Scherma/Schermo） |
| 6 | **`Default Keyboard Shortcuts.kys`** | 工具清单 + 默许键位（67 工具 / 107 命令） | ★★★★☆ | 版本间会漏；不含无快捷键命令 |
| 7 | **菜单资源 `.mnu`** | 完整菜单树（631 项的权威来源） | ★★★★☆ | 动态菜单（生成式/AI/云）不在静态资源里 |
| 8 | **录像/录制 Action 后 dump `ActionDescriptor`** | 参数**结构**与默认值（最接近"schema"的东西） | ★★★★☆ | 需要能录制；每版本可能变 |
| 9 | **行为实验（参数网格 + 差分截图）** | 算法细节（Soft Light 曲线、羽化 σ 系数、Expand 阈值顺序） | ★★★★☆ | 极贵；只能覆盖被实验到的点 |
| 10 | **主二进制字符串扫描** | 特性存在性（HDR/EDR、Metal、Touch Bar、笔特性） | ★★★☆☆ | 字符串在 ≠ 功能可用 |
| 11 | **第三方解析器对照（psd-tools 等）** | 文件结构理解的交叉验证 | ★★★☆☆ | 第三方也会错，只能当"第二意见" |
| 12 | **官方文档 / 发行说明** | 语义解释、版本差异 | ★★★☆☆ | 常滞后、常含糊、几乎不给公式 |

**最被低估**：第 1 条。多数人把 PSD 当"要解析的格式"，它同时是**一份覆盖全部图层语义的免费测试集**——合成器能复现 PS 写出的合并图，就"行为等价"了。refs 把 "Photoshop-authored oracle 190/258" 当最硬指标（`file-format-parity.md:3`）正是这个道理。
**最被高估**：第 10 条。字符串扫描给出"有这特性"，给不出"何时启用、精度如何"。

---

## 13. 对 LUMINA 的仿造建议

**用户画像（本次结论的唯一标尺）**：风光 / 建筑摄影师。特征是——
批量（一次几百到几千张）、重色彩与宽容度、重局部调整（天空/地面/建筑立面）、需要交付（网络图 + 打印/画册）、**极少做合成/文字/矢量/印刷分色**。

### 13.1 必须抄（决定"能不能替代 PS"）

| # | 能力 | 为什么是必须 | 粗估工作量（人日，估计） |
|---|---|---|---|
| 1 | **图层 + 图层蒙版 + 混合模式子集**（Normal/Multiply/Screen/Overlay/Soft Light/Luminosity + Pass Through） | 没有蒙版就没有局部调整；混合模式是"加减光"的语言 | 15–25 |
| 2 | **调整图层 + 剪贴** | 非破坏的骨架；摄影师 90% 的修图是调整而非像素绘制 | 12–20 |
| 3 | **局部蒙版 + 范围蒙版（亮度/颜色）+ Blend If** | 天空/地面的自然过渡靠它；Blend If 是零成本高质量蒙版的捷径 | 8–12 |
| 4 | **非破坏滤镜节点栈**（= §5.2 智能滤镜模型，先只做 Camera Raw 一个节点类型） | 这是把 RAW 参数与图层滤镜统一的关键；不做它，两套模型永远打架 | 20–30 |
| 5 | **选区精修**（快速选择 + 天空/主体 + 羽化/收缩/扩展 + 输出到蒙版） | 天空替换、地平线处理的入口 | 15–25 |
| 6 | **修复 + 内容感知填充 + 自由变换/裁剪/透视校正** | 去杂物、拉直建筑、校正歪地平线——建筑摄影刚需 | 20–30 |
| 7 | **真 ICC 色彩管理 + 软打样** | 打印与跨设备一致性的前提；`PRD-LUMINA-v1.2.0.md:159` 已列为基础件 | 10–15 |
| 8 | **PSD 带图层导出 + 往返**（v1.2.0 F6 已排） | 与修图师/设计师协作的唯一接口 | 15–25（v1.2.0 只做最小写入器） |
| 9 | **历史 / 快照 / 虚拟副本** | 摄影师的"试错"行为需要这个；PS 只有 History，LR 更强 | 8–12 |
| 10 | **导出预设 / 批量 / 水印** | LUMINA 已有，需与图层模型对齐 | 5–8 |

**小计：约 130–200 人日（估计，含测试；不含 §13.3 的红线项）。**
注：v1.2.0 实际只排了 #7（F7）与 #8（F6）两项，约占其中 25–40 人日。其余是 v1.3+ 的账。

### 13.2 可以晚抄（专业设计师才需要）

| 能力 | 为什么可以晚 | 粗估（人日，估计） |
|---|---|---|
| 完整文字系统（Character/Paragraph/OpenType/路径文字/区域文字） | 风光/建筑摄影几乎不加字；加也是水印级 | 20–35 |
| 矢量形状 + 钢笔 + 路径运算 | 同上；构图标注可用轻量方案 | 15–25 |
| 图层样式全谱（9 种效果 + 等高线 + 纹理） | 设计师做 UI/海报才用；摄影修图几乎不用 | 15–25 |
| 智能对象（嵌入/链接/替换内容/变换固化） | 摄影师更需要的是"RAW 图层"，不是通用智能对象 | 20–30 |
| Warp / Puppet Warp / Perspective Warp / CA Scale | 建筑需要 Perspective，但可用"镜头校正 + 裁剪拉直"覆盖 80% | 25–40 |
| Filter Gallery 的 47 个 look | 是"玩"的功能，非工作流 | 8–12 |
| Actions `.atn` + 批处理编排 | LUMINA 已有预设/批量，`.atn` 兼容是加分项 | 15–25 |
| 画笔引擎全谱（dual brush / texture / 全套 dynamics / .abr） | 摄影师用画笔主要是蒙版涂抹与局部减淡加深 | 25–40 |
| 通道面板 + 快速蒙版 + 专色通道 | 高级用法；先用"蒙版即通道"的内部模型顶上 | 10–15 |
| 16/32-bit 全链路 | 需要（风光 HDR），但可先 16-bit 全覆盖，32-bit 仅 EXR/HDR 输出路径 | 10–20 |

### 13.3 坚决不抄（成本远超收益）

| 能力 | 不抄的理由 | 若强行做的代价（人日，估计） |
|---|---|---|
| **3D 层 / 3D 面板 / 材质** | 与摄影工作流零交集；PS 自己都弱化了 | 60–120+ |
| **视频层 / 时间轴 / 动画** | 目标用户不拍视频；LUMINA 的延时是"序列合成"不是"视频剪辑" | 40–80 |
| **印刷前端**（CMYK 分色、陷印、专色、拼版、PDF 多页/演示） | 交付走"PDF 给印厂"即可；分色是印厂的事。**注意：软打样要抄，分色不要抄** | 50–100 |
| **Bitmap / Duotone / Indexed / Multichannel 完整模式** | 只有特殊印刷/科研需要；读进来能显示即可，不必可编辑 | 15–25 |
| **`.8BF` / CEP / UXP 宿主** | 需要 unsafe FFI 跑任意机器码，是**安全与法务双重风险**；生态收益对摄影师极低 | 60–120 + 法务评审 |
| **ExtendScript(JSX) 兼容** | 历史包袱；现代需求用自家脚本/CLI 覆盖 | 40–80 |
| **Adobe 云文档 / Libraries / C2PA 内容凭证** | 依赖 Adobe 账号体系，且属"分发他人资产"邻域 | 30–60 |
| **艺术笔刷 / 历史笔刷 / 高级对称（曼陀罗/螺旋）** | 绘画类功能，与摄影修图无关 | 10–20 |

**红线复述**：不涉及 Adobe 的 DRM/激活/授权绕过；不复制/分发 Adobe 资产（DCP/LCP 相机配置文件、预设、模型权重）。本文所有"抄"都是**行为与架构层面的仿造**，不含资产搬运。

---

## 14. 与 LUMINA 现有架构的冲突点

### 14.1 两套模型的对照

| 维度 | LUMINA（现状） | Photoshop |
|---|---|---|
| 一等公民 | 一张照片（RAW + 参数） | 一个文档（画布） |
| 参数真相源 | **XMP sidecar**（`crs:` + `lumina:`），逐字节保留式写回（`架构设计-LUMINA-v1.0.0.md:521`） | **PSD 图层结构**（`lumina:` 无对应物） |
| 调整模型 | **线性栈**（`lumina:AdjustmentStack`，`架构设计-LUMINA-v1.0.0.md:789`） | **树**（组/剪贴/多基底/穿透） |
| 蒙版 | 局部蒙版（范围蒙版：亮度/颜色） | 图层蒙版（α）+ 矢量蒙版 + Blend If |
| 库 | 有（目录库是核心） | 无 |
| 派生关系 | 需要（全景/景深堆栈的源引用，`PRD-LUMINA-v1.2.0.md:205`） | 无（智能对象是"嵌入副本"） |

### 14.2 五个结构性矛盾与处置建议

| # | 矛盾 | 具体表现 | 建议处置 |
|---|---|---|---|
| **C1** | **参数存 XMP 还是存 PSD 图层？** | LUMINA 的调整栈在 XMP；PS 只认图层。若两边都写 → 双真相源，必然不一致 | **XMP 是唯一真相源，PSD 是派生产物（单向导出）**。PSD 里同时写两份：(a) 图层结构（给人看/给人改）；(b) 逐层 `layer.xmpMetadata` 里塞一份 `lumina:` 快照（`file-format-parity.md:24` 证实 PSD 支持逐层 XMP）。导入时若发现该快照 → 可逆；否则 → 降级为像素层并**显式告知** |
| **C2** | **线性栈 → 树的映射失真** | LUMINA 的"范围蒙版"在 PS 里没有对应；转成图层蒙版就变成像素，失去可编辑性 | 规则化降级：(a) 范围蒙版 → 烘焙成图层蒙版的灰度图（保真但丢参数）；(b) 若范围仅由亮度决定 → 用 **Blend If** 表达（保参数，但有精度差）；(c) 两者都不满足 → 强制栅格化并标注 `degraded` |
| **C3** | **一张照片 vs 一个文档：合成产物算什么** | 全景/景深堆栈产出"新图像"，它既不是原片，也不该是一个独立文档（否则库里出现无名文件） | 定义为**派生文档（derived asset）**：库内一条记录 + `lumina:CompositeType` + `lumina:CompositeSourceRefs`（`PRD-LUMINA-v1.2.0.md:205`），编辑参数仍存 XMP，源片只读 |
| **C4** | **位深与色彩空间不统一** | LUMINA 内部管线以 RAW 解码为起点（16-bit 线性/ProPhoto 类），PS 文档有 8/16/32 三档 + 8 种模式 | 内部统一 **16-bit float + 工作空间**；导出时按目标降/升位深。**禁止**在 8-bit 上做调整栈（这是 PS 用户能一眼看出的差别） |
| **C5** | **往返（round-trip）不可逆** | PS 用户改了 PSD 后，LUMINA 无法把改动回写到 XMP（图层可能被合并/重命名/栅格化） | 只承诺**单向导出**；若做往返，用 `lyid`（稳定 ID，`architecture.md:391`）+ 层名双键匹配，匹配失败即**以 PSD 为准、XMP 作废并提示**。不做静默合并 |

### 14.3 一段话结论

> PS 的图层模型和 LUMINA 的 RAW 模型不是"两个要合并的模型"，而是**两个不同抽象层的模型**。PS 的文档是"渲染结果的容器"，LUMINA 的照片是"渲染参数的容器"。正确的合并方式不是二选一，而是**把 PS 的节点栈语义（§5.2）搬到 LUMINA 的参数侧**：让 `camera-raw` 成为栈底节点，让调整/滤镜成为后续节点，让蒙版成为节点的属性而非独立对象。这样只存在**一个**求值器、**一份**缓存、**一套**蒙版语义，PSD 则退化为"这个栈的一次序列化"。

---

## 附录 A：证据索引

`photocraft/docs/` 下：`target-app-parity.md`（版本与安装包实测清单、631/68/35/31 四项分母）、`gaps.md`（G1–G17 + agent-hour 估算，§13 的标尺）、`file-format-parity.md`（格式矩阵 + PiPL/Info.plist 取证）、`architecture.md`（planner 指令集、PSD 块级布局、CPU/GPU 对等测试）、`smart-filters.md`（智能滤镜全部语义）、`brush-parity.md`、`ui-parity.md`、`gpu-composite-reuse.md`、`hardware-parity.md`、`plugins.md`、`control-protocol.md`（Layer Style / Blend If / 混合模式名的参数形状）。
LUMINA 侧：`docs/架构设计-LUMINA-v1.0.0.md`（字节保留式 XMP、`lumina:AdjustmentStack`、错误码 1005）、`docs/PRD-LUMINA-v1.2.0.md`（F6 范围、F7 与 LCMS2 更正、XMP 命名空间扩展）。

## 附录 B：本文未覆盖的内容（明确声明边界）

1. PS 的 3D、视频、Libraries、生成式 AI（Firefly）内部实现——与目标用户无关，且无可靠外部证据。
2. PS 的激活/授权/DRM 机制——不在本文范围。
3. Adobe 自有资产格式的内部结构（DCP/LCP 相机与镜头配置文件、Neural Filters 模型权重）——涉及分发边界，只做能力级描述。
4. 精确算法常数（如 Soft Light 的分段阈值、Feather 的 σ 系数）——需行为实验才能标定，本文只给"可被精确复现"这一性质判断。
