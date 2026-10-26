// LUMINA 引擎子进程客户端（MCP 服务专用）
// 线路协议（docs/ipc-protocol.md）：4 字节小端长度前缀 + UTF-8 JSON，单帧 ≤ 8MiB，
// 引擎 stdin/stdout 上跑 JSON-RPC 2.0；本进程的 stdio 保留给 MCP 客户端，绝不混用。
// 职责：拉起 lumina-engine 子进程、请求-响应按 id 关联、event.job.* 任务状态跟踪、优雅退出。
import { spawn } from "node:child_process";
import { existsSync } from "node:fs";
import path from "node:path";
import { fileURLToPath } from "node:url";

const MAX_FRAME = 8 * 1024 * 1024; // 与引擎 rpc/frame.h kMaxFrameSize 一致
const TERMINAL_STATES = new Set(["done", "failed", "cancelled"]);
const sleep = (ms) => new Promise((resolve) => setTimeout(resolve, ms));

export class EngineError extends Error {
  constructor(code, message, data = null) {
    super(message);
    this.name = "EngineError";
    this.code = code;
    this.data = data;
  }
}

// 引擎可执行文件定位顺序：显式指定 → 仓库 build/engine/（mcp/ 的上一级）
export function resolveEnginePath(explicit) {
  const candidates = [];
  if (explicit) candidates.push(explicit);
  const here = path.dirname(fileURLToPath(import.meta.url));
  candidates.push(path.join(here, "..", "build", "engine", "lumina-engine.exe"));
  candidates.push(path.join(here, "..", "build", "engine", "lumina-engine"));
  for (const candidate of candidates) {
    if (existsSync(candidate)) return candidate;
  }
  return explicit || null; // 都没有则交给 spawn 报错（可能在 PATH 上）
}

export class EngineClient {
  constructor({ enginePath, home, requestTimeoutMs = 120000, log = () => {} } = {}) {
    this.enginePath = enginePath;
    this.home = home || "";
    this.requestTimeoutMs = requestTimeoutMs;
    this.log = log; // (level, line) => void，仅写 stderr，不碰 stdout
    this.child = null;
    this.exited = false;
    this.exitCode = null;
    this.nextId = 0;
    this.pending = new Map(); // id → {resolve, reject, timer, method}
    this.jobs = new Map(); // jobId → {jobId, kind, state, progress, detail, result?, error?, updatedAt}
    this.handshake = null; // system.initialize 的结果
    this.stderrTail = []; // 最近 60 行引擎 stderr，崩溃诊断用
    this._stdoutBuf = Buffer.alloc(0);
  }

  // 拉起引擎并完成握手（协议主版本必须为 1）
  async start() {
    if (this.child) return this.handshake;
    const args = [];
    if (this.home) args.push("--home", this.home);
    this.log("info", `spawning engine: ${this.enginePath} ${args.join(" ")}`);
    this.child = spawn(this.enginePath, args, {
      stdio: ["pipe", "pipe", "pipe"],
      windowsHide: true,
    });
    this.child.stdout.on("data", (chunk) => this._onStdout(chunk));
    this.child.stderr.on("data", (chunk) => this._onStderr(chunk));
    this.child.on("error", (err) => {
      this.exited = true;
      this._rejectAllPending(new EngineError(-32002, `引擎进程启动失败: ${err.message}`));
    });
    this.child.on("exit", (code, signal) => {
      this.exited = true;
      this.exitCode = code;
      const tail = this.stderrTail.slice(-8).join(" | ");
      this.log("warn", `engine exited code=${code} signal=${signal} ${tail}`);
      this._rejectAllPending(
        new EngineError(-32002, `引擎进程已退出 (code=${code ?? signal})${tail ? `: ${tail}` : ""}`),
      );
    });

    // 握手：单实例锁失败（App 已在运行）会在引擎侧 exit(3)，request 会先收到 exit 拒绝
    this.handshake = await this.request("system.initialize", {}, 30000);
    const proto = String(this.handshake?.protocolVersion ?? "");
    if (!proto.startsWith("1.")) {
      await this.shutdown().catch(() => {});
      throw new EngineError(
        -32002,
        `引擎协议版本不兼容: ${proto}（本 MCP 服务只支持 1.x）`,
      );
    }
    this.log(
      "info",
      `engine ready: v${this.handshake.engineVersion} protocol ${proto} home=${this.handshake.homeDir}`,
    );
    return this.handshake;
  }

  get ready() {
    return !!this.handshake && !this.exited;
  }

  // JSON-RPC 请求；失败抛 EngineError（含引擎业务错误码）
  request(method, params = {}, timeoutMs = this.requestTimeoutMs) {
    if (!this.child || this.exited) {
      return Promise.reject(new EngineError(-32002, "引擎未运行"));
    }
    const id = ++this.nextId;
    return new Promise((resolve, reject) => {
      const timer = setTimeout(() => {
        this.pending.delete(id);
        reject(new EngineError(-32001, `引擎请求超时: ${method} (${timeoutMs}ms)`));
      }, timeoutMs);
      this.pending.set(id, { resolve, reject, timer, method });
      this._write({ jsonrpc: "2.0", id, method, params });
    });
  }

