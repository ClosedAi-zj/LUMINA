// LUMINA MCP 服务器（stdio 传输，零依赖，Node >= 18）
//
// 本进程的 stdin/stdout 跑 MCP 协议（按行分隔的 JSON-RPC 2.0）；
// LUMINA 引擎作为子进程拉起，其私有 stdio 走 4 字节长度前缀帧（见 engine-client.mjs）。
// 两者完全隔离：任何引擎日志都不允许污染 MCP stdout。
//
// 环境变量：
//   LUMINA_ENGINE_PATH  引擎可执行文件路径（默认：仓库 build/engine/lumina-engine.exe）
//   LUMINA_HOME         引擎主目录（默认：引擎自身默认 %USERPROFILE%/.lumina）
//   LUMINA_MCP_TIMEOUT  引擎单请求超时毫秒（默认 120000）
//
// 注意：引擎按主目录单实例加锁。若 LUMINA App 正在打开同一个库，请先关闭 App，
// 或把 LUMINA_HOME 指到独立目录。
import { createInterface } from "node:readline";
import { EngineClient, EngineError, resolveEnginePath } from "./engine-client.mjs";
import { TOOLS } from "./tools.mjs";

const SERVER_INFO = { name: "lumina-mcp", version: "1.0.0" };
const SUPPORTED_PROTOCOL_VERSIONS = ["2025-06-18", "2024-11-05"];

const engine = new EngineClient({
  enginePath: resolveEnginePath(process.env.LUMINA_ENGINE_PATH),
  home: process.env.LUMINA_HOME || "",
  requestTimeoutMs: Number(process.env.LUMINA_MCP_TIMEOUT) || 120000,
  log: (level, line) => process.stderr.write(`[lumina-mcp][${level}] ${line}\n`),
});

// 引擎启动放后台：initialize 必须立即应答（引擎冷启动不应阻塞 MCP 握手）
let engineStartPromise = null;
function ensureEngine() {
  if (!engineStartPromise) {
    engineStartPromise = engine.start().catch((err) => {
      engineStartPromise = null; // 允许下次调用重试
      throw err;
    });
  }
  return engineStartPromise;
}

// ---- 工具调用 ----
function textContent(text) {
  return { content: [{ type: "text", text }], isError: false };
}
function errorContent(message) {
  return { content: [{ type: "text", text: message }], isError: true };
}
function buildToolResult(result) {
  const { _images, ...data } = result ?? {};
  const blocks = [{ type: "text", text: JSON.stringify(data ?? {}, null, 2) }];
  for (const img of _images ?? []) {
    blocks.push({ type: "image", data: img.data, mimeType: img.mimeType });
  }
  return { content: blocks, isError: false };
}
function describeError(err) {
  if (err instanceof EngineError) {
    let text = `引擎错误 ${err.code}: ${err.message}`;
    if (err.data != null) text += `\ndata: ${JSON.stringify(err.data, null, 2)}`;
    if (err.code === -32002) {
      text +=
        "\n提示：若 code=3 通常是引擎单实例锁冲突——LUMINA App 正在打开同一个库，请先关闭 App，" +
        "或为本 MCP 服务设置独立的 LUMINA_HOME。";
    }
    return text;
  }
  return err?.message ?? String(err);
}

async function callTool(name, args) {
  const tool = TOOLS.find((t) => t.name === name);
  if (!tool) return errorContent(`未知工具: ${name}`);
  try {
    await ensureEngine();
    const result = await tool.handler(engine, args ?? {});
    return buildToolResult(result);
  } catch (err) {
    engine.log("error", `tool ${name} failed: ${describeError(err)}`);
    return errorContent(describeError(err));
  }
}

// ---- MCP 消息处理 ----
function handleMessage(msg) {
  // 通知：无需应答
  if (msg.id == null) return null;

  const respond = (payload) => ({ jsonrpc: "2.0", id: msg.id, ...payload });
  const respondError = (code, message, data) =>
    respond({ error: { code, message, ...(data !== undefined ? { data } : {}) } });

  switch (msg.method) {
    case "initialize": {
      const requested = msg.params?.protocolVersion;
      const version = SUPPORTED_PROTOCOL_VERSIONS.includes(requested)
        ? requested
        : SUPPORTED_PROTOCOL_VERSIONS[SUPPORTED_PROTOCOL_VERSIONS.length - 1];
      return respond({
        result: {
          protocolVersion: version,
          capabilities: { tools: { listChanged: false } },
          serverInfo: SERVER_INFO,
        },
      });
    }
    case "ping":
      return respond({ result: {} });
    case "tools/list":
      return respond({
        result: {
          tools: TOOLS.map(({ name, description, inputSchema }) => ({ name, description, inputSchema })),
        },
      });
    case "tools/call": {
      const name = msg.params?.name;
      const args = msg.params?.arguments ?? {};
      // 异步执行，稍后通过回调写出（见主循环）
      return { __async: callTool(name, args) };
    }
    case undefined:
      return respondError(-32600, "missing method");
    default:
      return respondError(-32601, `method not found: ${msg.method}`);
  }
}

// ---- 主循环 ----
const rl = createInterface({ input: process.stdin, terminal: false });
let closing = false;

async function handleLine(line) {
  const trimmed = line.trim();
  if (!trimmed) return;
  let msg;
  try {
    msg = JSON.parse(trimmed);
  } catch {
    write({ jsonrpc: "2.0", id: null, error: { code: -32700, message: "parse error" } });
    return;
  }
  let reply;
  try {
    reply = handleMessage(msg);
  } catch (err) {
    reply = { jsonrpc: "2.0", id: msg?.id ?? null, error: { code: -32603, message: describeError(err) } };
  }
  if (reply == null) return; // 通知
  if (reply.__async) {
    // tools/call：等工具执行完再写出
    try {
      const result = await reply.__async;
      if (!closing) write({ jsonrpc: "2.0", id: msg.id, result });
    } catch (err) {
      if (!closing)
        write({
          jsonrpc: "2.0",
          id: msg.id,
          result: errorContent(describeError(err)),
        });
    }
    return;
  }
  write(reply);
}

function write(msg) {
  process.stdout.write(JSON.stringify(msg) + "\n");
}

rl.on("line", (line) => {
  handleLine(line).catch((err) => {
    engine.log("error", `line handler crashed: ${err?.stack ?? err}`);
  });
});

async function shutdown() {
  if (closing) return;
  closing = true;
  rl.close();
  await engine.shutdown().catch(() => {});
  process.exit(0);
}
rl.on("close", () => shutdown().catch(() => process.exit(0)));
process.on("SIGINT", () => shutdown().catch(() => process.exit(0)));
process.on("SIGTERM", () => shutdown().catch(() => process.exit(0)));

engine.log("info", `lumina-mcp ${SERVER_INFO.version} ready on stdio (${TOOLS.length} tools)`);
