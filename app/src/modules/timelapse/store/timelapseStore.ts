/**
 * 延时工作台 store：序列列表/详情、帧预览、去闪（分析/应用）、
 * Holy Grail 曝光坡度关键帧编辑、渲染参数与渲染队列。
 * 画面区恒 #08090B（组件层用 --Lumina-Tokens-viewer）。
 */

import { create } from 'zustand';
import { api } from '../../../api/engine';
import { rpc, errText } from '../../../api/rpc-client';
import { useJobStore } from '../../../store/jobStore';
import type { JobInfo, RampKeyframe, TimelapseColorAdjust, TimelapseSequenceDetail, TimelapseSequenceSummary } from '../../../api/protocol';

/** 去闪分析完成结果（引擎 result：{brightnessGainPct, flickerIndex, warnings?}） */
export interface FlickerAnalysisResult {
  brightnessGainPct: number | null;
  flickerIndex: number | null;
  warnings: string[];
}

export type Resolution = '1080p' | '4K' | '6K';
export type Fps = 25 | 30 | 60;
export type Codec = 'h264' | 'h265' | 'prores';

interface TimelapseStore {
  sequences: TimelapseSequenceSummary[];
  loading: boolean;
  loadError: string | null;
  selectedSeqId: number | null;
  detail: TimelapseSequenceDetail | null;
  detailLoading: boolean;
  frameNo: number;
  playing: boolean;
  /** frameNo → 预览图路径缓存 */
  framePreviews: Record<string, string>;
  framePreviewsRequested: Set<string>;
  /** 本地可编辑的坡度关键帧（从 detail.exposureRamp 拷贝，应用后回写引擎） */
  ramp: RampKeyframe[];
  rampDirty: boolean;
  resolution: Resolution;
  fps: Fps;
  codec: Codec;
  renderJobs: JobInfo[];
  /** 各异步动作的 jobId（组件用 useJobProgress 订阅完成事件） */
  analyzeJobId: number | null;
  deflickerJobId: number | null;
  renderJobId: number | null;
  /** 去闪分析量化结果（完成后回填，RenderPanel 展示 + warnings 零静默） */
  analyzeResult: FlickerAnalysisResult | null;
  /** 走带区间（仅影响预览播放区间；渲染区间随 Phase B） */
  inPoint: number | null;
  outPoint: number | null;
  loop: boolean;
  /** 调色状态（仅存 UI，引擎接入留 Phase B） */
  toneCurve: { shadow: number; mid: number; high: number };
  colorGrade: { shadows: { hue: number; amount: number }; mid: { hue: number; amount: number }; high: { hue: number; amount: number } };

  setInPoint: (n: number | null) => void;
  setOutPoint: (n: number | null) => void;
  setLoop: (v: boolean) => void;
  addKeyframeAtCurrent: () => void;
  setToneCurve: (patch: Partial<{ shadow: number; mid: number; high: number }>) => void;
  setColorGrade: (zone: 'shadows' | 'mid' | 'high', patch: Partial<{ hue: number; amount: number }>) => void;

  load: () => Promise<void>;
  select: (seqId: number) => Promise<void>;
  refreshDetail: () => Promise<void>;
  setFrameNo: (n: number) => void;
  stepFrame: (delta: number) => void;
  setPlaying: (v: boolean) => void;
  ensureFramePreview: (seqId: number, frameNo: number, size?: 320 | 640 | 1080) => Promise<void>;
  moveKeyframe: (index: number, ev: number, temperatureK?: number) => void;
  commitRamp: () => Promise<void>;
  resetRamp: () => void;
  setResolution: (r: Resolution) => void;
  setFps: (f: Fps) => void;
  setCodec: (c: Codec) => void;
  analyzeFlicker: () => Promise<void>;
  applyDeflicker: (strength: number) => Promise<void>;
  render: () => Promise<void>;
  refreshJobs: () => Promise<void>;
}

