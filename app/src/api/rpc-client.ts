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

class RpcClient {
  private listeners = new Set<EventCallback>();

  async request<T = unknown>(method: string, params?: unknown): Promise<T> {
    try {
      return (await window.lumina.request(method, params)) as T;
    } catch (err) {
      // preload invoke 抛出的错误以 message 形态携带 code
      const e = err as { code?: number; message?: string; data?: unknown; name?: string };
      if (typeof e?.code === 'number') {
        throw new RpcError(e.code, e.message ?? '未知错误', e.data);
      }
      throw new RpcError(-32603, e?.message ?? String(err), e?.data);
    }
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

  /** 启动事件泵（App 挂载时调用一次） */
  start(): void {
    window.lumina.onEvent((n) => {
      for (const cb of this.listeners) cb(n.method, n.params);
    });
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
