/**
 * RPC 客户端（渲染进程侧）：请求封装、错误码 → 用户可见文案映射、事件总线。
 * 真实传输由 preload 的 window.lumina 提供（主进程统一超时 10s / mock 兜底）。
 */

import { ERROR_MESSAGES } from './protocol';

export class RpcError extends Error {
  readonly code: number;
  readonly data: unknown;
  readonly messageZh: string;
  constructor(code: number, message: string, data?: unknown) {
    super(message);
    this.name = 'RpcError';
    this.code = code;
    this.data = data;
    this.messageZh = ERROR_MESSAGES[code] ?? message;
  }
}

type EventCallback = (method: string, params: unknown) => void;

/**
 * 主进程 RPC 错误信封（与 `electron/main.ts` 的 `RpcErrorEnvelope` 逐字对应，两处改要同步）。
 *
 * 为什么需要它：`ipcMain.handle` 里 throw 的错误经 `ipcRenderer.invoke` 到渲染层后
 * **只剩 message**（Electron 只序列化 message，且会加
 * `Error invoking remote method 'lumina:rpc': RpcCallError: ` 前缀），code / data 全丢。
 * B8 之前因此所有引擎错误都被包成 -32603，`ERROR_MESSAGES` 的本地化完全用不上，
 * UI 上显示的是那串英文前缀。现在主进程改为返回本信封，这里还原成 RpcError。
 * 注意：信封只可能是错误，成功结果原样透传（引擎返回 JSON 不会有该标记）。
 */
interface RpcErrorEnvelope {
  __luminaRpcError: true;
  code: number;
  message: string;
  data?: unknown;
}

function isRpcErrorEnvelope(v: unknown): v is RpcErrorEnvelope {
  return (
    v !== null &&
    typeof v === 'object' &&
    (v as { __luminaRpcError?: unknown }).__luminaRpcError === true &&
    typeof (v as { code?: unknown }).code === 'number' &&
    typeof (v as { message?: unknown }).message === 'string'
  );
}

class RpcClient {
  private listeners = new Set<EventCallback>();
  /** 事件订阅是否已建立（幂等标记：重复 start 会重复投递每个事件） */
  private started = false;
  /** start() 拿到的退订函数（保留以便将来重启/热更新时清理） */
  private offEvent: (() => void) | null = null;

  async request<T = unknown>(method: string, params?: unknown): Promise<T> {
    let res: unknown;
    try {
      res = await window.lumina.request(method, params);
    } catch (err) {
      // 传输层失败（主进程未注册通道 / 主进程崩溃 / invoke 被拒）：这里拿不到引擎原文
      const e = err as { code?: number; message?: string; data?: unknown; name?: string };
      if (typeof e?.code === 'number') {
        throw new RpcError(e.code, e.message ?? '未知错误', e.data);
      }
      throw new RpcError(-32603, e?.message ?? String(err), e?.data);
    }
    // 业务失败走信封：code 与引擎原文都在，RpcError.messageZh 才能映射成中文文案
    if (isRpcErrorEnvelope(res)) {
      throw new RpcError(res.code, res.message, res.data);
    }
    return res as T;
  }

  onEvent(cb: EventCallback): () => void {
    this.listeners.add(cb);
    return () => this.listeners.delete(cb);
  }

  /** 订阅单一事件名 */
  on(method: string, cb: (params: unknown) => void): () => void {
    return this.onEvent((m, params) => {
      if (m === method) cb(params);
    });
  }

  /**
   * 启动事件泵（App 挂载时调用一次）。
   *
   * 必须幂等：App 的启动 effect 依赖 [bootAttempt]，点「重试启动」会再跑一遍；
   * 旧实现每次都 `window.lumina.onEvent(...)` 且从不移除监听 ——
   * 于是重试后同一个引擎事件会被投递 N 次（重复 toast、进度跳变、计数翻倍）。
   */
  start(): void {
    if (this.started) return;
    this.started = true;
    this.offEvent = window.lumina.onEvent((n) => {
      for (const cb of this.listeners) cb(n.method, n.params);
    });
  }

  /** 停止事件泵（测试/热更可用；正常运行时不需要） */
  stop(): void {
    this.offEvent?.();
    this.offEvent = null;
    this.started = false;
  }
}

export const rpc = new RpcClient();

/** 统一错误 → 用户可见文案（RpcError 用 messageZh，其余降级 message） */
export function errText(err: unknown): string {
  if (err instanceof RpcError) return err.messageZh;
  if (err instanceof Error) return err.message;
  return String(err);
}

/**
 * 引擎落盘路径 → 渲染进程可加载 URL。
 * Windows 盘符作为 host（lumina-file://D/...），由主进程受控只读服务。
 */
export function fileUrl(path: string): string {
  const normalized = path.replace(/\\/g, '/').replace(/^([a-zA-Z]):/, '$1');
  return `lumina-file://${encodeURI(normalized)}`;
}