export const useTimelapse = create<TimelapseStore>((set, get) => ({
  sequences: [],
  loading: false,
  loadError: null,
  selectedSeqId: null,
  detail: null,
  detailLoading: false,
  frameNo: 1,
  playing: false,
  framePreviews: {},
  framePreviewsRequested: new Set(),
  ramp: [],
  rampDirty: false,
  resolution: '4K',
  fps: 30,
  codec: 'h265',
  renderJobs: [],
  analyzeJobId: null,
  deflickerJobId: null,
  renderJobId: null,
  analyzeResult: null,
  inPoint: null,
  outPoint: null,
  loop: true,
  toneCurve: { shadow: 0, mid: 0, high: 0 },
  colorGrade: {
    shadows: { hue: 0, amount: 0 },
    mid: { hue: 0, amount: 0 },
    high: { hue: 0, amount: 0 },
  },

  load: async () => {
    if (get().loading) return;
    bindTimelapseEvents();
    set({ loading: true, loadError: null });
    try {
      const sequences = normalizeSequenceList(await api.timelapse.listSequences());
      set({ sequences, loading: false, loadError: null });
      const first = sequences[0];
      if (first && get().selectedSeqId === null) await get().select(first.seqId);
    } catch (err) {
      set({ loading: false, loadError: errText(err) });
    }
  },

  select: async (seqId) => {
    if (get().selectedSeqId === seqId && get().detail) return;
    set({ selectedSeqId: seqId, detail: null, detailLoading: true, frameNo: 1, playing: false, rampDirty: false, inPoint: null, outPoint: null });
    await get().refreshDetail();
  },

  refreshDetail: async () => {
    const seqId = get().selectedSeqId;
    if (seqId === null) return;
    try {
      const detail = await api.timelapse.getSequence(seqId);
      // 回查守卫：请求期间用户可能已切到别的序列，旧响应不许覆盖新选中项
      //（旧行为：切 A→B 时 A 的慢响应最后到达，B 的画面下显示 A 的帧/坡度）
      if (get().selectedSeqId !== seqId) return;
      set((s) => ({
        detail,
        detailLoading: false,
        ramp: detail.exposureRamp.keyframes.map((k) => ({ ...k })),
        rampDirty: false,
        frameNo: Math.min(Math.max(1, s.frameNo), Math.max(1, detail.frameCount)),
      }));
    } catch (err) {
      if (get().selectedSeqId !== seqId) return;
      set({ detailLoading: false });
      useJobStore.getState().toast(`序列详情加载失败：${errText(err)}`, 'error', 8000);
    }
  },

  setFrameNo: (n) => {
    const total = get().detail?.frameCount ?? 1;
    set({ frameNo: Math.max(1, Math.min(total, Math.round(n))) });
  },

  stepFrame: (delta) => {
    const st = get();
    const total = st.detail?.frameCount ?? 1;
    // 预览播放区间：in/out 任一设置则以此为界，否则全程 [1,total]
    const lo = st.inPoint !== null && st.outPoint !== null ? Math.min(st.inPoint, st.outPoint) : st.inPoint !== null ? st.inPoint : 1;
    const hi = st.inPoint !== null && st.outPoint !== null ? Math.max(st.inPoint, st.outPoint) : st.outPoint !== null ? st.outPoint : total;
    let next = st.frameNo + delta;
    if (st.loop) {
      // 区间内循环回绕
      if (next > hi) next = lo;
      else if (next < lo) next = hi;
    } else {
      // 非循环：到边界停住并暂停播放
      if (next > hi) {
        next = hi;
        set({ playing: false });
      } else if (next < lo) {
        next = lo;
        set({ playing: false });
      }
    }
    set({ frameNo: Math.max(lo, Math.min(hi, next)) });
  },

  setPlaying: (v) => set({ playing: v }),

  ensureFramePreview: async (seqId, frameNo, size = 1080) => {
    const key = `${seqId}_${frameNo}_${size}`;
    const st = get();
    if (st.framePreviews[key] || st.framePreviewsRequested.has(key)) return;
    set((s) => {
      const next = new Set(s.framePreviewsRequested);
      next.add(key);
      return { framePreviewsRequested: next };
    });
    try {
      const res = await api.timelapse.previewFrame(seqId, frameNo, size);
      set((s) => ({ framePreviews: { ...s.framePreviews, [key]: res.path } }));
    } catch (err) {
      useJobStore.getState().toast(`帧预览加载失败：${errText(err)}`, 'error', 6000);
    }
  },

  moveKeyframe: (index, ev, temperatureK) => {
    set((s) => {
      const ramp = s.ramp.map((k, i) =>
        i === index ? { ...k, ev: Math.max(-5, Math.min(5, ev)), temperatureK: temperatureK ?? k.temperatureK } : k,
      );
      return { ramp, rampDirty: true };
    });
  },

  commitRamp: async () => {
    const st = get();
    if (st.selectedSeqId === null) return;
    try {
      await api.timelapse.setExposureRamp(st.selectedSeqId, st.ramp);
      useJobStore.getState().toast(`曝光坡度已应用（${st.ramp.length} 个关键帧）`, 'success');
      set({ rampDirty: false });
    } catch (err) {
      useJobStore.getState().toast(`坡度应用失败：${errText(err)}`, 'error', 8000);
    }
  },

  resetRamp: () => {
    const detail = get().detail;
    if (detail) set({ ramp: detail.exposureRamp.keyframes.map((k) => ({ ...k })), rampDirty: false });
  },

  setInPoint: (n) => set({ inPoint: n }),
  setOutPoint: (n) => set({ outPoint: n }),
  setLoop: (v) => set({ loop: v }),

  addKeyframeAtCurrent: () => {
    const st = get();
    if (st.selectedSeqId === null) return;
    const frameNo = st.frameNo;
    // 同帧已有关键帧则跳过（避免重复），保留既有 EV / 色温
    if (st.ramp.some((k) => k.frameNo === frameNo)) return;
    const { ev, temperatureK } = interpolateRamp(st.ramp, frameNo);
    const ramp = [...st.ramp, { frameNo, ev, temperatureK }].sort((a, b) => a.frameNo - b.frameNo);
    set({ ramp, rampDirty: true });
  },

  setToneCurve: (patch) => set((s) => ({ toneCurve: { ...s.toneCurve, ...patch } })),
  setColorGrade: (zone, patch) =>
    set((s) => ({ colorGrade: { ...s.colorGrade, [zone]: { ...s.colorGrade[zone], ...patch } } })),

  setResolution: (r) => set({ resolution: r }),
  setFps: (f) => set({ fps: f }),
  setCodec: (c) => set({ codec: c }),

  analyzeFlicker: async () => {
    const st = get();
    if (st.selectedSeqId === null) return;
    try {
      const { jobId } = await api.timelapse.analyzeFlicker(st.selectedSeqId);
      set({ analyzeJobId: jobId, analyzeResult: null });
      useJobStore.getState().toast(`去闪分析已启动（任务 ${jobId}）…`, 'info');
    } catch (err) {
      useJobStore.getState().toast(`去闪分析启动失败：${errText(err)}`, 'error', 8000);
    }
  },

  applyDeflicker: async (strength) => {
    const st = get();
    if (st.selectedSeqId === null) return;
    try {
      const { jobId } = await api.timelapse.applyDeflicker(st.selectedSeqId, strength);
      set({ deflickerJobId: jobId });
      useJobStore.getState().toast(`去闪已应用（强度 ${strength}%，任务 ${jobId}）…`, 'info');
    } catch (err) {
      useJobStore.getState().toast(`去闪应用失败：${errText(err)}`, 'error', 8000);
    }
  },

  render: async () => {
    const st = get();
    if (st.selectedSeqId === null) return;
    try {
      // 透传调色参数：store 内部 UI 形状（shadow/mid/high、hue/amount）映射为引擎 colorAdjust 契约
      // （引擎对零值≈恒等，故恒传，无需前端判空）
      const colorAdjust: TimelapseColorAdjust = {
        toneCurve: { shadows: st.toneCurve.shadow, midtone: st.toneCurve.mid, highlight: st.toneCurve.high },
        colorGrade: {
          shadowHue: st.colorGrade.shadows.hue,
          shadowSat: st.colorGrade.shadows.amount,
          midtoneHue: st.colorGrade.mid.hue,
          midtoneSat: st.colorGrade.mid.amount,
          highlightHue: st.colorGrade.high.hue,
          highlightSat: st.colorGrade.high.amount,
        },
      };
      const { jobId } = await api.timelapse.render(st.selectedSeqId, st.resolution, st.fps, st.codec, colorAdjust);
      set({ renderJobId: jobId });
      useJobStore.getState().toast(`渲染任务已创建（任务 ${jobId}）`, 'success');
      await get().refreshJobs();
    } catch (err) {
      // 显式降级（如 ProRes 编码器不可达），绝不静默
      useJobStore.getState().toast(`渲染启动失败：${errText(err)}`, 'error', 10000);
    }
  },

  refreshJobs: async () => {
    try {
      const jobs = await api.timelapse.getRenderJobs();
      // 用事件泵的实时进度覆盖运行中行
      const live = useJobStore.getState().jobs;
      const merged = jobs.map((j) => {
        const l = live[j.jobId];
        return l && l.state === 'running' && j.state === 'running' ? { ...j, progress: l.progress, detail: l.detail, etaMs: l.etaMs } : j;
      });
      set({ renderJobs: merged });
    } catch (err) {
      useJobStore.getState().toast(`渲染队列刷新失败：${errText(err)}`, 'error', 6000);
    }
  },
}));

