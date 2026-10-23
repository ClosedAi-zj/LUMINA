/**
 * 全局任务 / 通知 / 引擎状态 store。
 * job 事件由 src/store/events.ts 统一路由进本 store（避免 store 间循环依赖）。
 */

import { create } from 'zustand';
import type { EngineLogEvent, ImportFileResultEvent, JobCompletedEvent, JobProgressEvent } from '../api/protocol';

export interface ToastItem {
  id: number;
  kind: 'info' | 'success' | 'error' | 'warn';
  text: string;
  undoFn?: () => Promise<unknown> | void;
  createdAt: number;
}

export interface JobState {
  jobId: number;
  kind: string;
  state: 'running' | 'done' | 'failed' | 'cancelled';
  progress: number;
  detail: string;
  etaMs: number | null;
  result?: Record<string, unknown>;
  error?: { code: number; message: string };
}

export type EngineStatus = 'starting' | 'engine' | 'mock';

interface JobStore {
  engineStatus: EngineStatus;
  engineVersion: string;
  engineInfo: Record<string, unknown>;
  jobs: Record<number, JobState>;
  toasts: ToastItem[];
  importFileResults: ImportFileResultEvent[];
  /** 未处理的健康异常数（红点） */
  healthIssueCount: number;
  /** 引擎崩溃恢复提示 */
  recoveredAt: number | null;
  setEngineStatus: (s: EngineStatus, version?: string, info?: Record<string, unknown>) => void;
  onJobProgress: (e: JobProgressEvent) => void;
  onJobCompleted: (e: JobCompletedEvent) => void;
  onImportFileResult: (e: ImportFileResultEvent) => void;
  onEngineLog: (e: EngineLogEvent) => void;
  bumpHealthIssues: () => void;
  clearHealthIssues: () => void;
  notifyRecovered: () => void;
  toast: (text: string, kind?: ToastItem['kind'], ttlMs?: number) => void;
  addUndoToast: (text: string, undoFn: () => Promise<unknown> | void) => void;
  dismissToast: (id: number) => void;
}

let toastSeq = 1;

export const useJobStore = create<JobStore>((set, get) => ({
  engineStatus: 'starting',
  engineVersion: '',
  engineInfo: {},
  jobs: {},
  toasts: [],
  importFileResults: [],
  healthIssueCount: 0,
  recoveredAt: null,

  setEngineStatus: (s, version, info) => set((st) => ({ engineStatus: s, engineVersion: version ?? st.engineVersion, engineInfo: info ?? st.engineInfo })),

  onJobProgress: (e) =>
    set((st) => ({
      jobs: {
        ...st.jobs,
        [e.jobId]: {
          jobId: e.jobId,
          kind: e.kind,
          state: 'running',
          progress: e.progress,
          detail: e.detail,
          etaMs: e.etaMs ?? null,
        },
      },
    })),

  onJobCompleted: (e) => {
    set((st) => {
      const prev = st.jobs[e.jobId];
      return {
        jobs: {
          ...st.jobs,
          [e.jobId]: {
            jobId: e.jobId,
            kind: e.kind,
            state: e.state,
            progress: e.state === 'done' ? 100 : prev?.progress ?? 0,
            detail: prev?.detail ?? '',
            etaMs: null,
            result: e.result,
            error: e.error ? { code: e.error.code, message: e.error.message } : undefined,
          },
        },
      };
    });
    // 错误纪律（红线 R3）：job 失败必须显式告知，禁止吞错
    if (e.state === 'failed' && e.error) {
      get().toast(`任务失败（${e.error.code}）：${e.error.message}`, 'error', 8000);
    }
  },

  onImportFileResult: (e) =>
    set((st) => {
      const next = [...st.importFileResults, e];
      return { importFileResults: next.length > 400 ? next.slice(next.length - 400) : next };
    }),

  onEngineLog: (e) => {
    // xmp 防抖落盘日志由 developStore 自身计时呈现，此处只处理错误级
    if (e.level === 'error') {
      get().toast(`[${e.module}] ${e.message}`, 'error', 8000);
    }
  },

  bumpHealthIssues: () => set((st) => ({ healthIssueCount: st.healthIssueCount + 1 })),
  clearHealthIssues: () => set({ healthIssueCount: 0 }),
  notifyRecovered: () => {
    set({ recoveredAt: Date.now() });
    get().toast('引擎已恢复 · 未落盘参数已从 sidecar 重读', 'warn', 6000);
  },

  toast: (text, kind = 'info', ttlMs = 5000) => {
    const id = toastSeq++;
    set((st) => ({ toasts: [...st.toasts, { id, kind, text, createdAt: Date.now() }] }));
    setTimeout(() => {
      set((st) => ({ toasts: st.toasts.filter((t) => t.id !== id) }));
    }, ttlMs);
  },

  addUndoToast: (text, undoFn) => {
    const id = toastSeq++;
    set((st) => ({ toasts: [...st.toasts, { id, kind: 'info', text, undoFn, createdAt: Date.now() }] }));
    // 5s 撤销窗口（PRD §6.3：破坏性操作 5s 可撤销）
    setTimeout(() => {
      set((st) => ({ toasts: st.toasts.filter((t) => t.id !== id) }));
    }, 5000);
  },

  dismissToast: (id) => set((st) => ({ toasts: st.toasts.filter((t) => t.id !== id) })),
}));
