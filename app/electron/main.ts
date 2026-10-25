/**
 * Electron 主进程：窗口管理、引擎桥接（spawn / mock 兜底 / 崩溃看护）、
 * lumina-file:// 协议、画廊本地预览 HTTP、--home 透传。
 */

import { app, BrowserWindow, ipcMain, Menu, shell, dialog } from 'electron';
import path from 'node:path';
import fs from 'node:fs';
import { EngineBridge, RpcCallError, resolveEnginePath, resolveHomeDir, type BridgeLifecycle, type EngineNotification } from './engine-bridge';
import { registerFileProtocol, registerFileScheme } from './file-protocol';

const isDev = process.env.LUMINA_DEV === '1';
const isSmoke = process.env.LUMINA_SMOKE === '1';
/** WP-C1 视觉基线：逐帧导航 + 双主题截图（13 帧设计稿 → docs/ui-1.1/baseline/） */
const isBaseline = process.env.LUMINA_BASELINE === '1';
/** v1.2.0 外壳自检：四工作区切换 / 二级导航 / 精修占位 / 主题切换（?shellcheck=1） */
const isShellCheck = process.env.LUMINA_SHELLCHECK === '1';
// 软件渲染兜底：远程桌面、虚拟机、部分显卡驱动下 GPU 进程会反复崩溃
// （exit_code=-1073741819），表现为窗口白屏或起不来。置为 1 强制软件渲染。
// 覆盖场景：冒烟测试、无 GPU 服务器、用户手动排障。
const safeGL = process.env.LUMINA_SAFE_GL === '1';
// appRoot = 工程根。开发时 app.getAppPath() 是 {repo}/app，打包后是 {resources}/app，
// 两者父级各自就是 {repo} 与 {resources}。
// 不要改回 path.resolve(__dirname,'..','..','..')：开发布局下 __dirname 在 build/app/electron
// 恰好上溯三级，但打包布局（resources/app/electron）会多跳一层到安装根，导致找不到
// build/engine 与 build/app/index.html —— 表现为窗口不出现、引擎不拉起、stdout 无任何输出。
const appRoot = path.resolve(app.getAppPath(), '..');
const homeDir = resolveHomeDir();
const enginePath = resolveEnginePath(appRoot);

// 启动日志：Windows 下 Electron GUI 进程的 stdout 不会写入重定向文件，
// 出问题时只能靠落盘日志定位（内测排障同样依赖它）。
const bootLogPath = path.join(homeDir, 'logs', 'electron-boot.log');
function boot(msg: string): void {
  try {
    require('node:fs').mkdirSync(path.dirname(bootLogPath), { recursive: true });
    require('node:fs').appendFileSync(
      bootLogPath,
      `${new Date().toISOString()} [${process.pid}] ${msg}\n`,
    );
  } catch {
    /* 日志写失败不能影响启动 */
  }
}
boot(`boot: appPath=${app.getAppPath()} appRoot=${appRoot} engine=${enginePath} home=${homeDir} dev=${isDev} smoke=${isSmoke}`);

// 冒烟 / 无 GPU 环境：关闭硬件加速，WebGL 走 SwiftShader 软件渲染
// 打包版默认安全 GL：用户机器 GPU 驱动不可控，GPU 进程崩溃是 FATAL（实测 0xC0000005
// 连环崩溃后整个应用退出）。需要硬件加速时显式设 LUMINA_HARDWARE_GL=1；开发模式不受影响。
if (isSmoke || safeGL || (!isDev && process.env.LUMINA_HARDWARE_GL !== '1')) {
  app.disableHardwareAcceleration();
  app.commandLine.appendSwitch('disable-gpu');
  app.commandLine.appendSwitch('disable-gpu-compositing');
  app.commandLine.appendSwitch('in-process-gpu');
  app.commandLine.appendSwitch('use-angle', 'swiftshader');
  app.commandLine.appendSwitch('use-gl', 'angle');
  // Electron 新版默认禁用「自动回退软件 WebGL」，不显式开启时修图台拿不到 WebGL 上下文，
  // 日志报 "Automatic fallback to software WebGL has been deprecated"。
  app.commandLine.appendSwitch('enable-unsafe-swiftshader');
  // 软件渲染下 GPU 进程本就多余，崩溃日志会刷满控制台
  app.commandLine.appendSwitch('disable-gpu-process-crash-limit');
}