  // 等待任务终态（done|failed|cancelled），基于 event.job.* 通知维护的状态
  async waitForJob(jobId, timeoutMs = 180000) {
    const deadline = Date.now() + timeoutMs;
    for (;;) {
      const job = this.jobs.get(jobId);
      if (job && TERMINAL_STATES.has(job.state)) return job;
      if (this.exited) throw new EngineError(-32002, "引擎已退出，任务状态未知");
      if (Date.now() > deadline) {
        return {
          ...(job ?? { jobId, state: "queued", progress: 0 }),
          timedOut: true,
          note: `等待 ${timeoutMs}ms 未到终态，任务仍在后台运行，可稍后用 lumina_job_status 查询`,
        };
      }
      await sleep(300);
    }
  }

  // 启动长任务并可选地等到终态（默认等待，agent 体验最好）
  async startJob(method, params, { wait = true, waitTimeoutMs = 180000 } = {}) {
    const result = await this.request(method, params, 30000);
    const jobId = result?.jobId;
    if (!wait || jobId == null) return { ...result, state: "queued" };
    const final = await this.waitForJob(jobId, waitTimeoutMs);
    // 终态时把任务 result/error 平铺进返回值，agent 无需二次查询
    const { jobId: _j, ...rest } = final;
    return { jobId, ...rest };
  }

  async cancelJob(jobId) {
    // 引擎无统一取消入口：import/health/export 各有 cancel，逐个尝试，命中即返回
    for (const method of ["import.cancel", "health.cancelJob", "export.cancelJob"]) {
      try {
        const r = await this.request(method, { jobId }, 10000);
        return { cancelled: true, via: method, ...r };
      } catch {
        /* 换下一个入口 */
      }
    }
    return { cancelled: false, note: "该任务可能已结束或不支持取消" };
  }

  async shutdown() {
    if (!this.child || this.exited) return;
    try {
      await this.request("system.shutdown", {}, 8000);
    } catch {
      /* 引擎可能已自行退出 */
    }
    const deadline = Date.now() + 10000;
    while (!this.exited && Date.now() < deadline) await sleep(100);
    if (!this.exited) {
      this.log("warn", "engine did not exit gracefully, killing");
      this.child.kill();
    }
  }

  // ---- 内部：帧协议 ----
  _onStdout(chunk) {
    this._stdoutBuf = Buffer.concat([this._stdoutBuf, chunk]);
    for (;;) {
      if (this._stdoutBuf.length < 4) return;
      const length = this._stdoutBuf.readUInt32LE(0);
      if (length > MAX_FRAME) {
        this.log("error", `frame too large (${length} bytes), protocol violation, killing engine`);
        this.child?.kill();
        return;
      }
      if (this._stdoutBuf.length < 4 + length) return;
      const payload = this._stdoutBuf.subarray(4, 4 + length).toString("utf8");
      this._stdoutBuf = this._stdoutBuf.subarray(4 + length);
      let msg;
      try {
        msg = JSON.parse(payload);
      } catch (err) {
        this.log("error", `engine frame json parse failed: ${err.message}`);
        continue;
      }
      if (msg.id != null) this._onResponse(msg);
      else if (msg.method) this._onNotification(msg.method, msg.params ?? {});
    }
  }

  _onResponse(msg) {
    const entry = this.pending.get(msg.id);
    if (!entry) return;
    this.pending.delete(msg.id);
    clearTimeout(entry.timer);
    if (msg.error) {
      entry.reject(
        new EngineError(msg.error.code ?? -32603, msg.error.message ?? "engine error", msg.error.data ?? null),
      );
    } else {
      entry.resolve(msg.result ?? {});
    }
  }

  _onNotification(method, params) {
    if (method === "event.job.progress" || method === "event.job.completed") {
      const jobId = Number(params.jobId);
      if (Number.isFinite(jobId)) {
        const prev = this.jobs.get(jobId) ?? {};
        this.jobs.set(jobId, {
          jobId,
          kind: params.kind ?? prev.kind ?? "",
          state: params.state ?? prev.state ?? "running",
          progress: params.progress ?? prev.progress ?? 0,
          detail: params.detail ?? prev.detail ?? "",
          result: params.result ?? prev.result,
          error: params.error ?? prev.error,
          updatedAt: new Date().toISOString(),
        });
      }
    }
    // event.thumb.ready 等其余通知对 agent 无直接价值，忽略
  }

  _onStderr(chunk) {
    for (const line of chunk.toString("utf8").split(/\r?\n/)) {
      if (!line.trim()) continue;
      this.stderrTail.push(line);
      if (this.stderrTail.length > 60) this.stderrTail.shift();
      this.log("engine", line);
    }
  }

  _write(obj) {
    if (!this.child || this.exited) return;
    const payload = Buffer.from(JSON.stringify(obj), "utf8");
    const header = Buffer.alloc(4);
    header.writeUInt32LE(payload.length, 0);
    this.child.stdin.write(Buffer.concat([header, payload]));
  }

  _rejectAllPending(error) {
    for (const [, entry] of this.pending) {
      clearTimeout(entry.timer);
      entry.reject(error);
    }
    this.pending.clear();
  }
}
