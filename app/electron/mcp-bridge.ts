/**
 * MCP 控制端口（主进程侧，v1.3.0-m3）
 *
 * 架构：LUMINA GUI 是常驻应用，UI 状态（当前照片 / 参数 / 蒙版 / 模块路由）在
 * renderer 的 zustand store 里。MCP 客户端（WorkBuddy 等）会 spawn 独立 stdio
 * 进程，无法直接进 GUI —— 所以采用双进程桥接：
 *
 *   MCP stdio server（mcp/server.mjs）
 *     │  TCP 127.0.0.1:{port}，换行分隔 JSON：请求 {id, op, params}
 *     ▼                              响应 {id, ok, result|error}
 *   本文件（Electron main）
 *     │  webContents.send('lumina:mcp:call') 转发
 *     ▼
 *   renderer 执行器（app/src/mcp/executor.ts）→ 调 store 真实动作 → 原路回传
 *
 * 例外：lumina_screenshot 由本文件直答（capturePage 是主进程独占 API）。
 * 端口发现：监听成功后把实际端口写进 {homeDir}/mcp-port 文件；
 * 被占则从 27890 起递增重试。App 退出时删除该文件。
 */

import net from 'node:net';
import path from 'node:path';
import fs from 'node:fs';
import { ipcMain } from 'electron';

const MCP_PORT_BASE = 27890;
const MCP_PORT_MAX_TRIES = 32;
/** 默认单请求超时；export_photo 由 renderer 侧轮询导出任务，整体给长窗口 */
const DEFAULT_TIMEOUT_MS = 30_000;
const OP_TIMEOUT_MS: Record<string, number> = { export_photo: 180_000 };

interface McpRequest {
  id: number | string;
  op: string;
  params?: unknown;
}

interface PendingCall {
  resolve: (value: { ok: boolean; result?: unknown; error?: string }) => void;
  timer: ReturnType<typeof setTimeout>;
}

let server: net.Server | null = null;
let actualPort: number | null = null;
let portFilePath: string | null = null;
let nextReqId = 1;
const pending = new Map<number, PendingCall>();

function log(msg: string): void {
  process.stdout.write(`[mcp-bridge] ${msg}\n`);
}

/** 转发给 renderer 执行器并等待回传；窗口未就绪立即回错误，绝不挂死 */
function forwardToRenderer(wc: Electron.WebContents | null, req: McpRequest): Promise<{ ok: boolean; result?: unknown; error?: string }> {
  const reqId = nextReqId++;
  return new Promise((resolve) => {
    const timeoutMs = OP_TIMEOUT_MS[req.op] ?? DEFAULT_TIMEOUT_MS;
    const timer = setTimeout(() => {
      pending.delete(reqId);
      resolve({ ok: false, error: `执行超时（${Math.round(timeoutMs / 1000)}s）：op=${req.op}` });
    }, timeoutMs);
    pending.set(reqId, { resolve, timer });

    if (!wc || wc.isDestroyed()) {
      clearTimeout(timer);
      pending.delete(reqId);
      resolve({ ok: false, error: 'LUMINA 界面尚未就绪（renderer 未加载），请稍后重试' });
      return;
    }
    wc.send('lumina:mcp:call', { reqId, op: req.op, params: req.params ?? {} });
  });
}

/** renderer 回传入口（preload: lumina.mcp.resolveMcpCall → ipcRenderer.send） */
function onRendererResult(payload: { reqId: number; ok: boolean; result?: unknown; error?: string }): void {
  const entry = payload && typeof payload.reqId === 'number' ? pending.get(payload.reqId) : undefined;
  if (!entry) return; // 已超时/未知请求：pending 已在超时路径清理，此处直接丢弃
  pending.delete(payload.reqId);
  clearTimeout(entry.timer);
  entry.resolve({ ok: !!payload.ok, result: payload.result, error: payload.error });
}

/** lumina_screenshot：把当前窗口合成帧落 PNG（AI 操作 GUI 的「眼睛」，main 直答） */
async function opScreenshot(req: McpRequest, wc: Electron.WebContents | null, reply: (obj: unknown) => void): Promise<void> {
  const params = (req.params ?? {}) as { path?: unknown };
  const out = typeof params.path === 'string' && params.path.trim() !== '' ? params.path.trim() : '';
  if (out === '' || !path.isAbsolute(out) || !out.toLowerCase().endsWith('.png')) {
    reply({ id: req.id, ok: false, error: '参数 path 必填且必须是绝对路径的 .png 文件' });
    return;
  }
  if (!wc || wc.isDestroyed()) {
    reply({ id: req.id, ok: false, error: 'LUMINA 界面尚未就绪（renderer 未加载）' });
    return;
  }
  try {
    const img = await wc.capturePage();
    if (img.isEmpty()) {
      reply({ id: req.id, ok: false, error: '截到空帧（窗口尚未完成合成），请稍后重试' });
      return;
    }
    fs.mkdirSync(path.dirname(out), { recursive: true });
    fs.writeFileSync(out, img.toPNG());
    const sz = img.getSize();
    reply({ id: req.id, ok: true, result: { path: out, width: sz.width, height: sz.height } });
  } catch (err) {
    reply({ id: req.id, ok: false, error: `截图失败: ${String(err)}` });
  }
}