// WP-B6 验证开关：LUMINA_NO_WEBGL=1 强制禁用 WebGL（修图台走 CPU 软渲染路径）
if (process.env.LUMINA_NO_WEBGL === '1') {
  app.disableHardwareAcceleration();
  app.commandLine.appendSwitch('disable-webgl');
  app.commandLine.appendSwitch('disable-webgl2');
}

// WP-C1 视觉基线：截图必须拿到真实合成帧。窗口被判定为"遮挡/后台"时 Chromium 会停止
// 合成，capturePage 就返回上一帧（实测：连续 7 张 md5 完全相同）——必须关掉节流并强制可见。
if (isBaseline) {
  // 基线口径（team-lead 定）：1440×900 @2x，产物 → docs/ui-1.1/baseline-v12/
  app.commandLine.appendSwitch('force-device-scale-factor', '2');
  app.commandLine.appendSwitch('disable-background-timer-throttling');
  app.commandLine.appendSwitch('disable-backgrounding-occluded-windows');
  app.commandLine.appendSwitch('disable-renderer-backgrounding');
}

let mainWindow: BrowserWindow | null = null;
let bridge: EngineBridge | null = null;
let galleryServer: { close(): void } | null = null;

type InputModifier = 'shift' | 'control' | 'alt' | 'meta';

/** 单个待注入的按键：phase=down/up 用于长按类快捷键（如 Space 临时看原图） */
interface InjectedKey {
  key: string;
  mods?: InputModifier[];
  phase?: 'tap' | 'down' | 'up';
}

/**
 * 冒烟握手表：渲染层打印 `[smoke-scenario] <tag>`，主进程就往窗口投对应按键。
 * 用途是验证快捷键的真实 keydown 链路（WP-B7 DoD：每个键在 UI 生效）。
 * keyCode 用物理键符：`'` 为单引号键，`?` 走 Shift+/ 的物理路径。
 */
const KEY_INJECTION_STEPS: Array<[string, InjectedKey[]]> = [
  ['keys-copy', [{ key: 'c', mods: ['control'] }]],
  ['keys-paste', [{ key: 'v', mods: ['control'] }]],
  ['keys-undo', [{ key: 'z', mods: ['control'] }]],
  ['keys-redo', [{ key: 'z', mods: ['control', 'shift'] }]],
  ['keys-next', [{ key: "'", mods: ['control'] }]],
  ['keys-prev', [{ key: "'", mods: ['control', 'shift'] }]],
  ['keys-overlay-open', [{ key: '/', mods: ['shift'] }]],
  ['keys-overlay-close', [{ key: '/', mods: ['shift'] }]],
  ['keys-compare', [{ key: '\\' }]],
  ['keys-space-down', [{ key: 'Space', phase: 'down' }]],
  ['keys-space-up', [{ key: 'Space', phase: 'up' }]],
];

