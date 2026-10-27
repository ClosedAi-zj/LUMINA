// LUMINA MCP 服务器 —— GUI 桥接版（v1.3.0-m3）
//
// 架构（双进程桥接）：
//   MCP 客户端（WorkBuddy / Claude Desktop…）
//     │  stdio：JSON-RPC 2.0（官方 @modelcontextprotocol/sdk 承载）
//     ▼
//   本进程：MCP 协议 ↔ LUMINA 控制端口桥接
//     │  TCP 127.0.0.1:{port}，换行分隔 JSON：{id, op, params} → {id, ok, result|error}
//     ▼
//   LUMINA GUI（Electron main：mcp-bridge.ts → renderer 执行器 → zustand store 真实动作）
//
// 端口发现：读 {LUMINA_HOME|~/.lumina}/mcp-port（App 启动时写入；被占端口自动递增），
// 可用 LUMINA_MCP_PORT 环境变量覆盖。**App 未运行时本服务器照常启动**，
// 工具调用会返回清晰的连接错误（绝不挂死）。
//
// 与 legacy-engine-direct/ 的区别：旧版自己 spawn 引擎子进程直连引擎，
// 受引擎单实例锁限制（App 或 MCP 二选一）；本版操作的是**运行中的 GUI**，
// 与用户看到/操作的是同一个会话（当前照片、参数、蒙版、模块实时同步）。
//
// 选型说明：官方 SDK（@modelcontextprotocol/sdk，零手写协议面）——npm 安装可用，
// 规范细节（握手/能力协商/错误码）交给 SDK 维护；旧版零依赖手写实现保留在
// legacy-engine-direct/ 供 headless 直连场景参考。

import os from 'node:os';
import path from 'node:path';
import fs from 'node:fs';
import net from 'node:net';
import { Server } from '@modelcontextprotocol/sdk/server/index.js';
import { StdioServerTransport } from '@modelcontextprotocol/sdk/server/stdio.js';
import {
  ListToolsRequestSchema,
  CallToolRequestSchema,
} from '@modelcontextprotocol/sdk/types.js';

const SERVER_INFO = { name: 'lumina-mcp', version: '2.0.0' };
const DEFAULT_TIMEOUT_MS = 30_000;
const OP_TIMEOUT_MS = { export_photo: 180_000 };

function log(level, msg) {
  process.stderr.write(`[lumina-mcp][${level}] ${msg}\n`);
}

// ---------- 端口发现 ----------

function resolveHome() {
  const env = process.env.LUMINA_HOME;
  if (env && env.trim() !== '') return path.resolve(env.trim());
  return path.join(os.homedir(), '.lumina');
}

function resolvePort() {
  const envPort = process.env.LUMINA_MCP_PORT;
  if (envPort && /^\d+$/.test(envPort.trim())) return Number(envPort.trim());
  const file = path.join(resolveHome(), 'mcp-port');
  try {
    const raw = fs.readFileSync(file, 'utf8').trim();
    const port = Number(raw);
    if (Number.isInteger(port) && port > 0 && port < 65536) return port;
    log('warn', `端口文件内容非法: ${JSON.stringify(raw)} (${file})`);
  } catch {
    /* 文件不存在：常见于 App 未运行 */
  }
  return null;
}

// ---------- 控制端口客户端 ----------

class BridgeError extends Error {}

class BridgeClient {
  constructor() {
    this.socket = null;
    this.nextId = 1;
    this.pending = new Map(); // id -> {resolve, reject, timer}
    this.buf = '';
  }

  disconnect() {
    if (this.socket) {
      try { this.socket.destroy(); } catch { /* ignore */ }
      this.socket = null;
    }
    for (const [, p] of this.pending) {
      clearTimeout(p.timer);
      p.reject(new BridgeError('连接已断开'));
    }
    this.pending.clear();
  }

  connect(port) {
    return new Promise((resolve, reject) => {
      const socket = net.connect(port, '127.0.0.1');
      socket.setEncoding('utf8');
      const onErr = (err) => {
        socket.destroy();
        this.socket = null;
        reject(new BridgeError(`无法连接 LUMINA 控制端口 127.0.0.1:${port}（${err.code ?? err.message}）。LUMINA App 可能未运行，或端口文件已过期。`));
      };
      socket.once('error', onErr);
      socket.once('connect', () => {
        socket.removeListener('error', onErr);
        socket.on('error', (err) => {
          log('error', `控制端口连接错误: ${err.message}`);
          this.disconnect();
        });
        socket.on('data', (chunk) => this.onData(chunk));
        socket.on('close', () => { if (this.socket === socket) this.disconnect(); });
        this.socket = socket;
        resolve(socket);
      });
    });
  }

