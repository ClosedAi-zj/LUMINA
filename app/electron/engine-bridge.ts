/**
 * EngineBridge —— Electron 主进程侧的引擎子进程管理。
 *
 * 契约（docs/ipc-protocol.md §1/§2/§7）：
 * - 通道：lumina-engine.exe 的 stdin/stdout，UTF-8 JSON（无 BOM）
 * - 帧：4 字节小端 uint32 载荷长度 + 载荷字节；单帧上限 8 MiB
 * - 引擎 → UI 只发通知（event.*）；UI 是唯一请求方
 * - 超时默认 10s；超时后写类请求不重发（防重复落盘），只重发幂等读请求（由调用方决定）
 * - 崩溃看护：非 system.shutdown 导致的退出 → 3s 内自动重启 + 重放 system.initialize
 * - 当引擎 exe 不存在或无法启动时，自动切入内置 Mock（与协议字段完全一致），UI 可独立运行
 */

import { spawn, ChildProcessWithoutNullStreams } from 'node:child_process';
import { existsSync, readFileSync } from 'node:fs';
import os from 'node:os';
import path from 'node:path';

export const PROTOCOL_VERSION = '1.0';
const MAX_FRAME_BYTES = 8 * 1024 * 1024;
const DEFAULT_TIMEOUT_MS = 10_000;
const RESTART_DELAY_MS = 800; // 崩溃后 800ms 内重启，满足「3s 内自动重启」
/** 重启熔断：20s 窗口内连续 3 次退出 → 停止自动重启（防单实例锁冲突等场景的 spawn 风暴） */
const RESTART_BREAKER_COUNT = 3;
const RESTART_BREAKER_WINDOW_MS = 20_000;
/** 首次握手（system.initialize）超时：v1.1.1 由 15s 放宽到 60s，兜底大图库冷启动迁移/扫描耗时 */
const ENGINE_HANDSHAKE_TIMEOUT_MS = 60_000;
/** 启动期踩到单实例锁（上一实例引擎尚未退净）时的重试次数与间隔：8 次 × 1s ≈ 8s 观察窗 */
const ENGINE_START_RETRIES = 8;
const ENGINE_START_RETRY_MS = 1000;

/** 引擎 → UI 的通知（无 id） */
export interface EngineNotification {
  jsonrpc: '2.0';
  method: string;
  params?: unknown;
}

/** 生命周期事件（桥接层自身发出，非协议事件） */
export type BridgeLifecycle = 'starting' | 'ready' | 'crashed-restarting' | 'recovered' | 'stopped';

/**
 * 探测单实例锁当前的占用形态（只用于日志与诊断，不作为重试开关）。
 * 锁 = {home}/engine.lock，引擎以 dwShareMode=0 独占打开 → 别的进程连读都读不到，
 * 因此「读失败」本身就是有人的证据，只有 ENOENT 才代表确实没人：
 *  - free  : 文件不存在（含上一实例已退净）
 *  - live  : 文件存在但读不到（独占句柄 / delete-pending）→ 可能是活实例，也可能在收尾
 *  - stale : 能读到内容但 pid 已不在
 *  - held  : 能读到内容且 pid 存活（正常不会走到，独占模式下读不到）
 */
function probeEngineLock(homeDir: string): 'free' | 'live' | 'stale' | 'held' {
  try {
    const txt = readFileSync(path.join(homeDir, 'engine.lock'), 'utf8');
    const m = /pid=(\d+)/.exec(txt);
    if (!m) return 'stale';
    const pid = Number(m[1]);
    if (!Number.isFinite(pid) || pid <= 0) return 'stale';
    try {
      process.kill(pid, 0); // 仅探活
      return 'held';
    } catch {
      return 'stale';
    }
  } catch (err) {
    const code = (err as NodeJS.ErrnoException).code;
    return code === 'ENOENT' ? 'free' : 'live';
  }
}

interface PendingRequest {
  id: number;
  method: string;
  timer: ReturnType<typeof setTimeout>;
  resolve: (result: unknown) => void;
  reject: (err: RpcCallError) => void;
  startedAt: number;
}

export class RpcCallError extends Error {
  readonly code: number;
  readonly data: unknown;
  constructor(code: number, message: string, data?: unknown) {
    super(message);
    this.name = 'RpcCallError';
    this.code = code;
    this.data = data;
  }
}

