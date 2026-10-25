# R2 · Adobe Lightroom 架构与功能逆向社会报告

> **输入对象**：Adobe Lightroom Classic 15.5（2026-08）+ Lightroom 9.6（云版桌面）
> **用途**：LUMINA v1.2.0「全面对标 PS + Lightroom」的 LR 侧输入
> **日期**：2026-10-24 ｜ **作者**：re-lightroom（逆向工程分析师）
> **与 PS 报告的分工**：本文只讲 LR 侧——库/目录/管线/非破坏性/蒙版/交付，**不重复** PS 报告的图层、选区、混合模式、画笔引擎。凡涉及 PS 的交叉点只做一句指路。

---

## 0. 引用代号与证据分级

| 代号 | 实体 | 说明 |
|---|---|---|
| `LR-INV` | `reports/lr-catalog-investigation-report.md` | **本团队一手实测**：对用户真实库（18,192 张 / ≈512 GB / 92.2% 松下 S5M2 RW2）的 `.lrcat` 静态逆向。原始路径在工作区 `c:/Users/zj/WorkBuddy/2026-10-21-20-30-19/reports/`（任务书给出的 `D:/LUMINA/reports/` 下该文件不存在，以工作区这份为准） |
| `REF` | `D:/work/project/pycharm/p/refs/lightcraft/docs/` | 第三方对标实现 LightCraft 的公开对标文档；其中"实测"是针对 Adobe 安装包与真实文件的测量，不是我们的测量 |
| `LUM` | `D:/LUMINA/` 源码 | 我们自己的现状，用于 §14 |

**证据级四级**，全文每个结论后标一个：【实】=我们自己的文件/数据库实测（`LR-INV`）或 LUMINA 源码事实（可复现）；【测】=`REF` 文档中的测量数字（ΔE、ms、文件计数，他方测、未见原始数据）；【献】=`REF` 文档的陈述性结论（非测量）；【知】=本文作者的 LR 领域知识（Adobe 未公开文档化，凡关键处标"待验证"）；【推】=由前三类推出（标推理链）。

**不可越的边界**（本报告全体遵守）：不做 DRM/授权绕过；不反编译 Adobe 二进制；不提取/分发 Adobe 资产（DCP 相机配置、LCP 镜头配置、预设、37 个端上模型）。对 Adobe 安装包只做"列目录"级别的清点（`REF/target-app-parity.md:30-33` 的做法），这也是 clean-room 的通行底线。

---

## 1. 一句话结论（给决策者）

**LR 的本质不是"修图软件"，而是一个"以参数为中心的照片数据库 + 一台可版本化的渲染机"。** 它的护城河有四层，按"难仿 → 易仿"排序：

1. **色彩科学**（相机→工作空间的矩阵 + DCP + 曲线拟合）—— 需要 1,446 个机型的实测数据，不是代码问题；
2. **渲染管线的阶段顺序与空间选择**—— 代码只有几千行，但顺序错了就"永远不像"；
3. **目录库数据模型 + 非破坏性三层（history/snapshot/virtual copy）**—— 结构清晰，可完整复刻；
4. **外围模块（Book/Print/Web/Slideshow/Map）**—— 工程量问题，无技术难度。

LUMINA 目前 1、2 两层是**结构性缺口**，3 层**部分具备但有三处硬伤**，4 层**主动放弃**（这是对的）。详见 §14。

---

## 2. 产品架构总览：为什么是"数据库中心"而不是"文档中心"

### 2.1 两种范式的分野

| 维度 | 文档中心（Photoshop） | 数据库中心（Lightroom） |
|---|---|---|
| 真源 | 像素文件（`.psd`） | **目录库 `.lrcat`（SQLite）** |
| 打开一张图 | 解码整幅 + 图层树进内存 | 查库 → 读参数 → 渲染预览 |
| 一次编辑的成本 | 一次全文件写盘（数十 MB—GB） | 一次行更新（几十字节—几十 KB） |
| 批量 | 动作/脚本，逐文件全量重算 | 参数复制/同步，零像素搬运 |
| 单位 | 文档 | **照片记录（record）** |
| 失败模式 | 文件损坏 = 作品丢失 | 库损坏 = 全部编辑丢失（**比文件损坏更惨**，见 §3.5） |

这个选择的直接后果，是 LR 能在 2007 年就跑在双核机器上管理十万张图：**浏览从不解码原片**，只读嵌入式 JPEG 缩略图 + 库里的元数据。【知】

### 2.2 catalog 与照片文件分离的三个工程意义

| 意义 | 具体表现 | 证据 |
|---|---|---|
| **原片是不可变的** | LR 从不写 RW2/NEF/CR3 的像素；RAW 的调整只进库与 sidecar | 我们库里 18,147 条调整记录全部在库内，原片 mtime 未变【实】`LR-INV:66-70` |
| **库是可搬迁的** | 库在 `F:/cache/Lightroom/`，照片散在 `F:/`、`X:/`、`Y:/` 五个根 | 【实】`LR-INV:57`；根路径用 `relativePathFromCatalog` 做相对重定位【献】`REF/lightroom-catalog-import.md:104-107` |
| **库是单点故障** | 库损坏 = 全部编辑丢失；用户 2026-04 亲历过一次 | 【实】`LR-INV:88-93`（`catalogFixed` 修复标记 + 两个 0 字节 `temporaryRepairedCatalogue.lrcat` 空壳） |

### 2.3 七模块分工（Classic）

| 模块 | 职责 | 是否触碰像素 | 复用度 | 我们的取舍 |
|---|---|---|---|---|
| Library | 导入/筛选/集合/关键字/元数据/放大查看 | 否（读预览） | 日常 60% 时间 | **必须** |
| Develop | 全部调整参数 + 管线渲染 | 只在导出时 | 日常 35% 时间 | **必须** |
| Map | GPS 聚合 + 逆地理 | 否 | <2% | 晚抄（LUMINA 已有 map 域） |
| Book | 画册排版 → PDF/打印 | 是（排版合成） | <2% | 晚抄（v1.2.0 F7 已覆盖一部分） |
| Slideshow | 全屏播放 + 音乐 | 是 | <1% | 不抄 |
| Print | 单页/多页/联系表 → 打印机/JPEG | 是 | <2% | 晚抄（F7） |
| Web | 生成 HTML 画廊上传 FTP | 是 | <1% | **不抄**（LUMINA 用客户画廊替代） |

【献】权重取自 `REF/target-app-parity.md:67-82`（Classic 外围模块合计权重 4%）。

### 2.4 云版 LR 9.6 与 Classic 的关系（决定我们"对标谁"）

| 维度 | Classic 15.5 | 云版 9.6 |
|---|---|---|
| 真源 / 参数 | 本地 `.lrcat` + XMP sidecar | Adobe 云端库 + 本地缓存 |
| 生成式移除 / 端上模型 | 云端 / 37 个端上模型 | 云端 / 同一套 ModelZoo |

【献】`REF/target-app-parity.md:26-33`。**结论：LUMINA 对标 Classic 的"数据模型 + 管线"，云版的"界面布局"只能借壳，云端能力一律不抄**（违反 R4/R6）。

---

## 3. Catalog 数据模型（重点）

### 3.1 物理文件组

一个 LR Classic 目录不是"一个文件"，而是一组；**`.lrcat` 虽是 SQLite，但"完整数据"不止 SQLite**（大文本外置到 LevelDB 并用 `hasBigData` 标记，任何"只读 SQLite 就算完整解析"的说法都是错的，这也是 U2 至今未闭合的原因）：

| 文件/目录 | 作用 | 我们库中的实测 |
|---|---|---|
| `Lightroom Catalog.lrcat` | SQLite 3（页 4096），核心数据 | 414,920,704 B ≈ 395 MiB，101,299 页【实】`LR-INV:14-17` |
| `.lrcat-wal` | WAL 事务日志 | 分析时"无待检查点 WAL 帧"【实】`LR-INV:17` |
| `.lrcat-shm` | WAL 共享内存索引 | 静态检查不可读【实】 |
| `-Previews.lrdata/` | 预览缓存（分片目录） | **20 个分片**；内部映射 scheme 未解析【实】`LR-INV:115`（U5） |
| `-Smart Previews.lrdata/` | 智能预览（DNG 代理） | 库中未建【实】 |
| `.lrcat-data/` | **LevelDB**，存大载荷 | 184 develop + 5,131 history + 45 before 行 `hasBigData=1`【实】`LR-INV:112`（U2） |

### 3.2 表结构（真实表名 + 我们实测到的字段）

> 表名与主要字段为【知】；"我们实测"列的内容为【实】。

