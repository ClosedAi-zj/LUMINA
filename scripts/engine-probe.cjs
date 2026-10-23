/**
 * 引擎帧协议探针（验证用，不入构建产物）：
 * spawn 引擎 → 4 字节 LE 长度前缀帧收发 → 逐条调用指定方法并打印响应。
 *
 * v2 修正（对照 scripts/probe_shutdown_v2.py）：
 * - 每次运行使用独立 --home（默认 runtime-probe，先清空重建），避免共享 runtime 的单实例锁互踩；
 * - stderr 持续排空（引擎逐行写 stderr，不排空会因管道写满死锁引擎——测试伪象）；
 * - 内置回归断言：map.getLocations zoom 6/8/12 三档 locationId 唯一性（同名 cluster 去重）；
 * - shutdown 后打印进程退出码（验证干净退出 exit 0）。
 *
 * 用法：node scripts/engine-probe.cjs [method paramsJSON 方法 参数 ...]
 */
const { spawn } = require('node:child_process');
const { rmSync, mkdirSync, existsSync } = require('node:fs');
const path = require('node:path');

const ENGINE = process.env.LUMINA_ENGINE || path.resolve(__dirname, '..', 'build', 'engine', 'lumina-engine.exe');
const HOME = process.env.LUMINA_HOME || path.resolve(__dirname, '..', 'build', 'runtime-probe');

// 独立 home：每次先清空重建（引擎首启自动生成 mock 图库，隔离且幂等）
if (!process.env.LUMINA_HOME) {
  if (existsSync(HOME)) rmSync(HOME, { recursive: true, force: true });
  mkdirSync(HOME, { recursive: true });
}

const CALLS = [
  ['system.initialize', { uiVersion: 'probe-2.0' }],
  ['system.ping', {}],
  ['library.getStats', {}],
  ['map.getLocations', { zoom: 6 }],
  ['map.getLocations', { zoom: 8 }],
  ['map.getLocations', { zoom: 12 }],
  ['map.listUnlocated', { offset: 0, limit: 2 }],
  ['hdr.detectBracketGroups', {}],
  ['timelapse.listSequences', {}],
  ['raw.selfTest', { sample: 8 }],
];

const proc = spawn(ENGINE, ['--home', HOME], { stdio: ['pipe', 'pipe', 'pipe'], windowsHide: true });
let buf = Buffer.alloc(0);
const pending = new Map();
let nextId = 1;

proc.stdout.on('data', (chunk) => {
  buf = Buffer.concat([buf, chunk]);
  for (;;) {
    if (buf.length < 4) return;
    const len = buf.readUInt32LE(0);
    if (buf.length < 4 + len) return;
    const payload = buf.subarray(4, 4 + len).toString('utf8');
    buf = buf.subarray(4 + len);
    try {
      const msg = JSON.parse(payload);
      if (typeof msg.id === 'number') {
        const p = pending.get(msg.id);
        if (p) {
          pending.delete(msg.id);
          p(msg);
        }
      } else if (msg.method) {
        // 通知仅计数不打印（引擎 shutdown 后抑制外发，此处一般收不到）
      }
    } catch {
      console.log(`[parse-error] ${payload.slice(0, 100)}`);
    }
  }
});
// stderr：无条件持续排空（防管道背压死锁引擎），仅过滤打印关键行
proc.stderr.on('data', (c) => {
  const s = c.toString('utf8');
  if (/ERROR|FATAL|WARN|not found|ready|starting|leaving/.test(s)) process.stderr.write(`[engine] ${s}`);
});

let exitCode = null;
proc.on('exit', (code) => {
  exitCode = code;
});

function frame(obj) {
  const payload = Buffer.from(JSON.stringify(obj), 'utf8');
  const head = Buffer.alloc(4);
  head.writeUInt32LE(payload.length, 0);
  proc.stdin.write(Buffer.concat([head, payload]));
}

function call(method, params, timeoutMs = 20000) {
  return new Promise((resolve) => {
    const id = nextId++;
    const timer = setTimeout(() => {
      pending.delete(id);
      resolve({ timeout: true, method });
    }, timeoutMs);
    pending.set(id, (msg) => {
      clearTimeout(timer);
      resolve(msg);
    });
    frame({ jsonrpc: '2.0', id, method, params });
  });
}