  onData(chunk) {
    this.buf += chunk;
    let idx;
    while ((idx = this.buf.indexOf('\n')) >= 0) {
      const line = this.buf.slice(0, idx).trim();
      this.buf = this.buf.slice(idx + 1);
      if (line === '') continue;
      let res;
      try { res = JSON.parse(line); } catch { log('warn', `非 JSON 响应行: ${line.slice(0, 120)}`); continue; }
      const entry = this.pending.get(res.id);
      if (!entry) continue;
      this.pending.delete(res.id);
      clearTimeout(entry.timer);
      if (res.ok) entry.resolve(res.result ?? {});
      else entry.reject(new BridgeError(res.error ?? '未知错误'));
    }
  }

  async request(op, params = {}) {
    const port = resolvePort();
    if (port === null) {
      throw new BridgeError(
        `未找到 LUMINA 控制端口（端口文件 ${path.join(resolveHome(), 'mcp-port')} 不存在或非法）。` +
        `请先启动 LUMINA 桌面应用。`,
      );
    }
    if (!this.socket) await this.connect(port);
    const id = this.nextId++;
    const timeoutMs = OP_TIMEOUT_MS[op] ?? DEFAULT_TIMEOUT_MS;
    return new Promise((resolve, reject) => {
      const timer = setTimeout(() => {
        this.pending.delete(id);
        reject(new BridgeError(`请求超时（${Math.round(timeoutMs / 1000)}s）：op=${op}`));
      }, timeoutMs);
      this.pending.set(id, { resolve, reject, timer });
      this.socket.write(`${JSON.stringify({ id, op, params })}\n`);
    });
  }
}

const bridge = new BridgeClient();

// ---------- 工具声明（与 app/src/mcp/executor.ts 的 OPS 一一对应） ----------