| 表名 | 作用 | 关键字段（知识） | 我们库里的实测 |
|---|---|---|---|
| `Adobe_images` | 图像主表 | `id_local`, `rootFile`, `rating`, `pick`, `colorLabels`, `captureTime`, `orientation`, `aspectRatio`, `fileFormat`, `errorMessage` | 18,192 行；`errorMessage` **全 0**（421 个文件缺失却无任何错误标记）`LR-INV:54-55` |
| `AgLibraryFile` | 文件记录 | `id_local`, `folder`, `idxFilename`, `baseName`, `originalFilename`, `extension`, `fileSize`, `sidecarExtensions` | `sidecarExtensions` 记录 2,053 个文件带 `.xmp`，盘上 2,052 个 `LR-INV:76-77` |
| `AgLibraryFolder` | 文件夹 | `id_local`, `pathFromRoot`, `rootFolder` | 79 个文件夹 `LR-INV:45` |
| `AgLibraryRootFolder` | 根目录 | `id_local`, `absolutePath`, `name`, `relativePathFromCatalog` | 5 个根（F:/2025 10,555、F:/照片 2,553、X:/2026 3,347、Y:/2026/2026 350、F:/lrt 1,387）`LR-INV:45`；`relativePathFromCatalog` 被第三方实现用于跨机重定位【献】`REF/lightroom-catalog-import.md:107` |
| `Adobe_imageDevelopSettings` | **当前调整** | `image`, `settings`(blob), `text`(Lua 序列化), `beforeText`, `hasBigData`, `digest` | 18,147 条非空（全部 >400 字符，最长 307,499 字符）；45 条空串恰为全部视频；`beforeText` 4,337 条【实】`LR-INV:66-70,172` |
| `Adobe_libraryImageDevelopHistoryStep` | 历史步骤 | `image`, `ID`, `text`(zlib), `hasBigData` | 52,913 步，每图 1–396 步；**是全量状态快照，非增量**，步骤名为本地化中文【实】`LR-INV:48,71` |
| `Adobe_libraryImageDevelopSnapshot` | 命名快照 | `image`, `name`, 设置副本 | **0 行**（该用户从未用）【实】`LR-INV:47` |
| `AgLibraryCollection` / `AgLibraryCollectionContent` | 集合 | 集合定义 + 成员 | 8 条 = 快速收藏 1 + 收藏夹集 1 + 智能收藏 6；**静态收藏 0、已发布 0** `LR-INV:46` |
| `AgLibraryKeyword` / `AgLibraryKeywordImage` | 层级关键词 | `name`, `parent`, `synonym` / 关联 | 1 行占位 + **0 行关联** `LR-INV:47` |
| `AgHarvestedExifMetadata` | EXIF 提取缓存 | `image`, `cameraModelRef`, `lensRef`, `iso`, `aperture`, `shutterSpeed`, GPS | 用于相机/镜头分布统计：S5M2 16,781 / A7M3 23；6 款镜头 `LR-INV:61-62` |
| `AgMetadataDescriptor` | 标签/属性描述符 | 元数据字段注册 | 未逐表解析【实】 |
| `Adobe_variablesTable` | 库级变量 | `name`, `value` | 5 项修复标记（`LrCatalog_GenealogyFixedStatus='catalogFixed'` 等）`LR-INV:90` |

**三个必须记住的观察：**

1. **开发设置是两套序列化**：库内是 LR 私有的 Lua 风格 `s = { ... }` 文本（不是 XMP，不是 JSON），同时能写出标准 `crs:` XMP。二者是"同一设置的两种序列化"，因此可以**交叉验证**——这是 OB-02/OB-03 唯一可用的验证路径【实】`LR-INV:136,145`。
2. **历史是快照栈，不是命令日志**：52,913 条 × 平均数 KB = 每图存了 1–396 份完整参数副本。这就是为什么 history 需要 LevelDB 外置。
3. **视频行的 develop 是空串**（45/45），说明 LR 对视频走完全不同的分支。

### 3.3 previews 包结构（未完全解析，但代价可测）

| 层 | 尺寸策略 | 磁盘代价（估计） | 证据 |
|---|---|---|---|
| 缩略图 | 网格用，约 160–320 px | 每图 10–30 KB | 【知】 |
| Standard | 适配屏幕，常见 1440–2048 px | 每图 150–600 KB（JPEG）；我们的库 20 分片 ≈ 数 GB | 【知】+【推】 |
| 1:1 | 原分辨率 | 每图 1–5 MB | 【知】 |
| Smart Preview | 长边 ≤2540 的**有损 DNG** | 每图 ≈1 MB | 【献】`REF/parity-checklist.md:450` |

`-Previews.lrdata` 用 20 个分片目录、片内按哈希分桶——这就是"预览目录不能用普通文件浏览器理解"的原因【实】`LR-INV:115`。

### 3.4 `.lrcat-wal` 的事务性质

库以 `journal_mode=WAL` 运行，写入先落 `-wal`、checkpoint 后合入主库【实】`LR-INV:14`。三个后果：**好**——崩溃时已提交事务不丢（我们的库分析时"无待检查点帧"，说明 LR 已干净退出）；**坏**——外部工具以 `mode=ro` 打开会写 `-shm` 锁字节，**违反零写入纪律**，必须 `immutable=1`（直接适用于 LUMINA 的 `.lrcat` 导入器与备份工具）；**更坏**——WAL 不是快照隔离，第三方导入器因此要求"先关闭 LR"，并承认"metadata checks 不是 SQLite snapshot isolation"【实】`LR-INV:17,123` +【献】`REF/lightroom-catalog-import.md:57-58`。

### 3.5 catalog ↔ XMP sidecar 的双向同步与冲突

| 场景 | LR 行为 | 后果 / 风险 |
|---|---|---|
| RAW 调整 | 默认**只写库**；`Cmd/Ctrl+S` 或"自动写 XMP"开启后写 sidecar | 用户以为"存了"，实际只在库里——库坏 = 全丢【实】`LR-INV:88-93` |
| DNG / JPEG / TIFF / PSD | 调整**写进文件自身**（in-file XMP），不产生 sidecar | 原片字节被改写（对 DNG/JPEG 而言）【知】 |
| 库 ↔ sidecar 冲突 | **以库为准**（"Metadata > Read Metadata from File" 是显式的手动反向操作） | 外部工具（LRTimelapse、我们）改 sidecar 后，LR 不会自动吸收，需手动读入【知】+【实】我们的库中有 1 条"XMP 写入错误残留" `LR-INV:92` |
| 未知字段 | 库内 `s={}` 与 XMP 都能承载，但**部分字段只存在于库**（如 `RemoveAreas` 修复笔画、`pm_*` 补丁元数据） | 只读 sidecar 会丢修复笔画；我们库里 58 张含修复笔画，最大单张 214 KB+【实】`LR-INV:69` |
| 删除/移动 | 库记录路径；文件被外部移走后**静默**保留条目 | **421 个文件缺失（2.31%）而 `errorMessage` 全 0**【实】`LR-INV:54-55` |

**第一手证据的独特价值**：`2,052` 个盘上 sidecar 中，1,387 个是 LRTimelapse 写的（含完整 `crs:` + `lrt:` 命名空间），662 个在雾浓顶/lrt 批次——这说明**延时工作流是真实存在的跨工具互操作场景**，而 LR 对此零支持（LRTimelapse 靠"读 sidecar → 回写 → 让用户手动 Read Metadata"）。LUMINA 若把 sidecar 做成第一真源，天然赢下这个场景【实】`LR-INV:77-78` +【推】。

### 3.6 数据完整性的真实代价（实测）

| 指标 | 数值 | 证据 |
|---|---|---|
| `quick_check` | 0.49 s | 【实】`LR-INV:143` |
| `integrity_check` | 0.92 s（414 MB 库） | 【实】`LR-INV:17` |
| freelist | 仅 3 页（几乎无碎片） | 【实】`LR-INV:17` |
| 历史损坏事件 | 2026-04，修复标记 5 项 + 2 个 0 字节修复壳 | 【实】`LR-INV:88-91` |
| 静默丢文件 | 421 个（RW2 417 + MP4 4），集中在 7 个文件夹 | 【实】`LR-INV:54-58` |

**对 LUMINA 的直接启示**：`integrity_check` 在 400 MB 级库上不到 1 秒，**导入前预检的成本可以忽略**（OB-09）。而"421 个静默缺失"证明了 LR 的一条设计缺陷：**库的完整性 ≠ 资产的完整性，LR 从不校验后者**。这是我们 R3 红线（文件完整性透明）的正当性来源。

---

## 4. Develop 管线顺序（本报告最重要的一节）

### 4.0 先纠一个普遍误解：**面板顺序 ≠ 管线顺序**

LR 的面板是"按用户心智"排列的，不是按执行顺序。最典型的反例是 **Camera Calibration（相机校准）**：它在面板上排在**最底部**，但在管线里执行在**最前面**——因为它的三个 primary hue/sat 直接改的是"相机→工作空间"的色彩矩阵，属于色彩解码的一部分。

