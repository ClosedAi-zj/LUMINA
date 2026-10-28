# LUMINA v1.2.3 发布说明

稳定性修复批次 + 体积优化 + 补齐「幻灯片放映」。版本号统一为 **1.2.3**
（`engine/src/rpc/version.h`、`app/package.json`、`package-lock.json`、`scripts/installer.nsi`、`package-app.ps1` 全部对齐）。

---

## 一、修复（本批次全部为缺陷修复，无新增格式/算法）

### 引擎侧（29 个文件）

| 编号 | 缺陷 | 后果（修复前） |
|---|---|---|
| E1 / E20 | `EditSessionManager` 只有「500ms 防抖 tick」一条落盘路径，且 tick 收尾用**毫秒时间戳**判断 dirty | 同毫秒内到达的新变更被判为「无新变更」，用户的最后一次拖动在会话销毁时丢失 |
| E22 | 退出流程把多个清理动作放进同一 try | 任一步抛异常，`pool.shutdown()` / `database.close()` / `releaseLock()` 会被跳过 |
| E24 | `end(flush=true)` 无条件上报 `flushed:true` | 写盘期间到达的 `setParams` 没进 sidecar，UI 却显示「已落盘」，会话随后销毁 |
| E16 | 导入已有 XMP 的照片首次编辑必报「外部文件冲突」 | 首次编辑被误拦截 |
| E8 / E12 / E13 | job 失败不下发错误码、restoreFromBackup 伪装成异步、orphan 分支吞掉告警 | 任务失败界面零反馈，用户不知道到底做没做成 |
| E4 / E5 / E19 | 空数组参与 SQL 筛选、PRAGMA 不查返回值、close 不查 rc | 筛选结果不正确、配置未生效无提示 |
| E7 / E10 / E11 | 缩略图去重键漏尺寸、`randomHex` 非线程安全、线程池在锁内执行任务 | 并发丢失 / 竞态 / 锁竞争 |
| E14 / E17 | `encodePng/encodeJpeg` 走 printf 式路径、`getFolderTree` 吞 kDbError | 中文路径导出失败、目录树失败无错误码 |
| E6 / E9 / E18 | 延时 writeSidecar 不持写锁、XMP 顶层未知键静默丢弃、`status()` 有副作用 | 潜在竞态 / 排障无从下手 |

关键实现要点：

- E20 用**单调序号** `changeSeq` / `flushedSeq` 取代毫秒比较（时间戳在同一毫秒会相等，`system_clock` 还可能被回拨）。
- E24 抽出 `flushUntilClean(session, err, maxRounds=3)`，`FlushOutcome` 三态区分 `kClean` / `kWriteFailed` / `kPending`；
  **`flushed` 只在真的很干净时才为 true**，不再无条件上报成功。
- E22 拆成 10 段独立 `cleanupStep`，每段自己的 try/catch，`Logger::shutdown()` 压在最后。

### 前端侧（42 个文件，含 P/A/B 三组）

- **B8（最关键）**：Electron 的 `ipcMain.handle` 抛错经 `invoke` 后只剩 `message`，自定义 `code` 丢失，
  渲染层恒收到 `-32603` + `Error invoking remote method 'lumina:rpc'` 前缀。
  改为结构化错误信封 `__luminaRpcError`，**「sidecar 已被外部修改」这类 1005 提示第一次对用户可见**。
- **A2**：XMP 指示灯 `written` 只在 `endSession{flush:true}` 且 `sidecarMtime !== null` 之后点亮，中间态显示「已提交 · 待落盘」，不再是假绿。
- **B10**：乐观更新的成功回写加 revision 守卫，迟到的成功响应不会把更晚的失败状态覆盖掉。
- **P1 / A5**：`openSeq` 令牌避免异步打开竞态导致旧会话覆盖新会话。
- **P7 / P8 / A4**：缩略图请求键带尺寸、未定义筛选条件改为删键、失败按 id 单张回滚。
- **P2**：曲线控件改为严格受控，切走再切回不再残留旧控制点。
- **mock 契约对齐（B9 / B11）**：mock 的空栈与会话缺失错误与真引擎逐字一致（`-32602` + `nothing to undo` / `session not found`），
  `endSession` 不再静默返回 `{}`。这让上述错误分支在 mock 模式下第一次真正可测。

---