function createWindow(): void {
  mainWindow = new BrowserWindow({
    width: isBaseline ? 1440 : 1680,
    height: isBaseline ? 900 : 1000,
    // 基线口径：1440×900 指**内容区**，与设计稿 2880×1800@2x 逐像素可比
    useContentSize: isBaseline,
    minWidth: 1280,
    minHeight: 800,
    backgroundColor: '#0C0D10',
    title: 'LUMINA · 摄影工作台',
    webPreferences: {
      preload: path.join(__dirname, 'preload.cjs'),
      contextIsolation: true,
      nodeIntegration: false,
      sandbox: false,
      spellcheck: false,
      // WP-C1：后台节流会让 capturePage 返回陈旧帧
      backgroundThrottling: false,
    },
    show: true,
  });

  // 渲染进程 console 转发到主进程 stdout（smoke 验证用）
  mainWindow.webContents.on('console-message', (_e, level, message, line, sourceId) => {
    if (isSmoke || isBaseline || isShellCheck || level >= 2) {
      const tag = level === 3 ? 'error' : level === 2 ? 'warn' : level === 1 ? 'log' : 'debug';
      process.stdout.write(`[renderer][${tag}] ${message} (${sourceId}:${line})\n`);
    }
    // capturePage 偶发返回空帧/全黑帧（窗口尚未合成完成，软渲染下更常见）。
    // 防御三层：① 窗口 show+focus；② 逐次加大等待；③ 采样亮度判黑，黑帧重试后再失败就明确标注
    // （黑帧绝不当"通过"——报告里必须能看出来）。
    const meanLuma = (img: Electron.NativeImage): number => {
      try {
        const buf = img.toBitmap();
        if (buf.length === 0) return 0;
        let sum = 0;
        let n = 0;
        for (let i = 0; i < buf.length; i += 4 * 37) {
          sum += 0.299 * buf[i] + 0.587 * buf[i + 1] + 0.114 * buf[i + 2];
          n++;
        }
        return n > 0 ? sum / n : 0;
      } catch {
        return 0;
      }
    };
    // 与上一次截图逐字节比对：窗口未重新合成时 capturePage 会返回上一张（陈旧帧），
    // 只查"是否空/是否黑"抓不到这种情况（陈旧帧亮度完全正常）。
    const lastShot = new Map<string, Buffer>();
    const captureTo = (out: string, retries = 1, delayMs = 400): void => {
      setTimeout(() => {
        const win = mainWindow;
        if (!win) return;
        if (!win.isVisible()) win.show();
        win.focus();
        void win.webContents
          .capturePage()
          .then((img) => {
            const png = img.toPNG();
            const luma = meanLuma(img);
            const prev = lastShot.get(out);
            const stale = prev !== undefined && png.equals(prev);
            const black = img.isEmpty() || luma < 6;
            const bad = black || stale;
            if (bad && retries > 0) {
              process.stdout.write(
                `[baseline] ${img.isEmpty() ? 'empty' : stale ? 'stale' : 'black'} frame (luma=${luma.toFixed(2)}), retry -> ${out}\n`,
              );
              captureTo(out, retries - 1, Math.round(delayMs * 1.7));
              return;
            }
            lastShot.set(out, png);
            require('node:fs').mkdirSync(path.dirname(out), { recursive: true });
            require('node:fs').writeFileSync(out, png);
            const sz = img.getSize();
            process.stdout.write(
              `[baseline] shot -> ${out} (${sz.width}x${sz.height}) luma=${luma.toFixed(1)}${bad ? ' UNRESOLVED' : ''}\n`,
            );
          })
          .catch((err) => process.stdout.write(`[baseline] capture failed: ${String(err)}\n`));
      }, delayMs);
    };
    // smoke：修图页跑通后自动截图存档（验证渲染结果肉眼可查，黑屏类问题不再靠猜）
    if (isSmoke && message.includes('[smoke-scenario] develop-xmp')) {
      captureTo(path.resolve(appRoot, 'build/smoke-develop.png'), 2);
    }
    // T1.4：93 叶 parity 报表落盘（渲染层单行 JSON → build/parity-report.json）
    if (isSmoke && message.includes('[smoke-scenario] parity93-report: ')) {
      try {
        const json = message.slice(message.indexOf('parity93-report: ') + 'parity93-report: '.length).trim();
        JSON.parse(json); // 完整性校验
        const out = path.resolve(appRoot, 'build/parity-report.json');
        fs.mkdirSync(path.dirname(out), { recursive: true });
        fs.writeFileSync(out, json, 'utf8');
        process.stdout.write(`[smoke] parity-report -> ${out}\n`);
      } catch (err) {
        process.stdout.write(`[smoke] parity-report write failed: ${String(err)}\n`);
      }
    }
    // T1.5：导出=预览像素断言报表落盘（渲染层单行 JSON → build/export-parity-report.json）
    if (isSmoke && message.includes('[smoke-scenario] export-parity-report: ')) {
      try {
        const json = message.slice(message.indexOf('export-parity-report: ') + 'export-parity-report: '.length).trim();
        JSON.parse(json); // 完整性校验
        const out = path.resolve(appRoot, 'build/export-parity-report.json');
        fs.mkdirSync(path.dirname(out), { recursive: true });
        fs.writeFileSync(out, json, 'utf8');
        process.stdout.write(`[smoke] export-parity-report -> ${out}\n`);
      } catch (err) {
        process.stdout.write(`[smoke] export-parity-report write failed: ${String(err)}\n`);
      }
    }
    // T1.5 ⑦c / T1.7 t17 夹具：渲染层 PNG dataURL + 行内目标路径 → 主进程写盘
    // （⑦c 共享无损底图消除浏览器/引擎双解码差；t17 代表性夹具落盘后走真实 import 入库）
    if (isSmoke) {
      const pngWrite = /\[smoke-scenario\] (?:export-parity-base|t17-fixture-write) (.+?): data:image\/png;base64,([A-Za-z0-9+/=]+)/.exec(message);
      if (pngWrite) {
        try {
          const out = pngWrite[1].trim();
          fs.mkdirSync(path.dirname(out), { recursive: true });
          fs.writeFileSync(out, Buffer.from(pngWrite[2], 'base64'));
          process.stdout.write(`[smoke] png-write -> ${out}\n`);
        } catch (err) {
          process.stdout.write(`[smoke] png-write failed: ${String(err)}\n`);
        }
      }
    }
    // WP-B1：主题双截图。深色在 smoke 归位深色后拍（theme-pre 行，仍在修图台）；
    // 浅色在 theme 日志行后拍（smoke 已留 600ms 截图窗口）。
    if (isSmoke && message.includes('[smoke-scenario] theme-pre')) {
      captureTo(path.resolve(appRoot, 'docs/ui-1.1/theme-dark.png'), 2);
    }
    if (isSmoke && message.includes('[smoke-scenario] hsl-diag')) {
      captureTo(path.resolve(appRoot, 'build/hsl-diag.png'), 2);
    }
    if (isSmoke && message.includes('[smoke-scenario] brush-shot')) {
      captureTo(path.resolve(appRoot, 'build/brush-sdf.png'), 2);
    }
    // M-3：浮层（对话框）打开状态截图，验证玻璃材质 + 浮入动效
    if (isSmoke && message.includes('[smoke-scenario] overlay-shot')) {
      captureTo(path.resolve(appRoot, 'build/review-overlay.png'), 3, 600);
    }
    // T1.7：修图专业模式截图（确认占位提示条消失）
    if (isSmoke && message.includes('[smoke-scenario] pro-shot')) {
      captureTo(path.resolve(appRoot, 'build/review-t17-develop-pro.png'), 3, 600);
    }
    // WP-C1 视觉基线：`[baseline] shot <id>-<name> dark|light`
    if (isBaseline) {
      const m = /\[baseline\] shot ([a-z0-9]+-[a-z0-9-]+) (dark|light)/i.exec(message);
      if (m) {
        captureTo(path.resolve(appRoot, 'docs/ui-1.1/baseline-v12', `${m[2]}-${m[1]}.png`), 4, 700);
      }
    }
    // v1.2.0 外壳自检截图（深浅各一张，人工核对"不漏色"）
    if (isShellCheck) {
      const m = /\[shellcheck\] shot (dark|light)/i.exec(message);
      if (m) {
        captureTo(path.resolve(appRoot, 'build', `shell-${m[1]}.png`), 3, 600);
      }
    }
    if (isSmoke && message.includes('[smoke-scenario] theme: dark=')) {
      captureTo(path.resolve(appRoot, 'docs/ui-1.1/theme-light.png'), 1);
    }
    // WP-B7：快捷键端到端验证。sendInputEvent 把事件真正投进渲染层，走的是
    // 原生 keydown 链路（不是回读代码/直接调 store），所以能证明"键在 UI 生效"。
    // 每个 keys-* 日志行由 smoke.ts 发出，主进程据此投一次按键，形成握手。
    if (isSmoke) {
      for (const [tag, keys] of KEY_INJECTION_STEPS) {
        if (!message.includes(`[smoke-scenario] ${tag}`)) continue;
        const wc = mainWindow?.webContents;
        if (!wc) continue;
        for (const k of keys) {
          const phase = k.phase ?? 'tap';
          const mods = k.mods ?? [];
          if (phase === 'tap' || phase === 'down') {
            wc.sendInputEvent({ type: 'keyDown', keyCode: k.key, modifiers: mods });
            if (phase === 'tap') wc.sendInputEvent({ type: 'char', keyCode: k.key, modifiers: mods });
          }
          if (phase === 'tap' || phase === 'up') {
            wc.sendInputEvent({ type: 'keyUp', keyCode: k.key, modifiers: mods });
          }
        }
        process.stdout.write(`[smoke] key-inject ${tag} -> ${JSON.stringify(keys)}\n`);
      }
    }
  });
  mainWindow.webContents.on('render-process-gone', (_e, details) => {
    process.stdout.write(`[renderer-gone] ${JSON.stringify(details)}\n`);
  });

  if (isDev) {
    void mainWindow.loadURL('http://localhost:5173');
    mainWindow.webContents.openDevTools({ mode: 'detach' });
  } else {
    const indexPath = path.join(appRoot, 'build', 'app', 'index.html');
    boot(`loadFile: ${indexPath} exists=${require('node:fs').existsSync(indexPath)}`);
    // WP-B6 验证：LUMINA_NO_WEBGL=1 时给渲染层传 nowebgl=1，DevelopRenderer 直接拒绝初始化；
    // WP-C1：LUMINA_BASELINE=1 传 baseline=1，跑视觉基线导航（不跑冒烟场景）
    const query: Record<string, string> = {};
    if (process.env.LUMINA_NO_WEBGL === '1') query.nowebgl = '1';
    if (isBaseline) query.baseline = '1';
    if (isShellCheck) query.shellcheck = '1';
    // 诊断（C1 排查）：配合 baseline 使用，直接输出底图原色，用于区分"纹理是黑"与"管线算成黑"
    if (process.env.LUMINA_RAWTEX === '1') query.rawtex = '1';
    void mainWindow.loadFile(indexPath, Object.keys(query).length > 0 ? { query } : undefined);
  }

  mainWindow.webContents.on('did-finish-load', () => boot('did-finish-load'));
  mainWindow.webContents.on('did-fail-load', (_e, code, desc, url) =>
    boot(`did-fail-load code=${code} desc=${desc} url=${url}`),
  );

  mainWindow.on('closed', () => {
    mainWindow = null;
  });
}

