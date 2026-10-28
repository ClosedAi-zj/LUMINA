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
  /** M3-03：打开文件对话框（PSD / 图像），返回选中路径或 null */
  pickFile(opts?: { title?: string; extensions?: string[] }): Promise<string | null>;
  /** 导入向导：选择目录（重定位缺失文件夹 / 指定来源目录），返回选中路径或 null */
  pickDirectory(opts?: { title?: string; defaultPath?: string }): Promise<string | null>;
  /** M4-04 打印通道：按纸张尺寸出 PDF（无打印机环境的交付路径） */
  printPdf(req: {
    imagePath: string;
    pageSize?: 'A4' | 'A3' | 'Letter';
    landscape?: boolean;
    marginMm?: number;
    dpi?: number;
    outputPath?: string;
  }): Promise<{ path: string; bytes: number; pageSize: string; landscape: boolean; dpi: number; marginMm: number }>;
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
  /** v1.3.0-m3 MCP 桥接：订阅主进程从控制端口转发来的指令（op 由渲染层执行器分发） */
  onMcpCall(cb: (req: { reqId: number; op: string; params?: unknown }) => void): () => void;
  /** v1.3.0-m3 MCP 桥接：执行完成后回传结果（reqId 对应 onMcpCall 的 reqId） */
  resolveMcpCall(result: { reqId: number; ok: boolean; result?: unknown; error?: string }): void;
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
  pickFile: (opts?: { title?: string; extensions?: string[] }) => ipcRenderer.invoke('lumina:pickFile', opts),
  pickDirectory: (opts?: { title?: string; defaultPath?: string }) => ipcRenderer.invoke('lumina:pickDirectory', opts),
  printPdf: (req: { imagePath: string; pageSize?: 'A4' | 'A3' | 'Letter'; landscape?: boolean; marginMm?: number; dpi?: number; outputPath?: string }) => ipcRenderer.invoke('lumina:printPdf', req),
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
  // v1.3.0-m3：MCP 桥接通道（接收 → 执行 → 回传；send 单向回传，主进程 ipcMain.on 承接）
  onMcpCall: (cb: (req: { reqId: number; op: string; params?: unknown }) => void) => {
    const listener = (_e: unknown, req: { reqId: number; op: string; params?: unknown }) => cb(req);
    ipcRenderer.on('lumina:mcp:call', listener);
    return () => ipcRenderer.removeListener('lumina:mcp:call', listener);
  },
  resolveMcpCall: (result: { reqId: number; ok: boolean; result?: unknown; error?: string }) => {
    ipcRenderer.send('lumina:mcp:call-result', result);
  },
} satisfies LuminaPreload);
