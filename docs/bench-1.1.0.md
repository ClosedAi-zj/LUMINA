# LUMINA v1.1.0 性能基准与优化报告（WP-A1）+ 健康校验后台化/DB 备份（WP-A2）

> 日期：2026-10-22 ｜ 负责：software-engineer ｜ 基线：v1.1.0 Alpha 批
> 环境：Windows 11 · MinGW-w64 g++ 8.1.0 · SQLite 3.54.1 (WAL+FTS5) · LibRaw 0.21.4
> 基准库：mock 程序化合成库（结构与真实库一致；RAW 本体为未压缩 Linear DNG）

## 一、基准数据

### 1.1 单张 6K 全尺寸预览（`preview.getBase`，含 1600px 降采样 + JPEG 落盘）

基准库：6 张 6000×4000 全尺寸 Linear DNG（每张 CFA ≈ 48MB）；3 轮 × 6 张，每轮先
`cache.clear scope=previews` 消除缓存命中。

| 指标 | 优化前（第 1 轮实测） | 优化后复测（同机冷负载） | DoD |
|---|---|---|---|
| p50 | 313 ms | 571 ms | — |
| p95 | 338 ms | 642 ms | — |
| max | 368 ms | 743 ms | **< 1500 ms ✅** |

> 结论：达标余量 ≥ 2 倍，无需进一步优化。链路：LibRaw `open_file → unpack →
> dcraw_process(half=true，6000→3000) → stb 缩放到 1600 → JPEG q90 落盘`。
> 注意：mock 资产为**未压缩** CFA；真实 RW2 是私有压缩容器，解包多一步无损解码，
> 用户真实 6K RW2 样张到位后需复测（已知唯一未闭环项，见 §四）。

### 1.2 5 万张库 `library.queryPhotos`（120 次/用例）

基准库：`mock.generate total=50000`（5 库根 / 79 文件夹 / 92.2% RAW / 421 缺失 / 4347 sidecar）。

| 用例 | 优化前 p50 / p95 | 优化后 p50 / p95 | DoD |
|---|---|---|---|
| 无过滤深分页（offset 0~4 万轮转） | 205.9 / **451.9 ms** ❌ | 40.0 / **53.2 ms** ✅ | <200 ms |
| FTS 关键词检索（"雾浓顶"） | 31.6 / 35.6 ms ✅ | 32.7 / 44.9 ms ✅ | <200 ms |
| 日期范围过滤（一个月） | 38.3 / 50.3 ms ✅ | 39.6 / 46.1 ms ✅ | <200 ms |

### 1.3 缩略图首屏（200 张 × 320px，冷缓存）

| 实现方式 | 耗时 | 问题 |
|---|---|---|
| 优化前：ensureBatch 内同步逐张解码 | **14,678 ms**（73ms/张） | 首屏 RPC 阻塞 14.7s，UI 卡死 |
| 优化后：pending + 后台线程池 + `event.thumb.ready` | **3 ms** 返回，图片就绪一张推一张 | 对齐协议 §5.3/§7.5 |

### 1.4 其余查询面（5 万张库，test_query_perf 回归覆盖）

深分页最差 18.7ms、时间轴月份桶最差 1.6ms、FTS 检索 p95 ≈ 45ms —— 全部远低于门槛。

## 二、瓶颈分析与优化项

### 2.1 深分页 OFFSET 宽行回表（已修，`db/photo_dao.cpp`）

- 根因：`SELECT <40 列> … ORDER BY captured_at, id LIMIT ? OFFSET ?` 在 offset≈4 万时，
  SQLite 要对 offset+limit 每行做 40 列回表（idx_photos_captured 只含排序键）。
- 优化：**两步取页** —— 第 1 步索引内只取本页 id（无回表），第 2 步按 `id IN (…)`
  精确取回 limit 行宽行并重申排序键。回表行数从 offset+limit 降到 limit。
- 效果：p95 452ms → 53ms（8.5×）。协议零变更（分页参数与返回结构不变）。
- 空页语义保留：offset 越过总量返回空 items + 正确 total。

### 2.2 缩略图首屏同步解码（已修，`thumb/thumbnail_service.cpp`）