function buildMenu(): void {
  const template: Electron.MenuItemConstructorOptions[] = [
    {
      label: 'LUMINA',
      submenu: [
        { label: '关于 LUMINA', role: 'about' },
        { type: 'separator' },
        { label: '退出', role: 'quit' },
      ],
    },
    {
      label: '编辑',
      submenu: [
        { label: '撤销', role: 'undo' },
        { label: '重做', role: 'redo' },
        { type: 'separator' },
        { label: '剪切', role: 'cut' },
        { label: '复制', role: 'copy' },
        { label: '粘贴', role: 'paste' },
        { label: '全选', role: 'selectAll' },
      ],
    },
    {
      label: '视图',
      submenu: [
        { label: '重新加载', role: 'reload' },
        { label: '强制重新加载', role: 'forceReload' },
        { label: '开发者工具', role: 'toggleDevTools' },
        { type: 'separator' },
        { label: '重置缩放', role: 'resetZoom' },
        { label: '放大', role: 'zoomIn' },
        { label: '缩小', role: 'zoomOut' },
        { type: 'separator' },
        { label: '全屏', role: 'togglefullscreen' },
      ],
    },
    {
      label: '窗口',
      submenu: [{ label: '最小化', role: 'minimize' }, { label: '关闭', role: 'close' }],
    },
  ];
  Menu.setApplicationMenu(Menu.buildFromTemplate(template));
}