反过来，**Detail（锐化/降噪）** 在面板中部，但降噪实际发生在**去马赛克之后、色调之前**（线性域），锐化发生在**色调之后**（感知域）。所以"面板从上到下就是管线从上到下"是个**错的近似**，只在 Basic 面板内部大致成立。【知，高置信；可由 RGB 通道串扰实验证伪】

下面给出**实际执行顺序**。这是全文最重要的一张表。

### 4.1 实际管线（逐阶段）

| # | 阶段 | 执行空间 | 数学性质 | 为什么不能换位置 | 证据级 |
|---|---|---|---|---|---|
| 0 | 解码：黑/白电平、去马赛克、坏点 | 相机原生 **线性** RGB | 线性；CFA 插值 | 之后所有阶段都假定输入是干净的线性辐亮度 | 【知】/【献】`REF/architecture.md:50` |
| 1 | DNG 线性化 / 增益图（ProRAW 局部色调）/ 镜头不透明度 opcode | 线性 | LUT 或逐像素增益 | 属于"文件自带的解码修正"，必须在色彩解释之前 | 【献】`REF/parity-checklist.md:239` |
| 2 | **相机校准 + 配置文件**（DCP：ColorMatrix/ForwardMatrix、HueSatMap、LookTable、ProfileToneCurve） | 相机线性 → 工作线性（ProPhoto/RIMM 原色） | 3×3 矩阵 + 查找表 + 1D 曲线 | **改的是"这个传感器看到的颜色是什么"**；放到后面就等于给已经解释过的颜色再贴一层色偏 | 【献】`REF/raw-parity.md:215-221`（DCP 双光源模型 + HueSatMap/LookTable/ToneCurve 的分工） |
| 3 | **白平衡**（Temp/Tint → 相机空间 RGB 乘子） | 相机线性 | 对角矩阵 | 必须在矩阵转换前/同时（DNG 模型里 `AsShotNeutral` 就是相机空间乘子）；Tint 方向有符号陷阱：**负=偏绿，正=偏品红** | 【实】符号实测 `REF/tint-direction.md` + `REF/parity-checklist.md:195` |
| 4 | **曝光** | 线性 | `×2^EV` | 线性乘子；在 gamma 域做会把高光滚降位置算错 | 【知】 |
| 5 | **对比** | gamma（感知） | 中灰为轴的 S 曲线 | 先把整体反差定下来，后面的分区增益才有意义；在 gamma 域做才符合"感知反差" | 【知】以 Basic 面板顺序为据（Adobe 未公开完整图） |
| 6 | **高光恢复 / 高光·阴影·白·黑** | 感知亮度域（对数亮度） | 边缘感知的局部基 + 分区增益 | 需要"局部"而非"全局"才能保住细节；读的是对比之后的亮度分布，白/黑点同时定义曲线端点 | 【献】`REF/process-versions.md:11`（V2 的 base = log 亮度的可分离中值 + 边缘感知滤波，正是对 ACR 行为的逆向复刻） |
| 7 | **纹理 / 清晰度 / 去雾** | 感知亮度 | 频带分离（texture=中频；clarity=大半径局部对比 + 中间调保护；dehaze=暗通道先验估 airlight + 全局去雾） | 三者都读"当前反差状态"，必须在对比之后；dehaze 需全局量（airlight 百分位），故不能随便分块 | 【献】`REF/gpu-pipeline.md:151-163`（`map` kernel 内含 log 亮度、暗通道、导向滤波、airlight 采样） |
| 8 | **色调曲线**（Parametric + Point，`crs:ToneCurvePV2012*`） | gamma | 分段曲线 LUT | 曲线是"对已定影调再做造型"，必须晚于 5–7；若提前，后续线性乘子会把曲线当增益 | 【实】LUMINA 已按 `crs:ToneCurvePV2012` 写（`LUM:/engine/src/xmp/xmp_mapper.cpp:390-393`），语义正确 |
| 9 | **HSL / 黑白混合**（8 色带） | HSV/HSL | 色相加权分段线性 | 色彩关系需要"亮度已定"；黑白混合本质是"按色相权重重算亮度"，必须在颜色分级前 | 【知】 |
| 10 | **颜色分级**（3-Way/Split Toning，含 Blending/Balance） | 感知 | 按亮度分区加权 + 混色 | 分级要读最终的亮度分布；早期实现（Split Toning）只分两区，现版给 4 个区 | 【献】`REF/parity-checklist.md:201` |
| 11 | **细节：亮度/色彩降噪** | **线性或近线性** | 双边/NLM/引导滤波 | 噪点是传感器域的，越早越好；放到最后会把色调处理放大的噪声当信号 | 【知】；LightCraft 把 NR 放在 `map` kernel（早期）【献】`REF/gpu-pipeline.md:161` |
| 12 | **细节：锐化**（Amount/Radius/Detail/Masking） | 感知亮度 | USM + 边缘掩蔽 | 必须在锐化前完成 tone/color，否则锐化会放大人为色边；Masking 用亮度梯度做门控 | 【知】 |
| 13 | **色差 / 去边（Defringe）/ 镜头配置校正**（畸变、暗角、CA） | 线性几何 | 径向多项式 + 增益多项式 | 几何校正必须在裁剪之前，且畸变是"重采样"，越早做插值次数越少 | 【献】`REF/parity-checklist.md:215-217` |
| 14 | **变换 / 几何**（Upright、透视、旋转、缩放、偏移） | 齐次坐标 | 单应 + 引导线约束 | 透视变换基于"直线应直"，必须在畸变校正之后 | 【献】`REF/parity-checklist.md:219-221` |
| 15 | **裁剪 + 约束裁剪** | 原始像素坐标 | 仿射 + 有效区裁剪 | 裁剪坐标存的是**原始**归一化值，因此裁剪必须排在所有几何之后 | 【献】同上 +【知】 |
| 16 | **局部调整（蒙版）** | 与全局同阶段族 | 每蒙版一套参数 + 遮罩 alpha 合成 | 蒙版坐标随几何变换；局部参数内部按 WB→曝光→对比→…顺序再跑一遍 | 【献】`REF/parity-checklist.md:309`（每滑块都能渲染） |
| 17 | **效果：裁剪后暗角**（Post-Crop Vignette） | 输出坐标 | 径向增益 | 名字里的 "Post-Crop" 就是规格：必须在裁剪之后、输出分辨率下 | 【知】 |
| 18 | **效果：颗粒**（Amount/Size/Roughness） | 输出分辨率 | 程序化噪声 | 必须在最终分辨率下生成，否则预览≠导出 | 【知】 |
| 19 | **输出锐化** | 输出分辨率 | 按介质（屏幕/哑光/光面）定标 | 必须在**重采样之后**，否则重采样把锐化糊掉 | 【知】 |
| 20 | **色彩空间转换 + 编码** | 工作空间 → sRGB/P3/AdobeRGB/ProPhoto | 3×3 + TRC + 色域映射 | 最后一步；早做会截断色域 | 【献】`REF/parity-checklist.md:398` |

### 4.2 三条"顺序不可换"的根本原因

1. **线性 vs gamma 的分界**：曝光/白平衡/降噪是**物理量**，必须在线性域；对比/曲线/分级是**感知量**，必须在 gamma 域。把曝光挪到对比之后，同一组滑块在高低光上的响应会整体错位——这正是"看起来差不多但就是不像 LR"的典型症状。【知】
2. **几何的阶段耦合**：镜头校正（13）→ 变换（14）→ 裁剪（15）→ 蒙版（16）→ 暗角/颗粒（17-18）。任何一步前置，都会让"同一蒙版在预览与导出中落在不同像素上"。
3. **分辨率的阶段耦合**：颗粒（18）、暗角（17）、输出锐化（19）必须在**输出分辨率**求值。这不是性能优化，是**正确性**要求。

### 4.3 process version 升级时管线变了什么

| PV 代 | 变更 | 影响 | 证据 |
|---|---|---|---|
| 2003 代 | 老 Basic：Recovery / Fill Light / Brightness | — | 【知】 |
| **2010 代** | 高光恢复的局部化 + 新曲线 | 老图按 2010 解释 | 【知】 |
| **2012 代** | **Basic 面板重写**：Recovery/Fill Light 被 Highlights/Shadows/Whites/Blacks 取代；tone 映射改为局部拉普拉斯式；曲线改为 Parametric+Point 并存 | 同一组数值在 2010 与 2012 下**渲染完全不同**，所以必须按 PV 分派 | 【知】高置信 |
| 2012 之后 | 只做**小步前进**：新增阶段（Texture 2019、Dehaze 2015）默认恒等，不改已有渲染，**因此不 bump PV** | "加一个默认恒等的阶段"是安全的；"改一个已有阶段的响应"必须 bump | 【推】依据 `REF/process-versions.md:13-23` 的规则（第三方实现照抄的正是 LR 的这条规则） |
| **15.4** | 我们库里 **18,147 条全部为 `"15.4"`**，45 个视频为 NULL | 全库单一代，无历史包袱；但我们的分析当时无法判定 15.4 具体改了什么（`REF/process-versions.md` 未列该值） | 【实】`LR-INV:50,103` |

