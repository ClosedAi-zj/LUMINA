#!/usr/bin/env node
/**
 * 直连桥接诊断（不经 MCP 客户端，便于在 MCP 进程未重启时验证新 op）
 *
 * 用法：node mcp/test/bridge-call.mjs <op> '<json-params>'
 *   例：node mcp/test/bridge-call.mjs set_develop_mode '{"mode":"pro"}'
 *
 * 协议：127.0.0.1:<mcp-port>，换行分隔 JSON，请求 {id, op, params}，响应 {id, ok, result|error}
 */
import net from 'node:net';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';

const home = process.env.LUMINA_HOME || path.join(os.homedir(), '.lumina');
const portFile = path.join(home, 'mcp-port');

if (!fs.existsSync(portFile)) {
  console.error(`找不到 ${portFile}（应用未启动？）`);
  process.exit(1);
}
const port = Number(fs.readFileSync(portFile, 'utf8').trim());
if (!Number.isFinite(port)) {
  console.error(`端口文件内容非法: ${portFile}`);
  process.exit(1);
}

const op = process.argv[2];
if (!op) {
  console.error('用法: node mcp/test/bridge-call.mjs <op> \'<json-params>\'');
  process.exit(1);
}
let params = {};
if (process.argv[3]) {
  try {
    params = JSON.parse(process.argv[3]);
  } catch (e) {
    console.error(`params 不是合法 JSON: ${e.message}`);
    process.exit(1);
  }
}

const req = { id: 1, op, params };
const timeoutMs = Number(process.env.BRIDGE_TIMEOUT_MS || 60000);

const sock = net.createConnection({ host: '127.0.0.1', port }, () => {
  sock.write(`${JSON.stringify(req)}\n`);
});

let buf = '';
sock.setEncoding('utf8');
sock.on('data', (chunk) => {
  buf += chunk;
  const idx = buf.indexOf('\n');
  if (idx < 0) return;
  const line = buf.slice(0, idx);
  let res;
  try {
    res = JSON.parse(line);
  } catch {
    console.error('响应不是合法 JSON:', line.slice(0, 400));
    sock.destroy();
    process.exit(1);
  }
  if (res.ok) {
    console.log(JSON.stringify(res.result, null, 2));
    sock.destroy();
    process.exit(0);
  }
  console.error(`FAILED: ${res.error}`);
  sock.destroy();
  process.exit(2);
});

sock.on('error', (err) => {
  console.error(`连接 127.0.0.1:${port} 失败: ${err.message}`);
  process.exit(1);
});

const timer = setTimeout(() => {
  console.error(`超时 ${timeoutMs}ms（op=${op}）`);
  sock.destroy();
  process.exit(3);
}, timeoutMs);
timer.unref?.();