/** 处理一条来自 TCP 客户端的请求：lumina_screenshot 本地直答，其余转发 renderer */
function handleLine(raw: string, wc: () => Electron.WebContents | null, reply: (obj: unknown) => void): void {
  const line = raw.trim();
  if (line === '') return;
  let req: McpRequest;
  try {
    req = JSON.parse(line) as McpRequest;
  } catch {
    reply({ id: null, ok: false, error: 'JSON 解析失败' });
    return;
  }
  if (typeof req.op !== 'string' || req.op === '') {
    reply({ id: req.id ?? null, ok: false, error: '缺少 op 字段' });
    return;
  }
  if (req.op === 'lumina_screenshot') {
    void opScreenshot(req, wc(), reply);
    return;
  }
  void forwardToRenderer(wc(), req).then((res) => {
    reply(res.ok ? { id: req.id, ok: true, result: res.result ?? {} } : { id: req.id, ok: false, error: res.error ?? '未知错误' });
  });
}

/**
 * 启动控制端口。端口从 27890 递增试探，写 {homeDir}/mcp-port；
 * 失败不抛出（打日志），绝不能因为 MCP 端口问题挡住 GUI 启动。
 */
export function startMcpBridge(opts: { homeDir: string; wc: () => Electron.WebContents | null }): void {
  if (server) return;
  ipcMain.on('lumina:mcp:call-result', (_e, payload: unknown) => onRendererResult(payload as never));

  const tryListen = (port: number): Promise<boolean> =>
    new Promise((resolve) => {
      const s = net.createServer((socket) => {
        let buf = '';
        socket.setEncoding('utf8');
        socket.on('data', (chunk: string) => {
          buf += chunk;
          let idx: number;
          while ((idx = buf.indexOf('\n')) >= 0) {
            const line = buf.slice(0, idx);
            buf = buf.slice(idx + 1);
            handleLine(
              line,
              opts.wc,
              (obj) => socket.write(`${JSON.stringify(obj)}\n`),
            );
          }
        });
        socket.on('error', () => {/* 客户端断开等 IO 错误：连接级，不关服务 */});
      });
      s.once('error', () => resolve(false));
      s.listen(port, '127.0.0.1', () => {
        server = s;
        resolve(true);
      });
    });

  void (async () => {
    for (let i = 0; i < MCP_PORT_MAX_TRIES; i++) {
      const port = MCP_PORT_BASE + i;
      const bound = await tryListen(port);
      if (bound) {
        actualPort = port;
        portFilePath = path.join(opts.homeDir, 'mcp-port');
        try {
          fs.mkdirSync(opts.homeDir, { recursive: true });
          fs.writeFileSync(portFilePath, `${port}\n`, 'utf8');
        } catch (err) {
          log(`端口文件写入失败: ${String(err)}`);
        }
        log(`control port listening on 127.0.0.1:${port} (file: ${portFilePath})`);
        return;
      }
    }
    log(`ERROR: ${MCP_PORT_BASE}-${MCP_PORT_BASE + MCP_PORT_MAX_TRIES - 1} 端口全部被占，MCP 控制端口未启动`);
  })();
}

/** App 退出前调用：关端口、删发现文件、清 pending */
export function stopMcpBridge(): void {
  for (const [reqId, entry] of pending) {
    clearTimeout(entry.timer);
    entry.resolve({ ok: false, error: 'LUMINA 正在退出' });
    pending.delete(reqId);
  }
  if (server) {
    try {
      server.close();
    } catch {
      /* 已关则忽略 */
    }
    server = null;
  }
  if (portFilePath) {
    try {
      fs.rmSync(portFilePath, { force: true });
    } catch {
      /* 删不掉不阻塞退出 */
    }
    portFilePath = null;
  }
  actualPort = null;
}

/** 供主进程其他模块/诊断读取当前端口 */
export function mcpBridgePort(): number | null {
  return actualPort;
}
