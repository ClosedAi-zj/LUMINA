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
  /** 自定义标题栏窗口控制（frame:false 无边框窗口） */
  win: {
    minimize(): Promise<boolean>;
    /** 切换最大化/还原，返回切换后的 isMaximized */
    toggleMaximize(): Promise<boolean>;
    close(): Promise<boolean>;
    isMaximized(): Promise<boolean>;
    /** 订阅最大化状态推送（主进程 maximize/unmaximize 时触发） */
    onMaximizeChange(cb: (maximized: boolean) => void): () => void;
  };
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
  win: {
    minimize: () => ipcRenderer.invoke('lumina:win:minimize'),
    toggleMaximize: () => ipcRenderer.invoke('lumina:win:toggleMaximize'),
    close: () => ipcRenderer.invoke('lumina:win:close'),
    isMaximized: () => ipcRenderer.invoke('lumina:win:isMaximized'),
    onMaximizeChange: (cb: (maximized: boolean) => void) => {
      const listener = (_e: unknown, v: boolean) => cb(v);
      ipcRenderer.on('lumina:win:maximized', listener);
      return () => ipcRenderer.removeListener('lumina:win:maximized', listener);
    },
  },
} satisfies LuminaPreload);