/** 引擎传输层接口：真引擎与 Mock 共用同一收发抽象 */
interface EngineTransport {
  /** 发送一帧（JSON-RPC 信封），返回原始响应信封（result 或 error） */
  call(envelope: { id: number; method: string; params: unknown }): Promise<{ result?: unknown; error?: { code: number; message: string; data?: unknown } }>;
  dispose(): void;
}

export class EngineBridge {
  private enginePath: string;
  private homeDir: string;
  private transport: EngineTransport | null = null;
  private mode: 'engine' | 'mock' = 'engine';
  private proc: ChildProcessWithoutNullStreams | null = null;
  private pending = new Map<number, PendingRequest>();
  private nextId = 1;
  private initialized = false;
  private gracefulShutdown = false;
  private restarting = false;
  private restartTimer: ReturnType<typeof setTimeout> | null = null;
  /** 重启熔断计数（最近退出时间戳） */
  private crashTimes: number[] = [];
  /** 熔断触发后只允许 Mock（本次 bridge 生命周期内不再 spawn 真引擎） */
  private breakerTripped = false;
  private eventListeners = new Set<(n: EngineNotification) => void>();
  private lifecycleListeners = new Set<(s: BridgeLifecycle) => void>();
  private initParams: { uiVersion: string } = { uiVersion: '1.3.0' };
  /** initialize 返回的握手信息（供 UI 状态栏显示引擎版本） */
  engineInfo: Record<string, unknown> = {};

  constructor(opts: { enginePath: string; homeDir: string }) {
    this.enginePath = opts.enginePath;
    this.homeDir = opts.homeDir;
  }

  get isMock(): boolean {
    return this.mode === 'mock';
  }

  get hasProcess(): boolean {
    return this.proc !== null;
  }

  onEvent(cb: (n: EngineNotification) => void): () => void {
    this.eventListeners.add(cb);
    return () => this.eventListeners.delete(cb);
  }

  onLifecycle(cb: (s: BridgeLifecycle) => void): () => void {
    this.lifecycleListeners.add(cb);
    return () => this.lifecycleListeners.delete(cb);
  }

  private emitLifecycle(s: BridgeLifecycle): void {
    for (const cb of this.lifecycleListeners) cb(s);
  }

  private emitEvent(n: EngineNotification): void {
    for (const cb of this.eventListeners) {
      try {
        cb(n);
      } catch {
        // 事件监听器异常不允许拖垮桥接层（stderr 记录由调用方负责）
      }
    }
  }

  /** 真引擎 stdio 传输：帧写入复用 writeFrame，响应复用 pending 表（引擎单请求串行，协议 §7.2） */
  private makeStdioTransport(): EngineTransport {
    return {
      call: (envelope) =>
        new Promise((resolve, reject) => {
          const id = this.nextId++;
          const timer = setTimeout(() => {
            this.pending.delete(id);
            reject(new RpcCallError(-32603, `请求超时（${envelope.method}，60000ms）`));
          }, 60_000);
          this.pending.set(id, {
            id,
            method: envelope.method,
            timer,
            resolve: (result) => resolve({ result }),
            reject,
            startedAt: Date.now(),
          });
          this.writeFrame({ jsonrpc: '2.0', id, method: envelope.method, params: envelope.params });
        }),
      dispose: () => undefined,
    };
  }