/**
 * 在当前帧处推断坡度关键帧的 EV / 色温：取相邻关键帧线性插值；
 * 无关键帧则默认 EV 0 / 5500K（addKeyframeAtCurrent 使用）。
 */
function interpolateRamp(ramp: RampKeyframe[], frameNo: number): { ev: number; temperatureK: number } {
  if (ramp.length === 0) return { ev: 0, temperatureK: 5500 };
  const sorted = [...ramp].sort((a, b) => a.frameNo - b.frameNo);
  let lo: RampKeyframe | null = null;
  let hi: RampKeyframe | null = null;
  for (const k of sorted) {
    if (k.frameNo <= frameNo) lo = k;
    if (k.frameNo >= frameNo && hi === null) hi = k;
  }
  if (lo && hi) {
    if (lo.frameNo === hi.frameNo) return { ev: lo.ev, temperatureK: lo.temperatureK };
    const t = (frameNo - lo.frameNo) / Math.max(1, hi.frameNo - lo.frameNo);
    return {
      ev: lo.ev + (hi.ev - lo.ev) * t,
      temperatureK: Math.round(lo.temperatureK + (hi.temperatureK - lo.temperatureK) * t),
    };
  }
  if (lo) return { ev: lo.ev, temperatureK: lo.temperatureK };
  if (hi) return { ev: hi.ev, temperatureK: hi.temperatureK };
  return { ev: 0, temperatureK: 5500 };
}