**工程结论**：PV 是"渲染契约的版本号"，不是一个标签。它的三条硬规则（我们应直接抄）：

- **改外观 → 必须新 PV**；改 bug（NaN、黑图、CPU/GPU 不一致）→ 可以原地修；
- 复制/粘贴/同步/预设**不带 PV**（否则把 A 的渲染契约套到 B 上）；
- 老设置（无字段）读作**最老代**，不读作最新代。【献】`REF/process-versions.md:45-62`

---

## 5. 非破坏性模型：五个概念，五种语义

| 概念 | 存什么 | 生命周期 | 能否跨照片 | 存储位置 | 我们库实测 |
|---|---|---|---|---|---|
| **History** | 每次提交后的**完整参数快照** | 每图独立，可回退任意步 | 否 | `Adobe_libraryImageDevelopHistoryStep`（zlib）+ LevelDB 大载荷 | 52,913 步 / 每图 1–396【实】 |
| **Snapshot** | 用户命名的参数快照 | 手动创建，永久 | 否 | `Adobe_libraryImageDevelopSnapshot` | **0 条**【实】 |
| **Virtual Copy** | 同一原片的**第二条参数记录**（"主记录"仍是一张照片） | 与主记录同级 | 否 | `Adobe_images` 另一行 + 指向同一 `AgLibraryFile` | **0 条**【实】 |
| **Version**（云版） | 云端版本链 | 云 | — | 云 | 不适用【献】 |
| **Preset** | 一组参数（可按组勾选） | 全局/组内 | **是** | 用户预设目录（`.xmp` / `.lrtemplate`） | 库内 `presets.json` 无 LR 预设来源【实】 |
| **Profile** | 色彩解释方式（DCP/Look） | 全局 | 是（但语义是"换相机"） | `CameraProfiles`（1,500 个 Adobe Standard 名 + 395 个 Camera Matching 名）【测】 | 全库仅 **3 个 CameraProfileDigest + 3 个 LookTable**（风险面很小）【实】`LR-INV:79` |

### 5.1 "编辑参数即数据"的边界在哪

**在参数里，不在像素里**——但有四个例外，是 LR 自己承认"参数表达不了、必须落成数据"的地方：

| 例外 | 为什么必须落数据 | 证据 |
|---|---|---|
| **AI 蒙版结果** | 模型推理贵、且用户不希望每次开图重算；但 LR 存的是**参数+可重算的种子**（对比：LightCraft 存 288² logits 量化块，10–30 KB） | 【献】`REF/ai-masks.md:33-37` |
| **修复笔画（RemoveAreas）** | 补丁像素/位移必须持久化，否则无法复现 | 【实】58 张，单张 214 KB+ `LR-INV:69` |
| **HDR/Pano/超分产物** | 合成结果是**新像素**，只能落成新文件（DNG） | 见 §9 |
| **人脸识别向量** | 识别结果是模型输出，非用户参数 | 【献】`REF/faces.md`（未细读，仅指路） |

**边界判据**（可直接作为 LUMINA 的设计规则）：*如果一个编辑结果无法由"原片 + 参数 + 公开算法"在合理时间内确定性地复现，它就必须落成数据（且必须可被丢弃后重算）。* 这条判据同时解释了为什么 AI 蒙版要落盘、而曲线不用。

---

## 6. 局部调整与蒙版

### 6.1 五类蒙版的数学性质

| 类型 | 数学形式 | 参数 | 可编辑性 | 我们的库 |
|---|---|---|---|---|
| **线性渐变** | `alpha = clamp(dot(p−p0, d) / ‖d‖², 0, 1)` 再按 feather 平滑 | 起止两点 + feather | 拖两点 | 3,639 张含局部蒙版（83.8%）【实】 |
| **径向渐变** | 椭圆距离场 `r = ‖M⁻¹(p−c)‖`，`alpha = smoothstep(1, 1−f, r)`，可反转 | 中心、两轴、旋转、feather、invert | 可旋转/复合【献】`REF/parity-checklist.md:301` | 同上 |
| **画笔** | 沿笔画撒 dab（径向核），覆盖率按 pass 累积（**不是按按住时长**） | size/feather/flow/density + 逐 dab | 擦除、直线连接、压感 | 同上 |
| **颜色范围** | 在指定色彩空间里点到样本的距离阈值（最多 5 个样本 + refine） | 样本色 + 容差 + smoothness | 增删样本 | 【献】`REF/parity-checklist.md:302` |
| **亮度范围** | 亮度区间 + smoothness，两端可交叉 | 两个端点 + smoothness | 拖端点 | 【献】`REF/parity-checklist.md:303` |
| **深度范围** | 深度图区间（需深度数据） | 两端点 | — | 【献】`REF/parity-checklist.md:304`（未实现） |

### 6.2 组合逻辑：Add / Subtract / Intersect

一个"蒙版"是**组件树**，每个组件带一个 op，按顺序逐步合成：`mask = comp1`，然后 `∪ comp2`(Add) / `\ comp3`(Subtract) / `∩ comp4`(Intersect)。**关键细节**（易错、我们尚未实现）：op 之间**不是简单集合运算**，Intersect 后接 Subtract 与反过来结果不同；LightCraft 的判定是"每组件独立带 op，Add▸Brush 会新建一个带 op 的组件"【献】`REF/parity-checklist.md:305`。

### 6.3 范围蒙版为什么"在预览与导出一致"很难

范围蒙版要读"当前渲染后的像素值"来判断是否落入范围。若预览是 1920 px、导出是 6000 px，则同一像素的邻域统计不同 → 蒙版边缘会漂。LR 的解法是**在固定的中间分辨率上求蒙版，再上采样**。【知】LightCraft 的解法是把坐标归一化到原片坐标 + 蒙版在"请求的 HDR/SDR rendition"空间采样（`REF/parity-checklist.md:302`）。**两者本质相同：蒙版必须定义在与输出分辨率无关的空间里。** 这条是 LUMINA 蒙版降噪/预览必须遵守的硬约束。

### 6.4 AI 蒙版的技术路线（LR 9.6 的 37 个端上模型）

| 能力 | 技术路线（推测但有旁证） | 旁证 |
|---|---|---|
| 主体/背景 | 显著性分割（轻量 encoder-decoder） | LightCraft 用"saliency heuristic"作对照实现 `REF/parity-checklist.md:293` |
| 天空 | 语义分割（天/地二分类） | 同上 `:294` |
| 人物/皮肤/头发/牙齿/眼镜 | 部件级语义分割（多类） | `REF/ai-masks.md` 提到 DNG 语义 matte 分类正是 skin/hair/teeth/glasses |
| 物体（点选/框选） | 提示式分割（SAM 类） | LightCraft 用 SAM 3（Meta 2025）`REF/ai-masks.md:3-5` |
| 深度范围 / 镜头模糊 | 单目深度估计 | `REF/parity-checklist.md:304` |
| 降噪 / 去尘 / 去反射 / 干扰检测 / 闭眼·模糊·美学评分 / 搜索嵌入 | 各自独立小模型 | `REF/target-app-parity.md:86-89` |

**37 个模型的清单**来自对安装包 `ModelZoo` 目录的**仅列名**清点（`REF/target-app-parity.md:33`），我们未分发、未加载、未反编译任何权重。

**对 LUMINA 的判断**：AI 蒙版是"权重+数据"问题，不是算法问题。SAM 类可替换（LightCraft 用纯 Rust + candle 跑通了，说明工程可行），但 Sky/People 这类"语义类别"模型需要训练数据。**建议：先做"启发式 + 可插拔模型接口"，把模型当可选下载件**（与 R4/R6 离线红线兼容：缺模型时降级为手动蒙版，不阻塞工作流）。

---

## 7. 预览与性能

### 7.1 预览生成的策略与磁盘代价

| 层 | 何时生成 | 尺寸 | 代价 | 用途 |
|---|---|---|---|---|
| 嵌入式 JPEG 快路径 | 导入瞬间（不解码 RAW） | 相机内 JPEG 缩略图（通常 160×120 到全尺寸） | **0**（就在 RAW 里） | 网格首次显示、导入预览 |
| 标准预览 | 空闲/按需 | 适配屏幕（1440–2048） | 每图 0.15–0.6 MB | Loupe 默认 |
| 1:1 预览 | 按需（`Build 1:1 Previews`） | 原分辨率 | 每图 1–5 MB | 100% 检查、细节修图 |
| 智能预览 | 显式 `Build Smart Previews` | 长边 ≤2540 的有损 DNG | ≈1 MB/图【献】 | **离线可修图/可导出** |