## 二、新增功能：幻灯片放映

模块 tab「幻灯片」此前是占位页，v1.2.3 补齐为可用功能：

- 序列取自**图库当前结果**；选中 ≥2 张时只放映选中的那些
- 全屏播放 / 空格暂停 / ←→ 切换 / Esc 退出，停留时间 2/4/8/15s 可调
- 画面区固定 `#08090B`（与其它照片观察区同口径，浅色主题下不翻转）
- 只预取前方 2 张预览；**标签页切到后台时暂停计时**，不做额外解码开销
- 零静默：空序列、预览生成失败（含重试/跳过）、序列被截断都会显式告知

**仍未实现的模块**：书籍排版、Web 画廊依旧是显式占位页；全景拼接 / 景深堆栈 / AI 降噪属规划中的后续里程碑（见下）。

---

## 三、体积与速度

| 项 | v1.2.1 | v1.2.3 | 变化 |
|---|---|---|---|
| 引擎可执行文件 | 8.7 MB | **6.46 MB** | −26%（Release 追加 `-s` strip；测试二进制刻意保留符号以便回溯） |
| Electron 语言 pak | 90+ 个 / 41 MB | **2 个** | 仅留 zh-CN + en-US，其余 53 个移除，缺失语言自动回退英文 |
| 打包目录 | 277 MB | **236 MB** | −41 MB |
| portable zip | 112.7 MB | **102.4 MB** | −9% |
| setup.exe | 86.0 MB | **75.5 MB** | −12% |

测试侧也顺带变快：`edit_flush_on_tick` **49.3s → 15.0s**（把固定 `sleep` 换成有界轮询，值一到就退出，不用死等预算），
全量 ctest **321s → 220s**。

---

## 四、验证（均为实测，非推断）

- 引擎 **39/39 全绿**：`100% tests passed, 0 tests failed out of 39`，`Total Test time = 220.12s`
- 前端 `tsc --noEmit` 零错误；`npm run build` / `build:electron` 均通过
- 发布包端到端冒烟（`scripts/smoke-packaged.py`）：**SMOKE PASS** —— 临时 home 启动 exe → 引擎拉起 → 控制口真调 RPC → 正常退出

---

## 五、已知限制（如实登记，不粉饰）

1. **E20 没有回归用例覆盖。** 我曾要求负向对照证明新测试「抓得住这个回退」，实测结果是抓不住：
   把 `tick()` 收尾临时退回无条件 `dirty = false` 后，`edit_flush_on_tick` **仍然全绿**（8/8 轮正常落盘，耗时 510–545ms）。
   原因是插入点稳定落在收尾**之后**，根本没进入竞态窗口。此前观察到的那次变红属于时序侥幸，不能作为覆盖证据。
   需要在引擎侧提供测试接缝（可控延时/暂停的写盘）才能做出确定性用例。
2. **E24 的 `kPending` 分支零覆盖。** 它的主行为路径（`kClean`）由 T4c 覆盖，但「补刷 3 轮仍追不平」
   这个防御分支没有被任何用例触发过。
3. **幻灯片模块通过类型检查与构建，但未做 UI 运行实测。** 本轮两个验证代理因账号限流下线，
   UI 冒烟需要 mock 数据 + 截图对照，未能完成。它被 `ErrorBoundary` 包裹且需用户主动点击进入，最坏情况是显示错误卡片。
4. **新测试文件尚未入库基线。** 4 个新测试文件在本批次首次提交，此前无法用 `git diff` 机证「断言未被放宽」。
5. **Photoshop 2026 视觉验收（M3-04）仍未做**，本机无 Photoshop。
6. **AI 降噪目前是 NLM + 亮度蒙版**，不是真正的 AI 模型。

---

## 六、排期中的后续项

- 给引擎加测试接缝 → 补齐 E20 / E24 `kPending` 的确定性回归用例
- `build/runtime` 按 PID 隔离测试临时目录（`engine/tests/testenv.h` + `LUMINA_TEST_ROOT`，15 个用例各改 2 行）
- 全景拼接 / 景深堆栈 / AI 降噪：需独立里程碑。三者都各自是一条技术链
  （对齐算法 / 接缝 blending / 模型推理 + 素材素材成分离层），不是一个批次能交付的改动。
- 书籍排版模块、Web 画廊模块