/**
 * 引擎 listSequences 返回形状兼容：数组或 { items: [...] } 二者皆收
 * （引擎实现迭代期间出现过分页包装形状，UI 侧在边界归一化）。
 */
function normalizeSequenceList(raw: unknown): TimelapseSequenceSummary[] {
  if (Array.isArray(raw)) return raw as TimelapseSequenceSummary[];
  const items = (raw as { items?: unknown } | null)?.items;
  if (Array.isArray(items)) return items as TimelapseSequenceSummary[];
  return [];
}

/** 去闪分析/应用完成事件绑定（load 时绑定一次）：量化结果 + warnings 零静默展示 */
let timelapseEventsBound = false;
function bindTimelapseEvents(): void {
  if (timelapseEventsBound) return;
  timelapseEventsBound = true;
  rpc.on('event.job.completed', (params) => {
    const e = params as { jobId: number; kind: string; state: string; result?: Record<string, unknown> };
    const st = useTimelapse.getState();
    if (e.state !== 'done' || (e.jobId !== st.analyzeJobId && e.jobId !== st.deflickerJobId)) return;
    const result = e.result ?? {};
    const warnings = Array.isArray(result.warnings) ? (result.warnings as string[]) : [];
    useTimelapse.setState({
      analyzeResult: {
        brightnessGainPct: typeof result.brightnessGainPct === 'number' ? result.brightnessGainPct : null,
        flickerIndex: typeof result.flickerIndex === 'number' ? result.flickerIndex : null,
        warnings,
      },
    });
    // 降级/异常说明必须可见（如「走 embedded-preview」这类提示，零静默红线）
    for (const w of warnings) {
      useJobStore.getState().toast(`去闪分析提示 · ${w}`, 'warn', 10000);
    }
  });
}