  /** 启动：优先真引擎；exe 不存在或握手失败 → Mock 兜底（绝不阻塞 UI） */
  async start(): Promise<'engine' | 'mock'> {
    this.emitLifecycle('starting');
    if (this.breakerTripped) {
      process.stdout.write('[engine-bridge] 重启熔断生效，直接使用内置 Mock\n');
    } else if (existsSync(this.enginePath)) {
      let lastErr: unknown = null;
      // 上一实例的引擎进程在 Electron 被强杀后仍会存活到「stdin 关闭 → 退出」完成（实测数秒），
      // 这段时间内新引擎会撞单实例锁并退出 code=3，旧逻辑直接降级 Mock。
      // 但 Mock 与真实 RAW 参数不同源，进修图台会触发已知的 typed-array 崩溃 ——
      // 所以撞锁时先等锁释放再试，只有真起不来才降级。
      for (let attempt = 0; attempt < ENGINE_START_RETRIES; attempt += 1) {
        try {
          // 受控启动期间标记 restarting：抑制 close 处理器里的「崩溃看护 + 熔断计数」，
          // 否则 3 次重试会把自己算成崩溃并触发熔断
          this.restarting = true;
          await this.spawnEngine();
          this.mode = 'engine';
          this.emitLifecycle('ready');
          return 'engine';
        } catch (err) {
          lastErr = err;
          this.teardownProcess();
          const msg = err instanceof Error ? err.message : String(err);
          const lockConflict = /code=3|another engine instance/i.test(msg);
          const last = attempt === ENGINE_START_RETRIES - 1;
          if (!lockConflict || last) break;
          process.stdout.write(
            `[engine-bridge] 引擎单实例锁未释放（probe=${probeEngineLock(this.homeDir)}），${ENGINE_START_RETRY_MS}ms 后重试（${attempt + 1}/${ENGINE_START_RETRIES - 1}）\n`,
          );
          await new Promise((r) => setTimeout(r, ENGINE_START_RETRY_MS));
        } finally {
          this.restarting = false;
        }
      }
      process.stderr.write(
        `[engine-bridge] 真引擎启动失败，降级 Mock：${lastErr instanceof Error ? lastErr.message : String(lastErr)}\n`,
      );
    } else {
      process.stdout.write(`[engine-bridge] 引擎 exe 不存在（${this.enginePath}），使用内置 Mock\n`);
    }
    // Mock 兜底
    const { MockTransport } = await import('./mock/mock-engine');
    const mock = new MockTransport(this.homeDir);
    mock.onNotification((n) => this.emitEvent(n));
    this.transport = mock;
    this.mode = 'mock';
    this.engineInfo = {
      protocolVersion: PROTOCOL_VERSION,
      engineVersion: 'mock-1.3.0',
      capabilities: { libraw: false, fts5: true, codecs: ['h264', 'h265'], mock: true },
      homeDir: this.homeDir.replace(/\\/g, '/'),
      dbPath: `${this.homeDir.replace(/\\/g, '/')}/lumina.db`,
    };
    this.initialized = true;
    this.emitLifecycle('ready');
    return 'mock';
  }

  /** spawn 真引擎 + 重放 initialize 握手 */
  private spawnEngine(): Promise<void> {
    return new Promise((resolve, reject) => {
      let settled = false;
      const proc = spawn(this.enginePath, ['--home', this.homeDir], {
        stdio: ['pipe', 'pipe', 'pipe'],
        windowsHide: true,
      });
      this.proc = proc;
      this.gracefulShutdown = false;
      // 修复：真引擎模式此前从未创建 transport，渲染进程 RPC 一律报「引擎未启动」
      // （历史运行全部静默降级到 Mock）。此处挂接 stdio 传输，复用 pending/writeFrame 机制。
      this.transport = this.makeStdioTransport();

      let buffer: Buffer = Buffer.alloc(0);
      proc.stdout.on('data', (chunk: Buffer) => {
        buffer = Buffer.concat([buffer, chunk]);
        buffer = this.drainFrames(buffer);
      });
      proc.stderr.on('data', (chunk: Buffer) => {
        // 引擎日志一律走 stderr（协议 §1），转发到主进程 stdout 便于调试
        process.stderr.write(`[lumina-engine] ${chunk.toString('utf8')}`);
      });
      proc.on('error', (err) => {
        if (!settled) {
          settled = true;
          reject(err);
        }
      });
      proc.on('close', (code) => {
        this.proc = null;
        this.failAllPending(new RpcCallError(-32603, `引擎进程已退出（code=${code}）`));
        if (this.gracefulShutdown || this.restarting) return;
        // 重启熔断：短窗口内连续异常退出（如单实例锁冲突、二进制损坏）→ 停止 spawn 风暴，交由上层降级
        const now = Date.now();
        this.crashTimes = [...this.crashTimes.filter((t) => now - t < RESTART_BREAKER_WINDOW_MS), now];
        if (this.crashTimes.length >= RESTART_BREAKER_COUNT) {
          this.breakerTripped = true;
          process.stderr.write(`[engine-bridge] ${RESTART_BREAKER_WINDOW_MS / 1000}s 内引擎连续退出 ${this.crashTimes.length} 次，触发重启熔断\n`);
          void this.start();
          return;
        }
        // 崩溃看护：自动重启 + 重放 initialize（协议 §7.4）
        this.restarting = true;
        this.emitLifecycle('crashed-restarting');
        this.restartTimer = setTimeout(async () => {
          this.restarting = false;
          try {
            await this.spawnEngine();
            // initialize 重放；参数以 sidecar 为准，天然可恢复
            await this.rawRequest('system.initialize', this.initParams, ENGINE_HANDSHAKE_TIMEOUT_MS);
            this.initialized = true;
            this.emitLifecycle('recovered');
          } catch {
            // 重启失败 → 降级 Mock，保证 UI 可用
            await this.start();
          }
        }, RESTART_DELAY_MS);
      });

      // 发送 initialize 握手，15s 内未收到响应视为启动失败
      const initId = this.nextId++;
      const timer = setTimeout(() => {
        if (!settled) {
          settled = true;
          reject(new Error('initialize 握手超时'));
        }
      }, ENGINE_HANDSHAKE_TIMEOUT_MS);
      this.pending.set(initId, {
        id: initId,
        method: 'system.initialize',
        timer,
        resolve: (result) => {
          clearTimeout(timer);
          this.initialized = true;
          this.engineInfo = (result as Record<string, unknown>) ?? {};
          if (!settled) {
            settled = true;
            resolve();
          }
        },
        reject: (err) => {
          clearTimeout(timer);
          if (!settled) {
            settled = true;
            reject(err);
          }
        },
        startedAt: Date.now(),
      });
      this.writeFrame({
        jsonrpc: '2.0',
        id: initId,
        method: 'system.initialize',
        params: this.initParams,
      });
    });
  }