**磁盘策略**：LR 用"分片目录 + 按需丢弃"（`Discard Previews`）控制体积；我们的库有 20 个分片【实】`LR-INV:115`。**Smart Preview 的真正卖点不是"省空间"，是"原片离线时仍可编辑并导出"**——这是 LR 唯一能在笔记本上带全部素材出门的机制。【献】`REF/parity-checklist.md:450`

### 7.2 渲染性能：可对标的天花板（LightCraft 实测，非 LR 实测）

> ⚠️ 下面全部是**第三方对标实现的测量**，用作"同样的功能在什么量级上可实现"的参照，**不是 Adobe 的数字**。

| 场景 | CPU | GPU | 出处 |
|---|---|---|---|
| Loupe 1920×1280 冷启（含上传） | 415 ms | **32 ms** | `REF/gpu-pipeline.md:269` |
| 曝光拖动（热） | 22–42 ms | **3.9 ms** | `:271` |
| 高光拖动（draft，热） | 14 ms | 3.2 ms | `:272` |
| 清晰度拖动（draft，热） | 16–23 ms | 3.0 ms | `:273` |
| 降噪拖动 | 186–471 ms | 10–18 ms | `:274` |
| **6000×4000 导出渲染** | 1037–1414 ms | **291–330 ms（≈0.3 s）** | `:276` |
| 85k 图库打开 | — | 1.7–4.7 s | `REF/target-app-parity.md:55` |

**CPU/GPU 一致性**：8-bit sRGB 下 `mean|Δ|<0.5 LSB, max|Δ|≤3 LSB`，实测 max 1 LSB（`REF/gpu-pipeline.md:240-257`）。这给了我们一个可执行的验收口径：**GPU 路径与 CPU 参考实现的差必须 ≤1 LSB**，否则视为 bug 而非"近似"。LUMINA 目前是"UI 侧 WebGL 近似 + 引擎侧 8bit 近似"，两条路都不满足这个口径（见 §14）。

### 7.3 大库的工程边界

打开 85k 图库 1.7–4.7 s 靠的是"索引 + 分页 + 预览缓存"而非全量扫盘；100k 规模时栈/集合查询用索引做到 ~14 ms（原 0.6–2.7 s）【测】+【献】`REF/gaps.md:43`。导入只读**文件头**（`probe_info`），不解像素，个别格式仅需前若干行补齐标签【献】`REF/gpu-pipeline.md:314-316`——**这条必须抄**：LUMINA 的 `readMetadata` 若整幅解码，5 万张导入会崩。

---

## 8. RAW 解码与色彩：最难仿的部分

### 8.1 相机覆盖

| 指标 | Adobe | 数值 | 出处 |
|---|---|---|---|
| 支持的相机型号 | ≈1,446 | — | 【测】`REF/target-app-parity.md:50` |
| Adobe Standard 配置名 | 1,500 | — | 【测】`REF/target-app-parity.md:32` |
| Camera Matching 配置名 | 395（另处写 481 个机型） | 两处数字不一致，取"数百" | 【测】`REF/target-app-parity.md:32` vs `REF/raw-parity.md:218` |
| 镜头配置 | 3,635 个文件 / 56 厂商 | — | 【测】`REF/raw-parity.md:245` |
| 我们用户的实际需求 | **RW2 占 90.5%（16,470 张），DNG 273，ARW 15** | — | 【实】`LR-INV:63` |

**结论一**：对 LUMINA 而言，"1,446 个机型"是无意义的对标目标。先做 **RW2（松下）+ DNG + 主流 Sony/Canon/Nikon**，即可覆盖 95% 目标用户。【推】

### 8.2 DCP 相机配置文件到底是什么

| 层次 | 内容 | 能不能自己造 |
|---|---|---|
| 色彩矩阵 | `ColorMatrix1/2` + `ForwardMatrix1/2`（双光源：D65/A） | 能（需光谱仪/色卡） |
| HueSatMap / LookTable / ProfileToneCurve | 二维色相-饱和度表、风格化表、基础影调曲线（DNG 规范 ch.6） | 能（需拟合） |

**"Adobe Standard" 的含义**：不是"标准色彩"，而是 Adobe 自己的**审美基线**——把每个机型的传感器响应统一到一个"讨喜且中性"的外观。所以 **Adobe Standard ≠ 相机直出**（Camera Matching 才试图逼近各家 Picture Style / Film Simulation），且换配置文件等于换"这台相机看到的颜色"，属于 §4 第 2 阶段、**在曝光之前**。【献】`REF/raw-parity.md:215-221` +【知】

### 8.3 为什么色彩科学是 LR 最难仿的部分（有数）

| 对照 | ΔE | 出处 |
|---|---|---|
| 第三方实现 vs **相机 JPEG**（拟合目标） | median **5.1**（Sony 81/87 通过）；RW2 median **5.0**（140/174）；NEF 2.9–7.8 | 【测】`REF/raw-parity.md:225-228` |
| 拟合被拒时回退中性矩阵 | ΔE **18.5**，ΔL\* −11，chroma ×0.39 | 同上 |
| 第三方实现 vs **Lightroom**（少数可测） | Sony DRO ΔE76 **7.5 → 3.3**；Olympus E-1 ΔE00 **11.25 → 8.30**；ProRAW 带增益图 ΔE00 **19** | 【测】`REF/raw-parity.md:229-231` |
| 对 **Adobe Standard** 的跨机型测量 | **不存在**（论文级空白） | 【测】`REF/raw-parity.md:231` |

**读法**：连一个投入 ~20 万行 Rust、跑了 2,500+ 测试的对标实现，也只能做到"对着相机 JPEG 的中位 ΔE ≈5"，并且**承认无法测量对 Adobe Standard 的差距**。三条推论：①「像不像 LR」在数学上等价于「有没有那 1,500 个 DCP 的等价物」；②没有 DCP 时 ΔE 5 已是"看着对但不是那个味"；③用户的感知阈值约在 ΔE 2–3（同场景并排比对）。

**给 LUMINA 的策略**（三选一，必须在 v1.2.0 前定）：
- **A（推荐）**：自建 DCP 等价物，只做 5–10 个目标机型（松下 S5M2 必做），用色卡 + 分光数据拟合；
- **B**：不追求 Adobe Standard，明确宣称"自有色彩科学"，把 ΔE 目标定在"对相机 JPEG ≤3"；
- **C**：允许用户导入第三方 `.dcp`（**不内置、不分发**，只做加载器）。这条路法律上最干净，但会把"像不像 LR"的责任转移给用户。

---

## 9. 合成能力：为什么产物全是 DNG

| 能力 | 输入 | 输出 | 为什么是这个形态 |
|---|---|---|---|
| **HDR Merge** | 2–9 张不同曝光的 RAW/JPEG | **32-bit float DNG** | ① 需要 >8bit 且 >1.0 的辐亮度才能容纳高光；② DNG 是唯一"被 LR 自己当 RAW 继续调"的容器；③ 保留线性 → 后续管线无需特殊分支 |
| **Panorama Merge** | 多张重叠 | **DNG**（球面/柱面/透视） | ① 投影后像素是重采样的，必须是新的"负片"；② 保留线性 + 相机矩阵，才能继续用 Develop 全部滑块 |
| **Super Resolution** | 单张 RAW | **DNG（2× 线性 = 4× 像素）** | ① 超分发生在去马赛克前后的线性域；② 输出必须"看起来还是一条 RAW"才能继续降噪/锐化 |
| **HDR Panorama** | 组合 | **DNG** | 同上 |

**共同点（这是设计哲学，不是实现细节）**：
1. **产物必须是"新的负片"，而不是"新的成品"** —— 因此后续所有非破坏性能力（快照、虚拟副本、局部调整、PV 升级）在产物上继续可用；
2. **线性浮点** —— 只有线性浮点能承载 HDR 的 >1.0 值；
3. **保留来源的镜头/色彩元数据** —— 合并后的 DNG 要带上参考帧的 `OpcodeList3`（WarpRectilinear / FixVignetteRadial）与 `AsShotNeutral`（多帧白平衡中位数），否则产物一打开就偏色。【献】`REF/merge.md:36-45`、`REF/parity-checklist.md:355-356`

**对 LUMINA 的含义**：v1.2.0 的 F1（全景）/F2（景深堆栈）如果输出 JPEG/TIFF，就**主动放弃了"产物可继续非破坏性编辑"**。建议：输出 **16-bit 线性 DNG（或 TIFF + 自描述 sidecar）**，并把产物登记成"合成来源组"（PRD §7 已有 `lumina:CompositeSourceRefs`，方向正确）。

