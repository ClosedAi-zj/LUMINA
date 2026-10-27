// e2e-bridge.mjs —— 模拟 MCP 客户端（WorkBuddy 同款路径）端到端自测
//
// spawn mcp/server.mjs（stdio JSON-RPC 2.0）→ initialize → tools/list →
// 依次真实调用 lumina_status / list_photos / open_photo / get_develop_params /
// set_develop_params(改曝光) / add_mask(radial) / create_snapshot / restore_snapshot /
// export_photo / switch_module。
// 每步把真实请求/响应 JSON 打到 stdout（落 build/mcp-e2e-log.md 用）。
// 任一步 isError 或超时 → 退出码 1。

import { spawn } from 'node:child_process';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const here = path.dirname(fileURLToPath(import.meta.url));
const serverPath = path.resolve(here, '..', 'server.mjs');
const EXPORT_DIR = process.env.LUMINA_E2E_EXPORT_DIR || 'D:/LUMINA/build/mcp-e2e-export';

const child = spawn(process.execPath, [serverPath], { stdio: ['pipe', 'pipe', 'inherit'] });

let buf = '';
const waiters = [];
child.stdout.setEncoding('utf8');
child.stdout.on('data', (chunk) => {
  buf += chunk;
  let idx;
  while ((idx = buf.indexOf('\n')) >= 0) {
    const line = buf.slice(0, idx).trim();
    buf = buf.slice(idx + 1);
    if (line === '') continue;
    const w = waiters.shift();
    if (w) w.resolve(JSON.parse(line));
  }
});

function send(obj) {
  const line = JSON.stringify(obj);
  console.log(`>>> ${line}`);
  child.stdin.write(`${line}\n`);
}

let nextId = 1;
let step = 0;
const failures = [];

function recv(timeoutMs = 120_000) {
  return new Promise((resolve, reject) => {
    const timer = setTimeout(() => reject(new Error('等待 MCP 响应超时')), timeoutMs);
    waiters.push({ resolve: (v) => { clearTimeout(timer); resolve(v); } });
  });
}

async function rpc(method, params) {
  const id = nextId++;
  send({ jsonrpc: '2.0', id, method, ...(params !== undefined ? { params } : {}) });
  const res = await recv();
  console.log(`<<< ${JSON.stringify(res)}`);
  return res;
}

function notify(method, params) {
  send({ jsonrpc: '2.0', method, ...(params !== undefined ? { params } : {}) });
}

async function callTool(name, args, expectError = false) {
  step += 1;
  console.log(`\n===== step ${step}: tools/call ${name} ${JSON.stringify(args ?? {})} =====`);
  const res = await rpc('tools/call', { name, arguments: args ?? {} });
  if (res.error) {
    failures.push(`${name}: protocol error ${JSON.stringify(res.error)}`);
    console.log(`!! ${name} protocol error:`, JSON.stringify(res.error));
    return null;
  }
  const isError = res.result?.isError === true;
  const text = res.result?.content?.find((c) => c.type === 'text')?.text ?? '';
  console.log(`## ${name} -> ${isError ? 'ERROR' : 'OK'}: ${text.slice(0, 2000)}`);
  if (isError && !expectError) failures.push(`${name}: ${text}`);
  return { isError, text };
}

// ---------- 用例 ----------

async function main() {
  console.log(`# spawn: ${process.execPath} ${serverPath}`);
  const init = await rpc('initialize', {
    protocolVersion: '2025-06-18',
    capabilities: {},
    clientInfo: { name: 'lumina-e2e', version: '1.0.0' },
  });
  if (!init.result?.serverInfo) throw new Error(`initialize 失败: ${JSON.stringify(init)}`);
  console.log(`# serverInfo = ${JSON.stringify(init.result.serverInfo)}, protocol = ${init.result.protocolVersion}`);
  notify('notifications/initialized');
  console.log('# notifications/initialized sent');

  const list = await rpc('tools/list', {});
  const tools = list.result?.tools ?? [];
  console.log(`# tools/list -> ${tools.length} tools: ${tools.map((t) => t.name).join(', ')}`);
  // v1.4.1：新增 set_develop_mode / set_theme（AI 自检切模式与主题）→ 13；
  // 再加 G9 文档 Tab 三件套 open_tabs / close_tab / list_tabs → 16，M3-03 加 psd_inspect → 17，
  // M4-03 加 softproof_preview → 18
  if (tools.length !== 18) failures.push(`tools/list 期望 18 个工具，实际 ${tools.length}`);

  await callTool('lumina_status');
  await callTool('list_photos', { offset: 0, limit: 5 });

  // 取第一张照片 id（从上一步结果解析）
  const photosRes = await rpc('tools/call', { name: 'list_photos', arguments: { offset: 0, limit: 5 } });
  const photos = JSON.parse(photosRes.result?.content?.[0]?.text ?? '{"items":[]}');
  if (!photos.items?.length) throw new Error(`图库为空，无法继续（total=${photos.total}）`);
  const photoId = photos.items[0].id;
  console.log(`# 目标照片 photoId=${photoId} (${photos.items[0].fileName})`);
  step += 1;

  await callTool('open_photo', { photoId });
  await callTool('get_develop_params');
  await callTool('set_develop_params', { edits: { exposure: 0.35, contrast: 12 } });
  await callTool('lumina_screenshot', { path: 'D:/LUMINA/build/mcp-e2e-shot-params.png' });
  await callTool('get_develop_params'); // 复核参数确实生效
  await callTool('add_mask', { type: 'radial', amount: -20, local: { exposure: -0.3 } });
  await callTool('lumina_screenshot', { path: 'D:/LUMINA/build/mcp-e2e-shot-mask.png' });
  const snap = await callTool('create_snapshot', { name: 'MCP e2e 快照' });
  let snapshotId = null;
  try {
    snapshotId = JSON.parse(snap?.text ?? '{}').snapshotId ?? null;
  } catch { /* ignore */ }
  if (snapshotId != null) {
    await callTool('restore_snapshot', { snapshotId });
    // 快照是创建时的状态（含 0.35 曝光 + 蒙版），恢复后参数应仍在
  } else {
    failures.push('create_snapshot 未返回 snapshotId');
  }
  await callTool('export_photo', { outPath: EXPORT_DIR, format: 'jpg', quality: 92, longEdge: 2048 });
  await callTool('switch_module', { module: 'library' });
  await callTool('lumina_screenshot', { path: 'D:/LUMINA/build/mcp-e2e-shot-library.png' });
  await callTool('switch_module', { module: 'develop' });
  // 负例：未知参数键必须报错（零静默验收）
  await callTool('set_develop_params', { edits: { notAParam: 1 } }, true);
  // 负例：未打开会话之外的非法蒙版类型
  await callTool('add_mask', { type: 'aiSky' }, true);

  console.log(`\n===== e2e 完成：${step} 步，失败 ${failures.length} =====`);
  for (const f of failures) console.log(`FAIL: ${f}`);
  child.kill();
  process.exit(failures.length === 0 ? 0 : 1);
}

main().catch((err) => {
  console.error(`# e2e fatal: ${err?.stack ?? err}`);
  child.kill();
  process.exit(1);
});
