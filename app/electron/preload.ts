/**
 * preload —— contextBridge 暴露类型化 RPC + 事件订阅给渲染进程（window.lumina）。
 * 渲染进程不直接持有 Node 能力（UI 纪律：渲染层不碰文件系统，一律经 IPC）。
 */

import { contextBridge, ipcRenderer } from 'electron';

export interface LuminaPreload {
  /** 同步可读：冒烟模式标识 */
  readonly isSmoke: boolean;
  /** JSON-RPC 请求（主进程侧统一 10s 超时与 mock 兜底） */
  request(method: string, params?: unknown): Promise<unknown>;
  /** 订阅引擎通知（event.*）与桥接生命周期 */
  onEvent(cb: (notification: { method: string; params: unknown }) => void): () => void;
  onLifecycle(cb: (state: string) => void): () => void;
  /** 引擎连接信息（mock/real、引擎版本） */
  getInfo(): Promise<{ isMock: boolean; enginePath: string; homeDir: string; engineInfo: Record<string, unknown>; isSmoke: boolean; isPackaged: boolean }>;
  /** WP-B3：Mock 兜底后重试连接真引擎（成功后需重载窗口重建会话） */
  engineRetry(): Promise<{ mode: 'engine' | 'mock'; error?: string }>;
  /** 在系统资源管理器中打开/定位 */
  openPath(path: string): Promise<boolean>;
  showInFolder(path: string): Promise<boolean>;
  /** 破坏性操作二次确认（原生对话框） */
  confirm(opts: { title: string; message: string; confirmLabel?: string }): Promise<boolean>;
}

contextBridge.exposeInMainWorld('lumina', {
  /** 同步可读：冒烟模式标识（渲染进程启动即可判定） */
  get isSmoke() {
    return process.env.LUMINA_SMOKE === '1';
  },
  request: (method: string, params?: unknown) => ipcRenderer.invoke('lumina:rpc', method, params),
  onEvent: (cb: (n: { method: string; params: unknown }) => void) => {
    const listener = (_e: unknown, n: { method: string; params: unknown }) => cb(n);
    ipcRenderer.on('lumina:event', listener);
    return () => ipcRenderer.removeListener('lumina:event', listener);
  },
  onLifecycle: (cb: (s: string) => void) => {
    const listener = (_e: unknown, s: string) => cb(s);
    ipcRenderer.on('lumina:lifecycle', listener);
    return () => ipcRenderer.removeListener('lumina:lifecycle', listener);
  },
  getInfo: () => ipcRenderer.invoke('lumina:info'),
  engineRetry: () => ipcRenderer.invoke('lumina:engineRetry'),
  openPath: (p: string) => ipcRenderer.invoke('lumina:openPath', p),
  showInFolder: (p: string) => ipcRenderer.invoke('lumina:showInFolder', p),
  confirm: (opts: { title: string; message: string; confirmLabel?: string }) => ipcRenderer.invoke('lumina:confirm', opts),
} satisfies LuminaPreload);