---

## 10. 导出与交付

| 维度 | LR 的做法 | 易错点 |
|---|---|---|
| **导出预设** | 命名预设（格式/尺寸/锐化/水印/元数据/命名一次性打包）+ "Export with Previous" | 预设必须**整体**是快照，不能只存部分 |
| **输出锐化** | 三档语义（Screen / Matte / Glossy）+ Amount + 按输出尺寸定标 | 必须在**重采样之后**；预览锐化与输出锐化是两套 |
| **水印** | 文字/图形，9 锚点 + 水平/垂直 inset + 旋转 + 不透明度 + 阴影 | inset 是**分别**的水平/垂直（旧版只有一个）【献】`REF/parity-checklist.md:402` |
| **色彩空间 + 尺寸** | sRGB/P3/AdobeRGB/ProPhoto + 长边·短边·宽高·MP·百分比 + 不放大 + ppi 标签 | 必须内嵌 ICC；ppi 只写标签不改像素；"不放大"是硬开关 |
| **元数据 + 命名** | 全保留/仅版权/全部移除（含 GPS）+ 模板语言（日期/序号/原文件名） | **移除 GPS 是隐私刚需**；命名冲突策略必须连带 sidecar |
| **DNG 导出** | 压缩（无损 JPEG / ZIP / 无 / 有损）+ "渲染后的 DNG" | 有损 DNG 是"去马赛克后"的线性相机 RGB，不是 JPEG |

【献】`REF/parity-checklist.md:392-408`（逐项对标 LR 导出对话框的实现说明，可直接当字段清单用）。

---

## 11. 文件格式与互操作

### 11.1 RAW 容器谱系（按我们用户的实际优先级）

| 族 | 代表 | LUMINA 优先级 |
|---|---|---|
| TIFF/EP 派生 | NEF、ARW、**RW2**、PEF、SRW、ORF、CR2 | **RW2 第一**（16,470 张 / 90.5%）【实】 |
| DNG | 原生 / 转换 / 合成产物 | **第二**（273 张 + 未来全部合成产物）【实】 |
| ISO BMFF 派生 | CR3、HEIF 静帧 | 第三（Canon 用户）／X-Trans RAF 更低 |

### 11.2 DNG 的角色（四种完全不同的东西）

| 种类 | 来源 | 特性 |
|---|---|---|
| **原生 DNG** | 宾得/徕卡/理光/手机（ProRAW） | 自带 `ColorMatrix`、opcode、语义 matte |
| **转换 DNG** | LR "Convert to DNG" | 内嵌原片 + 转换后 TIFF 结构；可选"仅嵌入原片" |
| **合成 DNG** | HDR/Pano/超分 | 浮点线性、无 CFA、`LinearRaw` |
| **智能预览 DNG** | Build Smart Previews | **有损**、长边 ≤2540、仅供离线编辑 |

**互操作意义**：DNG 是唯一"既能当输入又能当输出"的格式，LR 的全部合成与代理能力都寄生在它上面；LUMINA 不实现 DNG 写，就永远做不了"产物可继续编辑"。【推】

### 11.3 XMP sidecar 的 `crs:` 字段集（我们应覆盖的最小集）

| 组 | 字段 | 我们现状 |
|---|---|---|
| 基础 + 存在感 | `WhiteBalance`/`Temperature`/`Tint`/`Exposure2012`/`Contrast2012`/`Highlights2012`/`Shadows2012`/`Whites2012`/`Blacks2012`/`Texture`/`Clarity2012`/`Dehaze`/`Vibrance`/`Saturation` | 全部已写【实】`LUM:/engine/src/xmp/xmp_mapper.cpp:26-40` |
| 曲线 | `ToneCurvePV2012`(+RGB 三通道)、`ParametricHighlights/Lights/Darks/Shadows` + 三个 Split | 已写 `:390-393` |
| 混色器 | `HueAdjustment*`/`SaturationAdjustment*`/`LuminanceAdjustment*` × 8 带 | 已写 `:51-75` |
| 分级 + 校准 | `SplitToningShadow*/Highlight*`、`ColorGradeMidtone*`、`ColorGradeBlend/Balance`、`Calibration*` | 已写 `:76-93` |
| 几何 + 光学 | `CropTop/Bottom/Left/Right/Angle`、`HasCrop`、`Perspective*`、`LensProfileEnable`、`LensManualDistortionAmount`、`PostCropVignette*` | 已写 `:94-106` |
| 细节 + 配置 | `Sharpness`/`SharpenRadius·Detail·Masking`、`LuminanceSmoothing`、`ColorNoiseReduction*`、`CameraProfile`/`CameraProfileDigest`/`LookTable` | 已写（但无 DCP 可指向） |
| **PV** | `ProcessVersion` | 只写常量 `kMaxProcessVersion`【实】`xmp_mapper.cpp:491` |
| **蒙版 + 修复** | `MaskGroupBasedCorrections`、`RetouchAreas`（LR 的标准位置） | **均未写**，改用 `lumina:Masks`/`lumina:Removals`【实】`xmp_mapper.cpp:139-145` |

**互操作四条硬规则**：①原始 RAW 只写 sidecar，绝不改字节（R1 已对齐）；②DNG/JPEG/TIFF/PSD 要写 in-file XMP，否则 Lightroom 读不到；③不认识的字段必须原样保留（`REF/gaps.md:87` 记录过"命名空间前缀冲突覆盖外来元数据"的真实 bug）；④导入他人库必须只读、有上限、要求关闭源程序、缺失原片保留为可重定位条目并列出原因（`missingFile/missingFolder/missingRoot/incompletePath/masterSkipped`）【献】`REF/lightroom-catalog-import.md:25-58`。

---

## 12. 逆向方法论提炼（以 `LR-INV` 为案例）

我们**只靠 SQLite + XMP + 文件系统**，没有运行 LR、没有反编译、没有联网，就得出了 23 条结论（21 条被独立复核复现，2 条被纠正）。方法论如下：

| 步骤 | 做法 | 得到的结论（举例） |
|---|---|---|
| 1–2. **先锁只读 + 规模画像** | `immutable=1` 只读打开，事后校验 mtime/大小未变；数行数/体积/freelist | 全程零写入可复现；18,192 张 / 395 MiB / freelist 3 页【实】`LR-INV:19,123` |
| 3–4. **全路径存在性扫描 + 分组聚合** | 18,192 条路径逐条 `stat`，按文件夹聚合异常 | **421 个缺失（2.31%）而 `errorMessage` 全 0 → LR 静默丢文件**，集中在 7 个文件夹（含 2 个近乎整夹丢失） |
| 5–6. **抽样解剖 + 双序列化交叉验证** | 4 条 `text` 结构分析；库内 `s={}` ↔ 盘上 2,052 个 `crs:` sidecar | 确认是 Lua 风格 `s={}`；建立"同一设置两种写法"的验证通道 |
| 7. **推翻自己的第一版** | Phase 1 称 2,775 个 .xmp，复核后为 2,052，且 `F:/lrt/` 无 sidecar | 数字类结论必须能点验到具体路径 |
| 8–9. **未知登记 + 义务账本** | U1–U7 未知项（原因+处置）；14 条 OB 标闭合状态 | 只有 1 条闭合（OB-13 放弃类）→ 诚实而非好看 |
| 10. **用"限制说明"反证可信度** | 有界否定、静态纪律、验证者不可用 | 凡"盘上只有 N 个"均为有界否定，不作绝对断言 |

**最值钱的三条方法论**：

1. **"缺失"比"存在"信息量大**：421 个静默缺失 + 10,216/4,015 的伴随 JPG 分裂，只有全量扫描能发现。R3 红线由此而来。
2. **同源双序列化是最好的 oracle**：私有格式没文档，但标准 XMP 是它的"翻译版"，二者互为基准。**任何逆向都应先找"同一事实的两种表达"。**
3. **把未知写成账本，而不是省略号**：U1（`MJ` 镜头 761 张）、U2（LevelDB 键空间）、U5（previews 映射）都留了处置路径，后续工程可直接接手。

---

## 13. 对 LUMINA 的仿造建议（三档 + 人日）

> 人日口径：1 人日 = 6 人时有效编码 + 验收。参照 `REF/gaps.md` / `REF/target-app-parity.md` 的 agent-hours 校准，但**换算系数按 1.6 上调**（工程化、测试、文档与 Windows 打包的实际成本），全部标【估】。

### 13.1 必须抄（决定摄影工作流闭环）

