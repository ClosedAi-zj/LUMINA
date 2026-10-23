/**
 * 交付中心 store：导出预设、导出规格编辑、导出对象统计、任务队列。
 * 降级纪律（零静默红线）：任一 export.* 方法不可用时记录显式错误状态，
 * 页面渲染降级横幅与禁用按钮，绝不崩溃、绝不静默。
 */

import { create } from 'zustand';
import { api } from '../../../api/engine';
import { errText } from '../../../api/rpc-client';
import { useJobStore } from '../../../store/jobStore';
import type { ExportPreset, ExportSpec, Filter, JobInfo } from '../../../api/protocol';

export type ExportSource = 'all' | 'picked' | 'rated';

export const DEFAULT_SPEC: ExportSpec = {
  format: 'jpg',
  longEdge: 4096,
  colorSpace: 'sRGB',
  quality: 90,
  psdKeepLayers: true,
  // WP-B8：position/scale 改为引擎原生语义（5 锚点 kebab-case；scale = 相对图宽 12% 的倍率）。
  // 旧默认 bottomRight/scale:8 发给引擎会静默落到右下角且放大到 96% 图宽。
  watermark: { enabled: false, position: 'bottom-right', scale: 0.88, opacity: 60, text: 'LUMINA PHOTO' },
  namingTemplate: '{date}_{name}_{seq}',
  outputDir: 'D:/LUMINA/build/runtime/exports',
};

/** 命名模板 → 示例文件名（{date} {seq} {name} 占位符） */
export function previewFileName(spec: ExportSpec, sampleName = 'DSC_0142'): string {
  const extMap: Record<ExportSpec['format'], string> = { jpg: 'jpg', tiff: 'tif', png: 'png', psd: 'psd' };
  const base = spec.namingTemplate
    .replace(/\{date\}/g, '2026-04-25')
    .replace(/\{seq\}/g, '0001')
    .replace(/\{name\}/g, sampleName.replace(/\.[^.]+$/, ''));
  return `${base || '未命名'}.${extMap[spec.format]}`;
}

function sourceFilter(source: ExportSource): Filter | undefined {
  if (source === 'picked') return { pick: 'pick' };
  if (source === 'rated') return { minRating: 3 };
  return undefined;
}

interface ExportStore {
  presets: ExportPreset[];
  presetsLoading: boolean;
  /** 预设接口不可用时的降级提示 */
  presetsError: string | null;
  selectedPresetId: number | null;
  spec: ExportSpec;
  source: ExportSource;
  sourceCount: number | null;
  jobs: JobInfo[];
  /** 队列接口不可用时的降级提示 */
  jobsError: string | null;
  creating: boolean;
  savingPreset: boolean;

  loadPresets: () => Promise<void>;
  applyPreset: (id: number) => void;
  savePreset: (name: string) => Promise<void>;
  patchSpec: (patch: Partial<ExportSpec>) => void;
  patchWatermark: (patch: Partial<ExportSpec['watermark']>) => void;
  setSource: (s: ExportSource) => Promise<void>;
  refreshSourceCount: () => Promise<void>;
  refreshJobs: () => Promise<void>;
  createJob: () => Promise<void>;
  cancelJob: (jobId: number) => Promise<void>;
  retryJob: (jobId: number) => Promise<void>;
}