/** 画廊本地预览端口（引擎不感知该端口，URL 由主进程重写） */
const GALLERY_PORT = 51866;

/**
 * 引擎返回的 previewUrl 是 `lumina-gallery://{token}` —— 这是稳定标识，不是可点击地址。
 * 主进程持有静态服务端口，在这里统一重写成 UI 可直接打开/复制的 http 地址。
 * 只改写该 scheme，其余字段原样透传（包括嵌套在 listPublished 数组里的）。
 */
function rewriteGalleryUrls<T>(value: T): T {
  const prefix = 'lumina-gallery://';
  const toHttp = (u: string) => `http://127.0.0.1:${GALLERY_PORT}/gallery/${u.slice(prefix.length)}/`;
  const walk = (v: unknown): unknown => {
    if (typeof v === 'string') return v.startsWith(prefix) ? toHttp(v) : v;
    if (Array.isArray(v)) return v.map(walk);
    if (v && typeof v === 'object') {
      const out: Record<string, unknown> = {};
      for (const [k, sub] of Object.entries(v as Record<string, unknown>)) out[k] = walk(sub);
      return out;
    }
    return v;
  };
  return walk(value) as T;
}

/** 画廊本地预览：localhost http 静态服务（协议 §5.14 previewUrl） */
async function startGalleryServer(): Promise<void> {
  const http = await import('node:http');
  const fs = await import('node:fs');
  const server = http.createServer((req, res) => {
    const url = new URL(req.url ?? '/', 'http://localhost');
    // /gallery/{token}/... → {home}/gallery/{token}/...
    const rel = url.pathname.replace(/^\/gallery\//, '').replace(/\.\./g, '');
    let filePath = path.join(homeDir, 'gallery', ...rel.split('/').filter(Boolean));
    if (rel === '' || rel.endsWith('/')) filePath = path.join(filePath, 'index.html');
    fs.readFile(filePath, (err, data) => {
      if (err) {
        res.writeHead(404, { 'Content-Type': 'text/plain; charset=utf-8' });
        res.end('404 Not Found');
        return;
      }
      const ext = path.extname(filePath).toLowerCase();
      const mime: Record<string, string> = { '.html': 'text/html', '.css': 'text/css', '.js': 'text/javascript', '.jpg': 'image/jpeg', '.png': 'image/png', '.svg': 'image/svg+xml' };
      res.writeHead(200, { 'Content-Type': mime[ext] ?? 'application/octet-stream' });
      res.end(data);
    });
  });
  await new Promise<void>((resolve) => server.listen(GALLERY_PORT, '127.0.0.1', resolve));
  galleryServer = server;
}

async function bootstrap(): Promise<void> {
  boot('bootstrap: begin');
  registerFileProtocol();
  buildMenu();

  bridge = new EngineBridge({ enginePath, homeDir });
  const forward = (n: EngineNotification) => {
    mainWindow?.webContents.send('lumina:event', n);
  };
  bridge.onEvent(forward);
  bridge.onLifecycle((s: BridgeLifecycle) => {
    // 生命周期同步给 UI（崩溃恢复提示由渲染层 toast 呈现）
    mainWindow?.webContents.send('lumina:lifecycle', s);
  });

  boot('bootstrap: bridge.start() ...');
  await bridge.start();
  boot(`bootstrap: engine mode = ${bridge.isMock ? 'mock' : 'real'}`);
  process.stdout.write(`[lumina] engine mode = ${bridge.isMock ? 'mock' : 'real'} (path=${enginePath})\n`);
  void startGalleryServer();

  ipcMain.handle('lumina:rpc', async (_e, method: string, params: unknown) => {
    if (!bridge) throw new RpcCallError(-32603, '引擎未初始化');
    const result = await bridge.requestWithTimeout(method, params, 10_000);
    return rewriteGalleryUrls(result);
  });
  ipcMain.handle('lumina:info', () => ({
    isMock: bridge?.isMock ?? true,
    enginePath,
    homeDir,
    engineInfo: bridge?.engineInfo ?? {},
    isSmoke,
  }));
  // WP-B3：Mock 兜底后的"重试连接引擎"。start() 可重入：真引擎就位则替换 Mock 传输层。
  ipcMain.handle('lumina:engineRetry', async () => {
    if (!bridge) return { mode: 'mock' as const, error: '桥接未初始化' };
    try {
      const mode = await bridge.start();
      process.stdout.write(`[lumina] engine retry -> ${mode}\n`);
      return { mode };
    } catch (err) {
      return { mode: 'mock' as const, error: (err as Error).message };
    }
  });
  ipcMain.handle('lumina:openPath', async (_e, p: string) => {
    await shell.openPath(p);
    return true;
  });
  ipcMain.handle('lumina:showInFolder', async (_e, p: string) => {
    shell.showItemInFolder(p);
    return true;
  });
  ipcMain.handle('lumina:confirm', async (_e, opts: { title: string; message: string; confirmLabel?: string }) => {
    if (!mainWindow) return false;
    const r = await dialog.showMessageBox(mainWindow, {
      type: 'warning',
      title: opts.title,
      message: opts.message,
      buttons: [opts.confirmLabel ?? '确定', '取消'],
      defaultId: 0,
      cancelId: 1,
    });
    return r.response === 0;
  });

  createWindow();

  if (isSmoke) {
    // 冒烟模式：跑完全模块场景（素材库→修图台→导入→健康→协议契约→主题）后截图并退出；
    // T1.7 起逐控件像素断言（19 控件 × ~1.2s）+ 夹具 import/引擎交叉验证 + 导出=预览断言
    // 入列，120s 预算不够 → 放宽到 240s（场景自身有 DONE/异常路径，防挂死靠进程退出）。
    setTimeout(async () => {
      try {
        const image = await mainWindow?.webContents.capturePage();
        if (image) {
          const out = path.join(homeDir, 'smoke-capture.png');
          require('node:fs').writeFileSync(out, image.toPNG());
          process.stdout.write(`[smoke] screenshot -> ${out}\n`);
        }
      } catch (err) {
        process.stdout.write(`[smoke] screenshot failed: ${(err as Error).message}\n`);
      }
      process.stdout.write('[smoke] timeout reached, quitting\n');
      await bridge?.shutdown();
      app.quit();
    }, 240_000);
  }

  if (isBaseline) {
    // 基线模式：渲染层走完 13 帧后打 `[baseline] DONE`，这里收尾；另设 3 分钟兜底防挂死
    let done = false;
    const finish = async (why: string) => {
      if (done) return;
      done = true;
      process.stdout.write(`[baseline] ${why}\n`);
      await bridge?.shutdown();
      app.quit();
    };
    const wc = mainWindow?.webContents;
    wc?.on('console-message', (_e, _l, message) => {
      if (message.includes('[baseline] DONE')) void finish('walk finished, quitting');
    });
    setTimeout(() => void finish('watchdog timeout, quitting'), 180_000);
  }

  if (isShellCheck) {
    // 外壳自检：渲染层跑完打 `[shellcheck] DONE` 后退出；60s 兜底
    let finished = false;
    const finish = async (why: string) => {
      if (finished) return;
      finished = true;
      process.stdout.write(`[shellcheck] ${why}\n`);
      await bridge?.shutdown();
      app.quit();
    };
    const wc = mainWindow?.webContents;
    wc?.on('console-message', (_e, _l, message) => {
      if (message.includes('[shellcheck] DONE')) void finish('check finished, quitting');
    });
    setTimeout(() => void finish('watchdog timeout, quitting'), 60_000);
  }
}

// 自定义文件协议必须在 app ready 前注册为特权方案
registerFileScheme();

app.whenReady().then(bootstrap);

app.on('window-all-closed', () => {
  if (process.platform !== 'darwin') app.quit();
});

app.on('before-quit', async (e) => {
  if (bridge) {
    e.preventDefault();
    const b = bridge;
    bridge = null;
    await b.shutdown();
    galleryServer?.close();
    app.quit();
  }
});