| # | 能力 | 为什么必须 | 不抄的后果 | 人日 |
|---|---|---|---|---|
| M1 | **场景线性渲染管线 + §4 的 20 阶段顺序** | 决定"像不像"的一切；也是所有局部调整/合成的前提 | 滑块手感永远不对；合成产物无法继续编辑 | 18–30【估】 |
| M2 | **catalog 数据模型**（图像/文件/文件夹/根 四表分离 + 集合 + 层级关键词） | 决定 5 万张能否滚动；决定"移动文件夹不改库" | 大规模库必然重构（现在改比以后改便宜 10 倍） | 10–16【估】 |
| M3 | **history / snapshot / virtual copy 三层落盘** | 非破坏性的"可回退"承诺必须有物证 | R2 红线无法验证；崩溃即丢编辑历史 | 6–10【估】 |
| M4 | **蒙版组件树（Add/Subtract/Intersect）+ 五类蒙版** | 现代修图 80% 的工作量在局部 | 只能做"全局滤镜"，产品定位降级 | 15–25【估】 |
| M5 | **预览三层（嵌入 JPEG 快路径 / 标准 / 1:1）** | 决定"打开是否卡" | 5 万张库无法达到 60fps 滚动（现有红线） | 8–14【估】 |
| M6 | **ProcessVersion 机制（含"改外观必 bump"纪律）** | 决定"以后能不能改管线而不毁老片" | 第一次调管线就会毁掉全部老片外观 | 2–4【估】 |
| M7 | **导出预设的"整体快照"语义 + 输出锐化 + 色彩空间 + 元数据裁剪** | 交付是工作流终点 | 客户交付不可重复、GPS 泄露 | 6–10【估】 |
| M8 | **合成产物 = 线性 DNG（或等价的可再编辑容器）** | 决定 F1/F2 是不是"一次性玩具" | 全景/堆栈产物变成死图 | 8–14【估】（含 DNG 写） |

**必须抄合计：73–123 人日【估】**

### 13.2 可以晚抄（Classic 外围模块）

| # | 能力 | 为什么可晚 | 现成替代 | 人日 |
|---|---|---|---|---|
| L1 | Book / Print / Slideshow | 权重合计 <4%【献】；且 v1.2.0 F7 已覆盖"打印/打样"核心 | F7 自研（更贴中国用户） | 15–25【估】 |
| L2 | Map 模块（地图聚合 + 拖拽定位） | LUMINA 已有 map 域与离线瓦片 | 现有 map | 6–10【估】 |
| L3 | Web 模块（生成 HTML + FTP 上传） | 已被"客户画廊"取代，且 FTP 是过时交付 | gallery 域 | 0（不抄） |
| L4 | 人脸 / People 视图 | 价值高但与"全面对标"不同线；模型可后接 | 预留接口 | 10–18【估】 |
| L5 | 智能预览（离线可修） | 与 R4"离线不可修"直接冲突 | 需产品决策：改红线 or 不做 | 8–14【估】 |
| L6 | 插件 SDK | 生态是长期事，且 LUMINA 无第三方开发者 | MCP/RPC 已可扩展 | 20–40【估】 |

**可晚抄合计：59–107 人日（其中 L3 建议不做）**

### 13.3 坚决不抄

| # | 能力 | 为什么不抄 | 依据 |
|---|---|---|---|
| N1 | **云端生成式移除 / Firefly 类生成** | 违反 R4/R6 离线红线；需订阅额度；不可审计 | PRD R4/R6 |
| N2 | **Creative Cloud 同步 / 账号绑定 / 订阅校验** | 我们卖的是"不锁库"，账号锁是反面 | PRD R1 |
| N3 | **分发 Adobe 资产**（DCP 1,500 / LCP 3,635 / 预设 / 37 模型） | 法务风险；clean-room 底线 | §0 边界 |
| N4 | **"库为唯一真源、sidecar 可选"** | 正是用户 2026-04 数据事故的根因 | `LR-INV:88-93` |
| N5 | **静默失败**（缺失 421 个而 `errorMessage=0`） | 违反 R3 | `LR-INV:54-55` |
| N6 | **单写者独占库**（LR 开着时任何工具不能写） | 违反"可被任何工具读取"卖点 | `REF/lightroom-catalog-import.md:57` |
| N7 | **视频编辑**（trim/调色/导出） | 权重 1%，成本 25–45 h【献】 | `REF/gaps.md:35` |
| N8 | **发布服务**（Flickr/SmugMug 直传） | 平台已死/国内不可用 | — |

---

## 14. 与 LUMINA 现有架构的对齐度

### 14.1 已对齐（做对了，别改）

| 能力 | LR | LUMINA 现状 | 判定 |
|---|---|---|---|
| **原片零改写 + 开放 sidecar** | RAW 只写 sidecar | R1 红线 + `xmp_service` 只写 sidecar【LUM】 | ✅ **优于 LR**（LR 对 DNG/JPEG 是 in-file 改写） |
| **`crs:` 标准字段覆盖面** | 全套 | 60+ 个 `crs:` 字段已映射（含曲线、混色器、分级、校准、几何、光学、细节）`xmp_mapper.cpp:24-118` | ✅ 覆盖面接近完整 |
| **SQLite 目录库** | `.lrcat` | `schema.sql` 单一事实源 + WAL + FTS5 中文全文 `schema.sql:1-93` | ✅ 结构方向对 |
| **虚拟副本 / 快照表** | 有 | `virtual_copies` / `snapshots` 表 + `VirtualCopyService` `schema.sql:171-185` | ✅ 表在，语义需补（见 14.2） |
| **智能集合** | 有（6 条规则） | `SmartCollections::all()` 预设规则 `library/smart_collections.h:12-19` | ✅ 有，但比 LR 的规则树弱 |
| **健康检查 / 缺失显式告知** | ❌ LR 不做 | `health_issues` 表 + `health` 字段 `schema.sql:150-161` | ✅ **优于 LR**（正对 R3） |
| **备份** | 只有"备份目录" | `backups` 表 + `verified_at` | ✅ 优于 LR |

### 14.2 结构性缺陷（必须改，且越早越便宜）

| # | 缺陷 | 证据 | 后果 | 建议 |
|---|---|---|---|---|
| **D1** | **渲染管线在 8-bit sRGB 域做近似，不是场景线性** | WebGL：`pow(2.2)` 近似线性 `app/src/modules/develop/webgl/shaders.ts:64-65`；曝光在 sRGB 域乘 `pow(2,EV)` `:110-111`。引擎侧只有 3 参数近似 `engine/src/imaging/imaging.cpp:121`（`applyToneApprox`：曝光/对比/饱和度） | 高光滚降位置错、局部调整无法与全局解耦、合成产物无从谈起；GPU 与 CPU 两条路径互不相等（LR 口径是 ≤1 LSB） | 引入线性工作空间（`float` + ProPhoto/RIMM 原色）+ CPU 参考实现；GPU 与 CPU 做等价性测试（口径 ≤1 LSB） |
| **D2** | **阶段顺序与 LR 不一致** | 现序：曝光 → 高光/阴影/白/黑 → 对比 → **色温/色调** → 去雾/清晰度 → 曲线 → 饱和 → HSL → 蒙版 `shaders.ts:110-155`。LR 序：**WB 在曝光之前**、对比在高光/阴影之前、细节/镜头/几何/效果在蒙版前后各有固定位（§4.1） | 同一组参数"看着像但手感不对"，且无法通过调参数补偿 | 按 §4.1 重排；**WB 必须提到第一位**（它现在排在对比之后，是最大的单项错误） |
| **D3** | **缺整个几何/光学阶段** | shader 只有 crop 参数与透视参数写入 sidecar（`xmp_mapper.cpp:94-106`），无畸变/透视/镜头重采样实现 | `lensProfileEnable`、`Perspective*` 写得出读得回，但画面上没有效果 → 用户认为"参数是假的" | 加几何阶段（畸变多项式 → 单应 → 裁剪），放在蒙版之前 |
| **D4** | **history 只在内存，不落盘** | `HistoryStack` 是 `std::vector<JsonValue>`，无 DAO、无表 `engine/src/edit/history.h:14-40`；schema 里**没有** history 表 | 重启即丢撤销栈；"历史"面板无法实现；与 LR 的 52,913 步（实测）不在一个量级 | 加 `history_steps` 表（image, seq, label, params_json, created_at），滑块中间态用 `replaceCurrent` 语义不入栈 |
| **D5** | **参数不在库里，只在 sidecar** | `photos.has_edits` 只是布尔 `schema.sql:54`；无 develop settings 表 | 无法"按调整筛选"、无法多副本共享、无法原子备份；sidecar 丢失 = 编辑丢失（与 LR 的库中心相比是反向缺陷） | 库内加 `develop_settings(photo_id, params_json, digest, updated_at)` 作为**缓存/索引**，sidecar 仍是真源（两写一读，冲突以 sidecar mtime 为准） |
| **D6** | **ProcessVersion 是常量，不是契约** | 只写 `kMaxProcessVersion`，读时若高于已知则告警 `xmp_mapper.cpp:433-445,491` | 未来任何一次管线调优都会**静默改变全部老片外观** | 落地 §5 的三条规则；把 PV 写进每次调整 + 库里记录 |
| **D7** | **蒙版/修复用私有命名空间** | `lumina:Masks` / `lumina:Removals` `xmp_mapper.cpp:139-145` | LR 读不到我们的蒙版与修复 → 单向不可逆；我们读 LR 的 `MaskGroupBasedCorrections` 也要额外写解析 | **双写**：同时写 `crs:MaskGroupBasedCorrections`（标准）与 `lumina:Masks`（扩展，携带 LR 表达不了的字段） |
| **D8** | **无预览分层** | 只有 `thumbnail_service`，按 `size` 目录缓存 JPEG `engine/src/thumb/thumbnail_service.cpp:18-19`；无 1:1、无 Smart Preview | 放大查看要解码原片；无离线编辑能力 | 加 1:1 预览层；Smart Preview 是否做取决于 R4 是否松口 |
| **D9** | **无 DCP/相机配置等价物** | 有 `CameraProfile`/`CameraProfileDigest` 字段但无对应数据与查找 | 色彩科学无法对标（§8.3）；`CameraProfileDigest` 写出去是无意义的串 | 见 §8.3 方案 A/B/C，v1.2.0 前定 |
| **D10** | **关键词是单列 TEXT，无层级** | `photos.keywords TEXT` `schema.sql:50` | 不能做 LR 式层级关键词（`AgLibraryKeyword.parent`）；导出到 LR 会丢层级 | 加 `keywords` + `keyword_links` 两表（临时可用 `\|` 分隔 + 迁移） |
| **D11** | **集合无"收藏夹集"（collection set）** | `albums` 无 parent、无 kind 层级 `schema.sql:120-136` | 无法复刻"收藏夹集 + 智能收藏"的树 | `albums` 加 `parent_id` + `kind='set'` |

