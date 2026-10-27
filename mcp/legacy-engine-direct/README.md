# LUMINA MCP 服务器

让任意支持 MCP（Model Context Protocol）的 AI agent 直接连上 LUMINA 引擎：
导入照片、整理图库、显影编辑、分析直方图、导出成片、检查库健康——全部由 agent 驱动。

零依赖（Node >= 18，无 npm install），stdio 传输，引擎作为子进程自托管。

## 架构

```
AI agent (WorkBuddy / Claude Desktop / 任意 MCP 客户端)
   │  MCP: 按行分隔 JSON-RPC 2.0 (stdio)
   ▼
server.mjs  ── tools.mjs（28 个精选工具 + JSON Schema）
   │  引擎私有 stdio: 4 字节小端长度前缀 + JSON 帧（docs/ipc-protocol.md）
   ▼
lumina-engine.exe（子进程，SQLite + LibRaw + NodeGraph 管线）
```

本进程 stdout 只跑 MCP 协议；引擎日志全部走 stderr，绝不混流。
引擎按主目录单实例加锁：**同一时刻同一图库只能有一个引擎**（App 或 MCP 二选一，见下）。

## 接入 WorkBuddy

在 `~/.workbuddy-ai/mcp.json` 的 `mcpServers` 中加入（已自动加入的可跳过）：

```json
{
  "mcpServers": {
    "lumina": {
      "command": "node",
      "args": ["D:/LUMINA/mcp/server.mjs"],
      "env": {
        "LUMINA_HOME": "C:/Users/zj/AppData/Roaming/LUMINA/library",
        "LUMINA_ENGINE_PATH": "D:/LUMINA/build/engine/lumina-engine.exe"
      }
    }
  }
}
```

保存后到「连接器管理」页顶部自定义连接器入口，对新出现的 lumina 服务器点「信任」即可生效。

## 接入 Claude Desktop

`claude_desktop_config.json`：

```json
{
  "mcpServers": {
    "lumina": {
      "command": "node",
      "args": ["D:/LUMINA/mcp/server.mjs"],
      "env": { "LUMINA_HOME": "C:/Users/zj/AppData/Roaming/LUMINA/library" }
    }
  }
}
```

## 环境变量

| 变量 | 说明 | 默认 |
| --- | --- | --- |
| `LUMINA_ENGINE_PATH` | 引擎可执行文件路径 | 仓库 `build/engine/lumina-engine.exe` |
| `LUMINA_HOME` | 图库主目录（db/cache/exports 都在其下） | 引擎默认 `%USERPROFILE%/.lumina` |
| `LUMINA_MCP_TIMEOUT` | 引擎单请求超时毫秒 | `120000` |

**单实例锁**：引擎对同一 `LUMINA_HOME` 只允许一个实例。若 agent 报
`引擎错误 … code=3` / 进程退出 code=3，说明 LUMINA App 正开着同一个库——
先关 App，或给 MCP 配一个独立目录做沙盒库。

## 工具清单（28 个）

| 领域 | 工具 |
| --- | --- |
| 系统 | `lumina_system_info` `lumina_job_status` `lumina_job_cancel` |
| 整理 | `lumina_library_stats` `lumina_library_query` `lumina_library_search` `lumina_library_facets` `lumina_folder_tree` `lumina_photo_detail` `lumina_photo_update` `lumina_photo_remove` `lumina_photo_restore` |
| 相册 | `lumina_album_list` `lumina_album_create` `lumina_album_add_photos` `lumina_album_remove_photos` |
| 导入 | `lumina_import_scan` `lumina_import_check_target` `lumina_import_start` |
| 显影 | `lumina_edit_photo`（一站式编辑）`lumina_develop_render` `lumina_preview_get`（可回传图片给 agent 看）`lumina_presets_list` `lumina_presets_apply` |
| 分析 | `lumina_histogram` `lumina_exposure_check` |
| 导出 | `lumina_export_create` `lumina_export_list` |
| 健康 | `lumina_health_status` `lumina_health_scan` |

设计要点：

- **长任务自动等待**：导入/扫描/导出/健康扫描等 job 类工具默认等到终态才返回（`wait: false` 可关），
  超时则返回当前进度并提示用 `lumina_job_status` 续查。
- **agent 可看图**：`lumina_preview_get` / `lumina_develop_render` 传 `returnImage: true`
  会把 JPEG/PNG 以 MCP image content 回传（有 token 开销，按需开）。
- **安全边界**：`lumina_photo_remove` 是软删除（可 `lumina_photo_restore`）；导出只读原片绝不改写。

## 开发与验证

```bash
node mcp/test/smoke.mjs   # 隔离临时库全链路冒烟（MCP 握手/工具清单/真实引擎调用/优雅退出）
```
