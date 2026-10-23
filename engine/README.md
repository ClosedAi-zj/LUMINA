# LUMINA 引擎（lumina-engine）

C++17 单进程引擎，通过 stdio 与 Electron UI 以 JSON-RPC 2.0 通信（4 字节小端长度前缀帧，UTF-8 JSON，单帧上限 8 MiB）。契约文档：`D:/LUMINA/docs/ipc-protocol.md`（**v1.0.1 为准**，字段名逐字一致）。

## 构建

```powershell
# 一键（自动配置 MinGW/CMake/Ninja 路径）
powershell -File D:/LUMINA/scripts/build-engine.ps1

# 或手动
powershell -File D:/LUMINA/scripts/env.ps1
cmake -S D:/LUMINA/engine -B D:/LUMINA/build/engine -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build D:/LUMINA/build/engine
```

产物：`D:/LUMINA/build/engine/lumina-engine.exe`（静态链接 libgcc/libstdc++，无运行时 DLL 依赖）。

## 运行

```powershell
D:/LUMINA/build/engine/lumina-engine.exe --home D:/LUMINA/build/runtime/home
```

- 默认 home：`%USERPROFILE%/.lumina`；目录布局 `db/ cache/ logs/ gallery/`
- 单实例锁：同 home 只允许一个引擎进程（`CreateFileW` + `FILE_FLAG_DELETE_ON_CLOSE`）
- 启动即握手：进程起来后主循环立即读帧等请求；可被随时 kill，重启无残留状态
- **stdout 只输出协议帧**；日志一律 stderr + `{home}/logs/engine-YYYYMMDD.log`

## 测试

```powershell
D:/LUMINA/build/engine/test_xmp_roundtrip.exe   # XMP 字节保留 round-trip（7 用例）
D:/LUMINA/build/engine/test_rpc_smoke.exe       # RPC 冒烟（15 用例）
D:/LUMINA/build/engine/test_query_perf.exe      # 查询性能（6000 条，均 <100ms）
```

## 模块地图（src/）

| 目录 | 职责 |
|---|---|
| `rpc/` | 帧编解码、Dispatcher、RpcServer 主循环、方法注册（组合根） |
| `json/` | 自研 JSON（保序 object、mergePatch 深合并、UTF-8 直通） |
| `core/` | 路径/文件工具、MD5、日志、线程池、配置、数据模型、JobManager |
| `db/` | SQLite（WAL + FTS5 外部内容表 + 触发器）、PhotoDao/AlbumDao/HealthDao |
| `library/` | 查询引擎、文件夹树、智能集合 |
| `imaging/` | stb 图像编解码、缩放、直方图、近似色调管线 |
| `thumb/` | 缩略图服务（LibRaw 优先，内嵌代理兜底，LRU 配额） |
| `raw/` | LibRaw 真解码、内嵌代理提取、`raw.selfTest` L1 自检 |
| `metadata/` | EXIF（JPEG/PNG/TIFF）、视频容器探测 |
| `xmp/` | **字节保留 sidecar 编辑器（红线 R1）**、字段映射、读写服务、迁移器 |
| `edit/` | 历史栈、编辑会话（500ms 防抖落盘）、虚拟拷贝 |
| `import/` | 重命名模板、来源扫描、导入去重 |
| `health/` | 全量扫描、MD5 校验、ReadDirectoryChangesW 实时监测、备份恢复 |
| `mock/` | 模拟图库生成器（见下） |

## 模拟图库（重要）

`mock.generate` 生成 18,192 条记录 + 影像文件，复刻真实基准分布（5 根 / 79 文件夹 / RW2 占 92.2% / 缺失 421 / 已调整 4,347 其中 83.8% 含局部蒙版 / GPS 137 等）。

**影像本体为结构合法的未压缩 Linear DNG**（TIFF-EP：IFD + SubIFD 内嵌 JPEG 预览，PhotometricInterpretation=32803 CFA，CFAPattern RGGB，BitsPerSample=16，Compression=1）。原因：Panasonic RW2 是私有压缩容器，无法程序化构造；索引层元数据仍按真实基准标注 `DC-S5M2 / RW2`，供检索/筛选/统计验证。

**接真实图库时 LibRaw 直接解码 RW2，引擎代码无需任何改动**（`raw/raw_decoder.cpp` 对 RW2/DNG 走同一条 `open_file → unpack → dcraw_process → dcraw_make_mem_image` 通路）。

生成完成后自动执行 `raw.selfTest`（LibRaw 真解码抽样，100% 成功且输出均值 ∈ [16,240]），结果见 `mock.status` 的 `lastSelfTest`；图库根目录自动写 `README.md` 说明上述事项。

## 关键设计决策

1. **schema.sql 是唯一事实源**：CMake 配置期读入并生成 `schema_sql.inc`（C++ raw string），避免手写副本漂移。
2. **XMP 字节保留（红线 R1）**：解析时记录每个节点的原始字节区间；写回只替换被修改节点的区间，未触碰节点（含 lrt:、未知命名空间）逐字节原样输出；temp+rename 原子写；mtime 外部修改检测返回 1005，绝不覆盖。
3. **LUMINA 蒙版模型是 crs: 的超集**：masks/removals 存 `lumina:Masks`/`lumina:Removals` JSON 块，避免污染既有 LR sidecar（已知会与 UI 工程师同步）。
4. **EditParams 用 JsonValue 表示**：Partial patch 经 `mergePatch` 深合并，避免巨型 C++ 结构体。
5. **零静默**：文件缺失/读失败/MD5 不符/外部冲突，一律进异常清单（`health_issues` 表）并可查询，或返回明确错误码。
6. **未实现领域（导出/延时/堆栈/降噪/地图/画廊）**：统一返回 `-32603` + `data:{method, plannedBatch}`，绝不静默失败。
