// LUMINA MCP 冒烟测试：驱动 server.mjs 的 stdio，验证 MCP 握手、工具清单与真实引擎调用。
// 使用隔离的临时 LUMINA_HOME，绝不触碰用户真实图库，也避免与运行中的 App 抢单实例锁。
import { spawn } from "node:child_process";
import { mkdtempSync, rmSync } from "node:fs";
import { tmpdir } from "node:os";
import path from "node:path";
import { fileURLToPath } from "node:url";

const here = path.dirname(fileURLToPath(import.meta.url));
const serverPath = path.join(here, "..", "server.mjs");
const home = mkdtempSync(path.join(tmpdir(), "lumina-mcp-smoke-"));

const child = spawn(process.execPath, [serverPath], {
  stdio: ["pipe", "pipe", "inherit"],
  env: { ...process.env, LUMINA_HOME: home },
});

let nextId = 0;
const pending = new Map();
let buf = "";

child.stdout.on("data", (chunk) => {
  buf += chunk.toString("utf8");
  let idx;
  while ((idx = buf.indexOf("\n")) >= 0) {
    const line = buf.slice(0, idx).trim();
    buf = buf.slice(idx + 1);
    if (!line) continue;
    const msg = JSON.parse(line);
    const entry = pending.get(msg.id);
    if (entry) {
      pending.delete(msg.id);
      entry(msg);
    }
  }
});

function rpc(method, params, timeoutMs = 60000) {
  const id = ++nextId;
  return new Promise((resolve, reject) => {
    const timer = setTimeout(() => {
      pending.delete(id);
      reject(new Error(`timeout waiting for ${method}`));
    }, timeoutMs);
    pending.set(id, (msg) => {
      clearTimeout(timer);
      if (msg.error) reject(new Error(`${method} → error: ${JSON.stringify(msg.error)}`));
      else resolve(msg.result);
    });
    child.stdin.write(JSON.stringify({ jsonrpc: "2.0", id, method, params }) + "\n");
  });
}

function assert(cond, label) {
  if (!cond) throw new Error(`断言失败: ${label}`);
  console.log(`  ok: ${label}`);
}

let failures = 0;
try {
  // 1. MCP initialize 握手
  const init = await rpc("initialize", {
    protocolVersion: "2024-11-05",
    capabilities: {},
    clientInfo: { name: "smoke", version: "0" },
  });
  assert(init.protocolVersion === "2024-11-05", `initialize 协议版本 ${init.protocolVersion}`);
  assert(init.serverInfo?.name === "lumina-mcp", "serverInfo.name");
  child.stdin.write(JSON.stringify({ jsonrpc: "2.0", method: "notifications/initialized" }) + "\n");

  // 2. ping
  await rpc("ping", {});
  console.log("  ok: ping");

  // 3. tools/list
  const list = await rpc("tools/list", {});
  assert(Array.isArray(list.tools) && list.tools.length >= 25, `tools/list 返回 ${list.tools.length} 个工具`);
  const names = new Set(list.tools.map((t) => t.name));
  for (const expected of [
    "lumina_system_info",
    "lumina_library_query",
    "lumina_import_start",
    "lumina_edit_photo",
    "lumina_develop_render",
    "lumina_export_create",
    "lumina_health_scan",
  ]) {
    assert(names.has(expected), `工具存在: ${expected}`);
  }

  // 4. lumina_system_info → 真实引擎握手
  const info = await rpc(
    "tools/call",
    { name: "lumina_system_info", arguments: {} },
    90000,
  );
  assert(info.isError === false, "lumina_system_info 无错误");
  const infoData = JSON.parse(info.content[0].text);
  assert(infoData.protocolVersion === "1.0", `引擎协议 ${infoData.protocolVersion}`);
  const norm = (p) => String(p).toLowerCase().replace(/\\/g, "/");
  assert(
    infoData.homeDir && norm(infoData.homeDir).startsWith(norm(home)),
    `引擎 home 隔离: ${infoData.homeDir}`,
  );
  console.log(`  引擎: v${infoData.engineVersion}, ${infoData.homeDir}`);

  // 5. lumina_library_stats → 空库统计
  const stats = await rpc("tools/call", { name: "lumina_library_stats", arguments: {} }, 60000);
  assert(stats.isError === false, "lumina_library_stats 无错误");
  console.log(`  空库统计: ${info.content[0].text.slice(0, 0)}${stats.content[0].text.slice(0, 120)}`);

  // 6. 未知工具 → isError
  const bad = await rpc("tools/call", { name: "no_such_tool", arguments: {} });
  assert(bad.isError === true, "未知工具返回 isError=true");

  // 7. 未知方法 → -32601
  const unknown = await new Promise((resolve, reject) => {
    const id = ++nextId;
    pending.set(id, resolve);
    child.stdin.write(JSON.stringify({ jsonrpc: "2.0", id, method: "resources/list" }) + "\n");
    setTimeout(() => reject(new Error("timeout")), 5000);
  });
  assert(unknown.error?.code === -32601, "未知 MCP 方法返回 -32601");
} catch (err) {
  failures = 1;
  console.error(`  FAIL: ${err.message}`);
} finally {
  child.stdin.end();
  await new Promise((resolve) => {
    const t = setTimeout(() => {
      child.kill();
      resolve();
    }, 15000);
    child.on("exit", () => {
      clearTimeout(t);
      resolve();
    });
  });
  try {
    rmSync(home, { recursive: true, force: true });
  } catch {
    /* Windows 句柄释放慢，忽略 */
  }
}
console.log(failures ? "SMOKE FAILED" : "SMOKE PASSED");
process.exit(failures);
