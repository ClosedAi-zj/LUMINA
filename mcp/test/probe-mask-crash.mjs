// A/B 探针:定位蒙版渲染崩溃触发条件(带/不带 local 调整的 radial 蒙版)
import { spawn } from 'node:child_process';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const here = path.dirname(fileURLToPath(import.meta.url));
const child = spawn(process.execPath, [path.resolve(here, '..', 'server.mjs')], { stdio: ['pipe', 'pipe', 'inherit'] });

let buf = '';
const waiters = [];
child.stdout.setEncoding('utf8');
child.stdout.on('data', (c) => {
  buf += c;
  let i;
  while ((i = buf.indexOf('\n')) >= 0) {
    const line = buf.slice(0, i).trim();
    buf = buf.slice(i + 1);
    if (line && waiters.length) waiters.shift()(JSON.parse(line));
  }
});
let nextId = 1;
const rpc = (method, params) => new Promise((resolve) => {
  child.stdin.write(`${JSON.stringify({ jsonrpc: '2.0', id: nextId++, method, ...(params ? { params } : {}) })}\n`);
  setTimeout(() => waiters.shift()(null), 120_000), waiters.push(resolve);
});
const call = async (name, args) => {
  const r = await rpc('tools/call', { name, arguments: args ?? {} });
  const t = r?.result?.content?.[0]?.text ?? JSON.stringify(r);
  console.log(`## ${name} ${JSON.stringify(args ?? {})} -> ${r?.result?.isError ? 'ERROR: ' : 'OK'} ${t.slice(0, 300)}`);
  try { return JSON.parse(t); } catch { return null; }
};

async function main() {
  await rpc('initialize', { protocolVersion: '2025-06-18', capabilities: {}, clientInfo: { name: 'probe', version: '0' } });
  child.stdin.write(`${JSON.stringify({ jsonrpc: '2.0', method: 'notifications/initialized' })}\n`);

  const photos = await call('list_photos', { offset: 0, limit: 10 });
  // 找一张没有蒙版的照片
  const target = photos.items.find((p) => !p.hasEdits) ?? photos.items[1];
  console.log('# probe photo', target.id, target.fileName, 'hasEdits=', target.hasEdits);
  await call('open_photo', { photoId: target.id });
  await call('lumina_screenshot', { path: 'D:/LUMINA/build/probe-0-open.png' });
  // A:radial 无 local
  await call('add_mask', { type: 'radial', amount: -20 });
  await call('lumina_screenshot', { path: 'D:/LUMINA/build/probe-1-radial-nolocal.png' });
  // B:radial 带 local.exposure
  await call('add_mask', { type: 'radial', amount: -20, local: { exposure: -0.3 } });
  await call('lumina_screenshot', { path: 'D:/LUMINA/build/probe-2-radial-local.png' });
  console.log('# probe done');
  child.kill();
}
main().catch((e) => { console.error('fatal', e); child.kill(); process.exit(1); });