const TOOLS = [
  {
    name: 'lumina_status',
    description: 'LUMINA 连接状态与应用概况：引擎模式（mock/real）、当前模块、当前照片、修图会话与图库选择状态。',
    inputSchema: { type: 'object', properties: {}, additionalProperties: false },
  },
  {
    name: 'list_photos',
    description: '查询图库照片列表（分页）。返回 total 与精简条目（id/文件名/格式/相机/星级/初选/拍摄时间/已编辑/缺失）。',
    inputSchema: {
      type: 'object',
      properties: {
        offset: { type: 'number', description: '起始偏移，默认 0' },
        limit: { type: 'number', description: '每页条数 1..200，默认 50' },
        searchText: { type: 'string', description: '全文搜索关键字（可选；提供时走 FTS 搜索）' },
      },
      additionalProperties: false,
    },
  },
  {
    name: 'open_photo',
    description: '在修图台打开指定照片（建立编辑会话并切换到 develop 模块，等同双击图库网格）。',
    inputSchema: {
      type: 'object',
      properties: { photoId: { type: 'number', description: '照片 id（list_photos 返回的 id）' } },
      required: ['photoId'],
      additionalProperties: false,
    },
  },
  {
    name: 'get_develop_params',
    description: '读取当前照片的全部调整参数（白平衡/曝光/对比/曲线/HSL/蒙版栈等）。',
    inputSchema: { type: 'object', properties: {}, additionalProperties: false },
  },
  {
    name: 'set_develop_params',
    description:
      '批量设置当前照片的调整参数（deepMerge 语义：对象递归、数组整体替换）。' +
      '顶层键白名单：processVersion/whiteBalance/temperature/tint/exposure/contrast/highlights/shadows/whites/blacks/' +
      'clarity/texture/dehaze/vibrance/saturation/toneCurveRGB/toneCurveR/toneCurveG/toneCurveB/parametric/hsl/' +
      'colorGrade/calibration/detail/optics/geometry/masks/removals/adjustmentStack。' +
      'exposure 单位 EV（-5..5）；contrast 等 -100..100；temperature 支持开尔文（≥2000 自动归一化）或归一化值（|v|≤100）。未知键会报错。',
    inputSchema: {
      type: 'object',
      properties: { edits: { type: 'object', description: '要合并的参数补丁，如 {"exposure":0.5,"contrast":15}' } },
      required: ['edits'],
      additionalProperties: false,
    },
  },
  {
    name: 'add_mask',
    description: '在当前照片新建局部蒙版（linear 渐变 / radial 径向 / brush 画笔）。可选携带 amount、local（局部调整）与 geometry 覆盖。',
    inputSchema: {
      type: 'object',
      properties: {
        type: { type: 'string', enum: ['linear', 'radial', 'brush'] },
        amount: { type: 'number', description: '强度 -100..100（可选，默认工厂值）' },
        local: {
          type: 'object',
          description: '局部调整（可选）：exposure/contrast/highlights/shadows/temperature/tint/saturation/clarity/dehaze',
        },
        geometry: {
          type: 'object',
          description: '几何覆盖（可选）：linear={x1,y1,x2,y2}；radial={cx,cy,radius,ratio}；brush 留空用默认落点',
        },
      },
      required: ['type'],
      additionalProperties: false,
    },
  },
  {
    name: 'create_snapshot',
    description: '为当前照片创建快照（引擎侧持久化），返回 snapshotId 供 restore_snapshot 使用。',
    inputSchema: {
      type: 'object',
      properties: { name: { type: 'string', description: '快照名，默认「MCP 快照」' } },
      additionalProperties: false,
    },
  },
  {
    name: 'restore_snapshot',
    description: '把当前照片恢复到指定快照的参数状态。',
    inputSchema: {
      type: 'object',
      properties: { snapshotId: { type: 'number', description: 'create_snapshot 返回的快照 id' } },
      required: ['snapshotId'],
      additionalProperties: false,
    },
  },
  {
    name: 'export_photo',
    description:
      '导出当前照片（或指定 photoId）为成品文件。outPath 为目录（按命名模板出文件）或以扩展名结尾的单文件路径。' +
      '引擎导出为异步任务，本工具会等待任务完成（默认上限 120s）。',
    inputSchema: {
      type: 'object',
      properties: {
        outPath: { type: 'string', description: '导出目录，或单文件路径（如 D:/out/DSC_0142.jpg）' },
        format: { type: 'string', enum: ['jpg', 'tiff', 'png', 'psd'], description: '默认 jpg' },
        quality: { type: 'number', description: '1..100，默认 90' },
        longEdge: { type: 'number', description: '长边像素 256..16384，默认 4096' },
        photoId: { type: 'number', description: '可选；缺省导出当前修图台照片' },
        timeoutMs: { type: 'number', description: '等待任务完成上限毫秒，默认 120000' },
      },
      required: ['outPath'],
      additionalProperties: false,
    },
  },
  {
    name: 'lumina_screenshot',
    description: '截取 LUMINA 当前窗口画面为 PNG（AI 操作 GUI 的「眼睛」：参数是否生效、界面是否切换，可用它亲眼确认）。',
    inputSchema: {
      type: 'object',
      properties: { path: { type: 'string', description: 'PNG 落盘绝对路径，如 D:/LUMINA/build/shot.png' } },
      required: ['path'],
      additionalProperties: false,
    },
  },
  {
    name: 'switch_module',
    description: '切换 LUMINA 工作区模块（library 素材库 / develop 修图台 / ingest 导入 / health 健康 / culling 筛选 / map 地图 / timelapse 延时 / landscape 风光 / export 交付 / portfolio 作品集 / gallery 画廊）。',
    inputSchema: {
      type: 'object',
      properties: { module: { type: 'string', enum: ['library', 'develop', 'ingest', 'health', 'culling', 'map', 'timelapse', 'landscape', 'export', 'portfolio', 'gallery'] } },
      required: ['module'],
      additionalProperties: false,
    },
  },
];

// ---------- MCP 服务 ----------

function textResult(text, isError = false) {
  return { content: [{ type: 'text', text }], isError };
}

const server = new Server(SERVER_INFO, { capabilities: { tools: {} } });

server.setRequestHandler(ListToolsRequestSchema, async () => ({
  tools: TOOLS.map(({ name, description, inputSchema }) => ({ name, description, inputSchema })),
}));

server.setRequestHandler(CallToolRequestSchema, async (request) => {
  const { name, arguments: args } = request.params;
  const tool = TOOLS.find((t) => t.name === name);
  if (!tool) return textResult(`未知工具: ${name}`, true);
  try {
    const result = await bridge.request(name, args ?? {});
    return textResult(JSON.stringify(result, null, 2));
  } catch (err) {
    const msg = err instanceof BridgeError ? err.message : (err?.message ?? String(err));
    log('error', `tool ${name} failed: ${msg}`);
    return textResult(msg, true);
  }
});

process.on('SIGINT', () => { bridge.disconnect(); process.exit(0); });
process.on('SIGTERM', () => { bridge.disconnect(); process.exit(0); });

async function main() {
  const transport = new StdioServerTransport();
  await server.connect(transport);
  const port = resolvePort();
  log('info', `started (${SERVER_INFO.name} v${SERVER_INFO.version}); control port ${port !== null ? `127.0.0.1:${port}` : '未发现（App 未运行时工具会报清晰错误）'}`);
}

main().catch((err) => {
  log('error', `fatal: ${err?.stack ?? err}`);
  process.exit(1);
});