/** map.getLocations 去重断言：跨网格同名 cluster 必须合并为单条（locationId 唯一、count 取并集） */
async function dedupeCheck() {
  // 复刻 verify_map_dedupe.py 的播种：网格 A(30.01,100.01) 两张 + 网格 B(30.15,100.15) 两张，同名"雾浓顶"
  const setA = await call('map.setPhotoLocation', { ids: [1, 2], lat: 30.01, lng: 100.01, locationName: '雾浓顶' });
  const setB = await call('map.setPhotoLocation', { ids: [3, 4], lat: 30.15, lng: 100.15, locationName: '雾浓顶' });
  const setOk = !setA.error && !setB.error;
  console.log(`[dedupe] 播种 setPhotoLocation A/B → ${setOk ? 'ok' : `ERROR ${setA.error?.code ?? setB.error?.code}`}（updated=${setA.result?.updated}/${setB.result?.updated}）`);
  let allPass = setOk;
  for (const zoom of [6, 8, 12]) {
    const res = await call('map.getLocations', { zoom });
    if (res.error || !Array.isArray(res.result)) {
      console.log(`[dedupe] zoom=${zoom} → ${res.error ? `ERROR ${res.error.code}` : '非数组返回'} ❌`);
      allPass = false;
      continue;
    }
    const ids = res.result.map((l) => l.locationId).filter((v) => v !== undefined && v !== null);
    const dup = ids.length - new Set(ids).size;
    const wucd = res.result.filter((l) => l.name === '雾浓顶');
    const oneWucd = wucd.length === 1 && wucd[0].count === 4;
    console.log(`[dedupe] zoom=${zoom} → ${res.result.length} 条, locationId 唯一=${dup === 0 ? 'YES' : `NO(重复${dup})`}, 雾浓顶条数=${wucd.length} count=${wucd[0]?.count ?? '-'} ${dup === 0 && oneWucd ? '✓' : '❌'}`);
    if (dup !== 0 || !oneWucd) allPass = false;
  }
  console.log(`[dedupe] MAP DEDUPE: ${allPass ? 'PASS' : 'FAIL'}`);
  return allPass;
}

(async () => {
  const t0 = Date.now();
  // 小规格 mock 播种（独立 home，秒级完成；30 张含 RW2 供 selfTest 抽样）
  await call('mock.generate', { spec: { total: 30, rootCount: 1, folderCount: 2, gps: 0, edited: 0, missing: 0, libraryRoot: HOME } }, 10000);
  for (let i = 0; i < 60; i++) {
    const s = await call('mock.status', {}, 8000);
    if (s.result?.generated) break;
    await new Promise((r) => setTimeout(r, 300));
  }
  for (const [method, params] of CALLS) {
    const res = await call(method, params, method === 'system.initialize' ? 60000 : 20000);
    const summary = res.timeout
      ? 'TIMEOUT'
      : res.error
        ? `ERROR ${res.error.code} ${res.error.message}`
        : JSON.stringify(res.result).slice(0, 260);
    console.log(`>> ${method} ${method === 'map.getLocations' ? `zoom=${params.zoom}` : ''} → ${summary}`);
  }
  const dedupePass = await dedupeCheck();

  // 干净退出验证：shutdown → 等进程自然退出（stderr 已持续排空，无背压伪象）
  const shutdownT0 = Date.now();
  await call('system.shutdown', {}, 8000).catch(() => undefined);
  const exited = await Promise.race([
    new Promise((r) => {
      const check = setInterval(() => {
        if (exitCode !== null) {
          clearInterval(check);
          r(true);
        }
      }, 50);
    }),
    new Promise((r) => setTimeout(() => r(false), 10000)),
  ]);
  if (exited) {
    console.log(`[shutdown] 引擎回复后 ${Date.now() - shutdownT0}ms 内退出，exit=${exitCode} ${exitCode === 0 ? '✓ 干净退出' : '❌ 非零退出码'}`);
  } else {
    console.log('[shutdown] ❌ 10s 内未退出（可能仍持锁）');
    const { execSync } = require('node:child_process');
    try {
      execSync(`taskkill /PID ${proc.pid} /T /F`, { stdio: 'ignore' });
    } catch {
      /* ignore */
    }
  }
  console.log(`[probe] 总耗时 ${(Date.now() - t0) / 1000}s`);
  setTimeout(() => process.exit(0), 300);
})();
