# LUMINA MCP 服务器（GUI 桥接版 · v1.3.0-m3）

让 MCP（Model Context Protocol）AI 助手**直接操作运行中的 LUMINA 桌面应用**——
切模块、翻图库、开照片、调参数、建蒙版、拍快照、导出成片，全部作用于用户眼前
的同一个界面会话（不另起引擎、不绕过 UI 状态）。

## 架构

```
AI agent（WorkBuddy / Claude Desktop / 任意 MCP 客户端）
   │  MCP 协议：stdio 上的 JSON-RPC 2.0（官方 @modelcontextprotocol/sdk）
   ▼
server.mjs（本进程）—— 协议 ↔ 控制端口桥接
   │  TCP 127.0.0.1:{port}，换行分隔 JSON：{id, op, params} → {id, ok, result|error}
   ▼
LUMINA 桌面应用（Electron）
   main：app/electron/mcp-bridge.ts（监听端口，转发 webContents）
   renderer：app/src/mcp/executor.ts（执行器 → zustand store 真实动作）
```

### 端口发现

- LUMINA 启动时在 `127.0.0.1:27890` 起监听（被占则递增），并把实际端口写入
  `{home}/mcp-port` 文件；App 退出时删除该文件。
- `{home}` 与 App 主目录一致：默认 `~/.lumina`（打包版为 userData/library），
  可用 `LUMINA_HOME` 覆盖。
- 本 MCP 服务器**总是能启动**；App 未运行时工具调用返回清晰错误（不挂死）。

## 接入 WorkBuddy

在 `~/.workbuddy-ai/mcp.json` 的 `mcpServers` 中加入：

```json
{
  "mcpServers": {
    "lumina": {
      "command": "node",
      "args": ["D:/LUMINA/mcp/server.mjs"]
    }
  }
}
```

保存后到「连接器管理」对新出现的 lumina 服务器点「信任」即可生效。
（需要自定义主目录时加 `env: { "LUMINA_HOME": "..." }`，一般不必。）

## 接入 Claude Desktop

`claude_desktop_config.json`：

```json
{
  "mcpServers": {
    "lumina": {
      "command": "node",
      "args": ["D:/LUMINA/mcp/server.mjs"]
    }
  }
}
```

## 工具（10 个）

| 工具 | 作用 |
| --- | --- |
| `lumina_status` | 引擎模式 / 当前模块 / 当前照片 / 会话与选择状态 |
| `list_photos` | 图库查询（分页，可选全文搜索） |
| `open_photo` | 修图台打开照片（建立编辑会话，等同双击网格） |
| `get_develop_params` | 读当前照片全部调整参数 |
| `set_develop_params` | 批量合并调整参数（deepMerge；未知键报错） |
| `add_mask` | 新建 linear / radial / brush 蒙版 |
| `create_snapshot` | 创建快照，返回 snapshotId |
| `restore_snapshot` | 恢复到快照 |
| `export_photo` | 走既有导出链（异步任务，等待完成） |
| `lumina_screenshot` | 截取当前窗口 PNG（亲眼确认 UI 响应） |
| `switch_module` | 切换工作区模块 |

## 环境变量

| 变量 | 说明 |
| --- | --- |
| `LUMINA_HOME` | App 主目录（端口文件查找根），默认 `~/.lumina` |
| `LUMINA_MCP_PORT` | 直接指定控制端口（跳过端口文件），排障用 |

## 依赖与选型

- 依赖：`@modelcontextprotocol/sdk`（npm 安装；协议握手 / 能力协商 / 传输层交由
  官方 SDK 维护）。Node >= 18。
- 历史版本（自 spawn 引擎子进程、零依赖手写协议）归档于 `legacy-engine-direct/`：
  那版受引擎单实例锁限制（App 或 MCP 二选一），已被本 GUI 桥接版取代。

## 排障

- **工具报「未找到 LUMINA 控制端口」**：App 没开，或端口文件过期 → 启动 LUMINA。
- **工具报「LUMINA 界面尚未就绪」**：窗口还在加载，稍后重试。
- **连接被拒 / 端口对不上**：检查 `{home}/mcp-port` 内容与 `netstat -ano | findstr 27890`；
  或用 `LUMINA_MCP_PORT` 显式指定。