export const useExport = create<ExportStore>((set, get) => ({
  presets: [],
  presetsLoading: false,
  presetsError: null,
  selectedPresetId: null,
  spec: { ...DEFAULT_SPEC, watermark: { ...DEFAULT_SPEC.watermark } },
  source: 'picked',
  sourceCount: null,
  jobs: [],
  jobsError: null,
  creating: false,
  savingPreset: false,

  loadPresets: async () => {
    set({ presetsLoading: true, presetsError: null });
    try {
      const presets = await api.exportApi.listPresets();
      set({ presets, presetsLoading: false, presetsError: null });
    } catch (err) {
      set({ presetsLoading: false, presetsError: errText(err) });
    }
  },

  applyPreset: (id) => {
    const preset = get().presets.find((p) => p.id === id);
    if (!preset) return;
    set({
      selectedPresetId: id,
      spec: { ...preset.spec, watermark: { ...DEFAULT_SPEC.watermark, ...preset.spec.watermark } },
    });
  },

  savePreset: async (name) => {
    const trimmed = name.trim();
    if (!trimmed) return;
    set({ savingPreset: true });
    try {
      await api.exportApi.savePreset(trimmed, get().spec);
      useJobStore.getState().toast(`预设「${trimmed}」已保存`, 'success');
      set({ savingPreset: false });
      await get().loadPresets();
    } catch (err) {
      set({ savingPreset: false, presetsError: errText(err) });
      useJobStore.getState().toast(`预设保存失败：${errText(err)}`, 'error', 8000);
    }
  },

  patchSpec: (patch) => set((s) => ({ spec: { ...s.spec, ...patch }, selectedPresetId: null })),

  patchWatermark: (patch) => set((s) => ({ spec: { ...s.spec, watermark: { ...s.spec.watermark, ...patch } }, selectedPresetId: null })),

  setSource: async (s) => {
    set({ source: s });
    await get().refreshSourceCount();
  },

  refreshSourceCount: async () => {
    try {
      const res = await api.library.queryPhotos(sourceFilter(get().source), { field: 'capturedAt', order: 'asc' }, { offset: 0, limit: 1 });
      set({ sourceCount: res.total });
    } catch (err) {
      set({ sourceCount: null });
      useJobStore.getState().toast(`导出对象统计失败：${errText(err)}`, 'error', 6000);
    }
  },

  refreshJobs: async () => {
    try {
      const jobs = await api.exportApi.listJobs();
      // 运行中任务用事件泵的实时进度覆盖，进度条平滑
      const live = useJobStore.getState().jobs;
      const merged = jobs.map((j) => {
        const l = live[j.jobId];
        return l && l.state === 'running' && j.state === 'running' ? { ...j, progress: l.progress, detail: l.detail, etaMs: l.etaMs } : j;
      });
      set({ jobs: merged, jobsError: null });
    } catch (err) {
      set({ jobsError: errText(err) });
    }
  },

  createJob: async () => {
    const st = get();
    if (st.creating) return;
    set({ creating: true });
    try {
      const res = await api.library.queryPhotos(sourceFilter(st.source), { field: 'capturedAt', order: 'asc' }, { offset: 0, limit: 1000 });
      if (res.items.length === 0) {
        useJobStore.getState().toast('当前导出对象为空：没有符合条件的照片', 'warn', 6000);
        set({ creating: false });
        return;
      }
      const { jobId } = await api.exportApi.createJob(res.items.map((p) => p.id), st.spec);
      useJobStore.getState().toast(`导出任务已创建（任务 ${jobId}）· ${res.items.length} 张`, 'success');
      await get().refreshJobs();
    } catch (err) {
      // 零静默：创建失败显式提示 + 队列区降级横幅
      set({ jobsError: errText(err) });
      useJobStore.getState().toast(`创建导出任务失败：${errText(err)}`, 'error', 8000);
    } finally {
      set({ creating: false });
    }
  },

  cancelJob: async (jobId) => {
    try {
      await api.exportApi.cancelJob(jobId);
      useJobStore.getState().toast(`任务 ${jobId} 已取消`, 'info');
      await get().refreshJobs();
    } catch (err) {
      useJobStore.getState().toast(`取消失败：${errText(err)}`, 'error', 6000);
    }
  },

  retryJob: async (jobId) => {
    try {
      await api.exportApi.retryJob(jobId);
      useJobStore.getState().toast(`任务 ${jobId} 已重新入队`, 'info');
      await get().refreshJobs();
    } catch (err) {
      useJobStore.getState().toast(`重试失败：${errText(err)}`, 'error', 6000);
    }
  },
}));