  /** 按协议帧格式排空缓冲区：4 字节 LE 长度前缀 + JSON 载荷 */
  private drainFrames(buffer: Buffer): Buffer {
    for (;;) {
      if (buffer.length < 4) return buffer;
      const len = buffer.readUInt32LE(0);
      if (len > MAX_FRAME_BYTES) {
        // 超限即断开并记日志（协议 §1）
        process.stderr.write(`[engine-bridge] 帧超限 ${len}B > ${MAX_FRAME_BYTES}B，断开引擎\n`);
        this.teardownProcess();
        return Buffer.alloc(0);
      }
      if (buffer.length < 4 + len) return buffer;
      const payload = buffer.subarray(4, 4 + len).toString('utf8');
      buffer = buffer.subarray(4 + len);
      let msg: unknown;
      try {
        msg = JSON.parse(payload);
      } catch {
        continue; // 非法 JSON（PARSE_ERROR 语义）：丢弃该帧，不崩溃
      }
      this.handleIncoming(msg);
    }
  }

  private handleIncoming(msg: unknown): void {
    const m = msg as { id?: number | null; result?: unknown; error?: { code: number; message: string; data?: unknown }; method?: string; params?: unknown };
    if (typeof m.id === 'number') {
      const p = this.pending.get(m.id);
      if (!p) return;
      this.pending.delete(m.id);
      clearTimeout(p.timer);
      if (m.error) {
        p.reject(new RpcCallError(m.error.code, m.error.message, m.error.data));
      } else {
        p.resolve(m.result);
      }
      return;
    }
    if (typeof m.method === 'string') {
      this.emitEvent({ jsonrpc: '2.0', method: m.method, params: m.params });
    }
  }

  private writeFrame(envelope: unknown): void {
    if (!this.proc) return;
    const payload = Buffer.from(JSON.stringify(envelope), 'utf8');
    const header = Buffer.alloc(4);
    header.writeUInt32LE(payload.length, 0);
    this.proc.stdin.write(Buffer.concat([header, payload]));
  }

  private failAllPending(err: RpcCallError): void {
    for (const [, p] of this.pending) {
      clearTimeout(p.timer);
      p.reject(err);
    }
    this.pending.clear();
  }

  private teardownProcess(): void {
    this.gracefulShutdown = true;
    // 取消挂起中的自动重启（start 失败降级 / shutdown 时不再触发残留 spawn）
    if (this.restartTimer) {
      clearTimeout(this.restartTimer);
      this.restartTimer = null;
      this.restarting = false;
    }
    if (this.proc) {
      try {
        this.proc.kill();
      } catch {
        /* ignore */
      }
      this.proc = null;
    }
  }

  private rawRequest(method: string, params: unknown, timeoutMs: number): Promise<unknown> {
    if (!this.transport) throw new RpcCallError(-32603, '引擎未启动');
    const id = this.nextId++;
    return this.transport.call({ id, method, params }).then((resp) => {
      if (resp.error) throw new RpcCallError(resp.error.code, resp.error.message, resp.error.data);
      return resp.result;
    });
  }