### 14.3 逐条评估团队要求的三个专项

**(a) 目录库 schema 与 LR 的差距**

| 维度 | LR | LUMINA | 差距性质 |
|---|---|---|---|
| 表数量 | 数十张（Ag* / Adobe_* 两族） | 20 张 | 不致命——LR 的多数表是"性能缓存"与"发布/云"遗留，可裁剪 |
| 图像↔文件解耦 | `Adobe_images` ↔ `AgLibraryFile` ↔ `AgLibraryFolder` ↔ `AgLibraryRootFolder` 四级 | `photos` 单表 + `folders`/`roots` 两级，`photos.file_path` 是全路径 UNIQUE | **结构性**：LUMINA 把"路径"当身份，LR 把"文件记录"当身份。后果：移动/重命名要改 `file_path`（UNIQUE 冲突风险），而 LR 只改 `pathFromRoot` |
| 调整参数存储 | 库内 `s={}` + sidecar | 仅 sidecar | **结构性**（D5） |
| 历史 | 库内快照栈 | 内存栈 | **结构性**（D4） |
| 大载荷外置 | LevelDB（`hasBigData`） | 无（JSON 进 sidecar 或列） | 规模上去后会撞 SQLite 行大小/内存墙 |

**(b) history / snapshot / virtual copy 是否具备**

- virtual copy：**表 + 服务具备**（`virtual_copies` / `VirtualCopyService`），但 `params_json` 是"副本自己的参数"，主记录的参数在 sidecar —— 一致性靠谁保证？需要明确"副本 = 独立 sidecar 文件名（`-2.xmp`）"这类规则，否则会互相覆盖。**部分具备，规则未定。**
- snapshot：**表具备**（`snapshots`），但无对应 RPC/UI 落地证据（`VirtualCopyService` 只有 create/list）。**表在，能力未通。**
- history：**不具备**（D4）。LR 的 history 是"每图 1–396 步的快照栈"，我们是内存栈且重启即失。**结构性缺失。**

**(c) 管线是否按 §4 的顺序实现**

**否。** 逐阶段对照：

| §4 阶段 | LR | LUMINA | 差 |
|---|---|---|---|
| 0–2 解码/校准/配置 | 线性 + DCP | 8-bit sRGB 近似，无 DCP | ✗ 全缺 |
| 3 白平衡 | **第一位**（线性） | **第五位**（对比之后，sRGB 域）`shaders.ts:129-135` | ✗ 位置错 |
| 4 曝光 | 线性 | sRGB 域乘 `pow(2,EV)` `:110` | △ 形式对、空间错 |
| 5–6 对比 + 高光/阴影/白/黑 | 对比在前、HS 在后 | 顺序**相反**（HS 在 `:116-126`，对比在 `:126-128`）；HS 为亮度域平滑蒙版而非局部基 | ✗ 顺序反 + 形式近似 |
| 7 纹理/清晰/去雾 | 频带分离 | "黑场拉伸 + 中间调微对比近似" `:136-141` | ✗ 性质不同 |
| 8–9 曲线 + HSL | LUT / 8 带 | LUT（256×1）`:28,142-147`；8 带（`mat4` 打包）`:178-210` | ✓ 结构对，但**在 sRGB 域** |
| 10 分级 | 4 区 + blend/balance | 字段有，shader 未见独立阶段 | △ |
| 11–15 细节 / 光学 / 几何 / 裁剪 | 五阶段各自成段 | 细节无；光学/几何只写参数不实施 | ✗ 缺整段 |
| 16 蒙版 | 组件树 + 五类 | 6 槽线性/径向/画笔（F4 将解限）+ 无范围蒙版 | △ |
| 17–18 暗角/颗粒 | 输出分辨率 | shader 内有 `uVignette`/`uGrain` `:35-36` | ✓（需确认按输出分辨率求值） |
| 19–20 输出锐化 + 色彩空间 | 有 | 未见 | ✗ |

**结论：20 个阶段里真正对齐的只有 3 个（曲线、HSL 结构、暗角/颗粒），其余 17 个要么缺失、要么空间/顺序错。** 这不是"打磨"能解决的，是**换内核**（D1+D2+D3 必须一起做，因为它们是同一个决定）。

### 14.4 建议的改造顺序（依赖链）

| 波次 | 内容 | 依赖 | 可并行性 |
|---|---|---|---|
| W1 | **D1 线性管线**（CPU 参考 + 工作空间）→ **D2 顺序重排** → **D3 几何/光学阶段** | W1 内部串行 | 不可并行，是地基 |
| W2 | **D9** DCP 等价物 · **D7** 蒙版双写 | 依赖 D1 的工作空间/蒙版求值空间 | 可与 W1 后半并行 |
| W3 | **D5** 库内 develop_settings 缓存 → **D4** history 落盘 → **D6** PV 契约 | 逐级依赖 | 与 W1/W2 完全并行（另一人） |
| W4 | **D8** 预览分层 · **D10/D11** 关键词与集合层级 | 无 | 随时可插入，工程量小 |

**最省事的切入点**：W4（1–2 人日级）能在不动内核的前提下先拿到"库能力对标"的体验提升；但 **D1→D2→D3 不可绕**，越晚做，sidecar 里的历史参数越难迁移（顺序变了，同一数值观感就变——**这正是必须先把 D6 的 PV 契约立起来的原因**）。

---

## 15. 附：本报告未采纳/存疑的项（诚实清单）

| 项 | 状态 | 原因 |
|---|---|---|
| `LR-INV` 任务书路径 `D:/LUMINA/reports/` 与"4,347 张已调整" | **均与实际不符** | 文件实际在 `c:/Users/zj/WorkBuddy/2026-10-21-20-30-19/reports/`；实测是 **4,337**（`LR-INV:69`，脚注亦记 "beforeText 4,337"）。本文按实际引用 |
| Camera Matching 配置数 | 两处冲突 | `395`（target-app-parity:32）vs `481 个机型`（raw-parity:218）。本文写"数百"，不作精确引用 |
| PV = 15.4 具体改了什么 | **未知** | `LR-INV:103` 已记为推断；`REF/process-versions.md` 无该值 |
| `.lrcat-data` LevelDB 键空间 · `-Previews.lrdata` 分片映射 | **未知** | `LR-INV:112` U2 / `:115` U5；后者影响"能否复用 LR 预览"这一优化项 |
| `MJ` 镜头（761 张）身份 · 37 个模型的架构 | **未知** | `LR-INV:111` U1；模型只清点到名字（`REF/target-app-parity.md:33`），未加载、未反编译 |
| LR 管线的"官方"阶段顺序 | **Adobe 未公开** | §4.1 为【知】+【推】，可用"通道串扰实验"或"ACR SDK 调用序列"进一步证伪 |

---

*本报告只写这一个文件，未修改任何其他文件。所有 Adobe 资产（DCP/LCP/预设/模型）未提取、未分发。*