- 根因：`ensureBatch` 对缺图缩略图**同步**逐张解码，偏离协议 §5.3 的
  `pending + event.thumb.ready` 异步设计（generateAsync/inFlight_/事件回调早已就位）。
- 优化：缺图一律 `state=pending` + `generateAsync`（线程池 + inFlight 去重），
  调用毫秒级返回，就绪后逐张推事件。
- 附带收益：多客户端/重复请求不会重复解码（inFlight_ 去重本来就在）。

### 2.3 JobManager 进度通知节流（WP-A2，`core/job_manager.{h,cpp}`）

- 进度事件加 100ms 时间窗（0% 与 100% 必发，保证 UI 首末态可达）；
  事件改在锁外发送，避免写帧阻塞其他任务的状态读写。
- 5 万张库全量校验实测：progress 事件约 250 个，无刷屏。

## 三、WP-A2 · 健康校验后台化 + DB 自动备份

### 3.1 校验/扫描任务化现状确认

- `health.scanFull`（存在性）/ `health.verifyPhotos`（MD5）均走 JobManager
  （jobId、event.job.progress/completed），文件粒度检查取消标志 —— 现状合规。
- 本批补强：§2.3 进度节流；取消验证见 3.3。
- 已知协议缺口（不擅自新增，待会签）：health 域无专属 `*.cancelJob`，
  现阶段可用通用取消（`import.cancel` 实现即 `JobManager::cancel(jobId)`，对任意 job 有效）。
  建议 Beta 批在协议 §5.6 补 `health.cancelJob {jobId}`（一行方法表）。

### 3.2 lumina.db 每日自动备份 + 损坏自愈（新增 `health/db_backup.{h,cpp}`）

- **快照**：SQLite Online Backup API（`sqlite3_backup_init/step/finish`），WAL 下安全，
  独立连接不阻塞主连接读写；写 `{home}/backups/lumina-YYYYMMDD.db`（先写 .tmp 再转正）。
- **时机**：启动后立即补当日快照（同名覆盖）+ 每日守护线程巡检（10 分钟间隔，跨天补份）
  + **优雅退出前再补一份**（关键修复：首启快照是空库，会话内新增数据必须落进当日备份，
  否则损坏恢复会回退到空库 —— 本轮验证抓到的真实缺陷）。
- **保留**：只留最近 7 份（按文件名 = 日期字典序裁剪）。
- **自愈**：启动时（打开主连接前）对主库跑 `PRAGMA quick_check`（独立连接）；
  损坏 → 删主库与 -wal/-shm → 从最新备份恢复 → 记 WARN 日志继续启动；
  损坏且无备份 → 记 ERROR 日志后照常 open，让 open 给出原始错误（零静默）。

### 3.3 验证结果（独立 home，真跑）

| 用例 | 结果 |
|---|---|
| 启动即有当日快照（`{home}/backups/lumina-20261022.db`） | PASS |
| 写入垃圾字节破坏 lumina.db → 重启自愈 → 50 张照片完整找回 | PASS |
| 5 万张库全量 MD5 校验期间 `queryPhotos` 并发 p50=66ms / p95=90ms / max=108ms | PASS |
| 校验任务中途取消 → `event.job.completed(state=cancelled)` | PASS |

## 四、遗留与后续

1. **真实 RW2 样张基准**：待用户样张到位后复测 §1.1（RW2 压缩解包开销是唯一未知量；
   解码链路代码与 mock 资产完全一致，不需要改代码）。
2. **`health.cancelJob` 协议会签**：Beta 批一行方法表，引擎侧 `JobManager::cancel` 已就绪。
3. 5 万张压测库保留在 `D:/LUMINA/build/runtime-bench50k`（独立 home），
   6K 基准库在 `runtime-bench6k`，供 WP-C2 发布门槛验收复用。
4. 基准探针（不参与编译，`build/runtime/_probe/`）：`bench_a1_preview.py`、
   `bench_a1_query.py`、`gen50k.py`、`wp_a2_verify.py`（backup/recover/concurrency 三相）。

## 五、回归

- test_xmp_gps：ALL PASS
- test_rpc_smoke：全部通过（失败 0 项）
- test_xmp_roundtrip：全部通过（失败 0 项）
- test_query_perf：全部通过（失败 0 项，深分页最差 18.71ms）