  /** 对渲染进程暴露的统一请求入口 */
  async request(method: string, params: unknown, timeoutMs = DEFAULT_TIMEOUT_MS): Promise<unknown> {
    if (!this.transport) throw new RpcCallError(-32603, '引擎未启动');
    if (!this.initialized && method !== 'system.initialize' && method.startsWith('mock.')) {
      // mock 模式无需握手；真引擎模式下未握手前放行（握手在 start 内完成）
    }
    const id = this.nextId++;
    const startedAt = Date.now();
    const resp = await this.transport.call({ id, method, params });
    void startedAt;
    if (resp.error) throw new RpcCallError(resp.error.code, resp.error.message, resp.error.data);
    return resp.result;
  }

  /** 带超时的请求（默认 10s，协议 §7.2）。超时后拒绝，不重发（防重复落盘）。 */
  async requestWithTimeout(method: string, params: unknown, timeoutMs: number): Promise<unknown> {
    if (!this.transport) throw new RpcCallError(-32603, '引擎未启动');
    const id = this.nextId++;
    let timer: ReturnType<typeof setTimeout> | null = null;
    const timeout = new Promise<never>((_, reject) => {
      timer = setTimeout(() => reject(new RpcCallError(-32603, `请求超时（${method}，${timeoutMs}ms）`)), timeoutMs);
    });
    try {
      const resp = await Promise.race([this.transport.call({ id, method, params }), timeout]);
      const r = resp as { error?: { code: number; message: string; data?: unknown }; result?: unknown };
      if (r.error) throw new RpcCallError(r.error.code, r.error.message, r.error.data);
      return r.result;
    } finally {
      if (timer) clearTimeout(timer);
    }
  }

  async shutdown(): Promise<void> {
    this.gracefulShutdown = true;
    try {
      if (this.mode === 'engine') await this.rawRequest('system.shutdown', {}, 3_000);
    } catch {
      /* 引擎可能已退出 */
    }
    this.teardownProcess();
    this.transport?.dispose();
    this.transport = null;
    this.emitLifecycle('stopped');
  }
}

export function resolveEnginePath(appRoot: string): string {
  // 优先环境变量 → {repo}/build/engine/lumina-engine.exe
  const fromEnv = process.env.LUMINA_ENGINE;
  if (fromEnv && existsSync(fromEnv)) return fromEnv;
  const candidate = path.resolve(appRoot, 'build', 'engine', 'lumina-engine.exe');
  return candidate;
}

/**
 * 运行时主目录（`--home`）解析。
 *
 * 三条互斥来源，优先级从高到低：
 * 1. `LUMINA_HOME` 环境变量 —— 排障与冒烟脚本（scripts/dev.ps1、多个 gate 脚本）
 *    依赖它，必须保留且优先级最高；
 * 2. 打包版（`app.isPackaged` 且非开发/冒烟/基线）—— 落 `{userData}/library`
 *    （Windows 即 `%APPDATA%/LUMINA/library`），与开发档案物理隔离；
 * 3. 其余（开发 / 冒烟 / 基线）—— 继续用 `~/.lumina`。
 *
 * 为什么必须隔离：开发机 `~/.lumina` 里有冒烟生成的 mocklib（上万张程序化测试图）
 * 和 5 个 mock 根目录。打包版若共用同一 home，用户一启动就看到满屏测试图
 * （用户实测反馈"发布的 exe 存在测试用的图片"）。冒烟链路大量硬编码
 * `~/.lumina/mocklib`，所以开发侧路径不能改，只能让打包版换目录。
 */
export interface HomeDirOptions {
  /** 是否打包版（主进程传 app.isPackaged） */
  isPackaged?: boolean;
  /** Electron userData 目录（主进程传 app.getPath('userData')） */
  userDataDir?: string;
  /** 开发 / 冒烟 / 基线：强制走 `~/.lumina` */
  useDevHome?: boolean;
}

export function resolveHomeDir(opts: HomeDirOptions = {}): string {
  const fromEnv = process.env.LUMINA_HOME;
  if (fromEnv && fromEnv.trim() !== '') return path.resolve(fromEnv.trim());
  if (opts.isPackaged && !opts.useDevHome && opts.userDataDir) {
    // 独立档案：引擎首次启动会在此目录自建 db/cache/logs，库为 0 张照片
    return path.join(opts.userDataDir, 'library');
  }
  // 回退必须是可移植的默认主目录（与引擎 --home 缺省一致）；
  // 硬编码开发机路径会让最终用户的数据落在不存在的盘符路径上。
  return path.join(os.homedir(), '.lumina');
}

/** 旧版（开发期）档案目录：仅用于"检测到但不迁移"的日志提示 */
export function legacyHomeDir(): string {
  return path.join(os.homedir(), '.lumina');
}
