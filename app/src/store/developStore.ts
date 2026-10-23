/**
 * 修图会话 store：快速/专业共用同一参数模型（PRD §6.1 —— 禁止两套参数状态）。
 * 滑块拖动 → 本地乐观更新（WebGL 即时渲染）+ edit.setParams(commitHistory=false)；
 * 松手 → commitHistory=true（引擎侧 500ms 防抖落盘 sidecar）。
 */

import { create } from 'zustand';
import { api } from '../api/engine';
import { errText } from '../api/rpc-client';
import type { AdjustmentLayer, EditParams, HslBand, MaskLayer } from '../api/protocol';
import type { Hsl } from '../api/protocol';

export type DevelopMode = 'fast' | 'pro';
export type XmpStatus = 'idle' | 'debouncing' | 'written';
export type GuidesMode = 'off' | 'thirds' | 'center' | 'grid';

/** 历史快照条目（UI 侧参数快照，供专业模式点击跳转回放） */
export interface HistoryEntry {
  label: string;
  params: EditParams;
  time: number;
}

/** 调整图层混合模式（协议 blendMode 为开放枚举，此处为 UI 白名单） */
export const BLEND_MODES: { value: string; label: string }[] = [
  { value: 'normal', label: '正常' },
  { value: 'multiply', label: '变暗 · 正片叠底' },
  { value: 'screen', label: '变亮 · 滤色' },
  { value: 'overlay', label: '叠加' },
  { value: 'softLight', label: '柔光' },
  { value: 'hardLight', label: '强光' },
  { value: 'lighten', label: '变亮' },
  { value: 'darken', label: '变暗' },
  { value: 'luminosity', label: '明度' },
  { value: 'color', label: '颜色' },
];

export function blendLabel(v: string): string {
  return BLEND_MODES.find((b) => b.value === v)?.label ?? v;
}

interface DevelopStore {
  photoId: number | null;
  sessionId: number | null;
  params: EditParams | null;
  revision: number;
  baseImageUrl: string | null;
  baseImageReady: boolean;
  beforeParams: EditParams | null;
  mode: DevelopMode;
  xmpStatus: XmpStatus;
  historyLabels: string[];
  canUndo: boolean;
  canRedo: boolean;
  selectedMaskId: string | null;
  maskTool: 'linear' | 'radial' | 'brush' | null;
  syncMessage: string | null;
  loading: boolean;
  xmpTimer: ReturnType<typeof setTimeout> | null;
  /** 协议 v1.0.1：最后一次 endSession{flush:true} 返回的 sidecarMtime（null=未确认落盘，禁示"已写入"） */
  lastFlushMtime: number | null;

  // ---- 专业模式（与快速模式共享同一 params，以下为 UI 侧状态 + params 扩展读写） ----
  /** 历史快照（与 historyLabels 一一对应，commit 时记录） */
  historySnapshots: HistoryEntry[];
  /** 当前选中调整图层 */
  selectedLayerId: string | null;
  /** 锁定的图层（UI 本地状态，协议无锁定字段，不落盘） */
  lockedLayerIds: string[];
  /** 参考线模式（UI 叠加层，不落盘） */
  guides: GuidesMode;

  openPhoto: (id: number) => Promise<void>;
  closePhoto: () => Promise<void>;
  setMode: (m: DevelopMode) => void;
  applyPatch: (patch: Partial<EditParams>, commit: boolean, label?: string) => Promise<void>;
  setNumeric: (key: keyof EditParams, value: number, commit: boolean) => Promise<void>;
  setHsl: (band: HslBand, key: keyof Hsl, value: number, commit: boolean) => Promise<void>;
  undo: () => Promise<void>;
  redo: () => Promise<void>;
  reset: () => Promise<void>;
  addMask: (type: MaskLayer['type']) => Promise<void>;
  updateMask: (maskId: string, patch: Partial<MaskLayer>) => Promise<void>;
  updateMaskLocal: (maskId: string, key: keyof MaskLayer['local'], value: number, commit: boolean) => Promise<void>;
  removeMask: (maskId: string) => Promise<void>;
  appendStroke: (maskId: string, stroke: NonNullable<MaskLayer['strokes']>[number], commit: boolean) => Promise<void>;
  selectMask: (id: string | null) => void;
  setMaskTool: (t: 'linear' | 'radial' | 'brush' | null) => void;
  applyPreset: (params: EditParams, name: string) => Promise<void>;
  saveSnapshot: (name: string) => Promise<void>;
  syncToSelection: (targetIds: number[]) => Promise<void>;
  syncToSequence: (seqId: number) => Promise<void>;
  toggleBeforeAfter: () => void;

  // ---- 专业模式 actions ----
  /** 点路径设值：'detail.sharpness' / 'geometry.perspective.rotate' / 'exposure' */
  setPath: (path: string, value: number | boolean | string, commit: boolean, label?: string) => Promise<void>;
  /** 历史跳转（参数快照回放） */
  jumpHistory: (index: number) => Promise<void>;
  addLayer: (kind: string, name?: string) => Promise<void>;
  updateLayer: (id: string, patch: Partial<AdjustmentLayer>) => Promise<void>;
  /** 图层内参数滑块（图层.params 顶层键） */
  updateLayerParam: (id: string, key: keyof EditParams, value: number, commit: boolean) => Promise<void>;
  removeLayer: (id: string) => Promise<void>;
  moveLayer: (id: string, dir: -1 | 1) => Promise<void>;
  renameLayer: (id: string, name: string) => Promise<void>;
  selectLayer: (id: string | null) => void;
  /** 锁定为 UI 本地状态（协议 AdjustmentLayer 无 locked 字段） */
  toggleLayerLock: (id: string) => void;
  setGuides: (g: GuidesMode) => void;
}

export const useDevelop = create<DevelopStore>((set, get) => ({
  photoId: null,
  sessionId: null,
  params: null,
  revision: 0,
  baseImageUrl: null,
  baseImageReady: false,
  beforeParams: null,
  mode: 'fast',
  xmpStatus: 'idle',
  historyLabels: [],
  canUndo: false,
  canRedo: false,
  selectedMaskId: null,
  maskTool: null,
  syncMessage: null,
  loading: false,
  xmpTimer: null,
  lastFlushMtime: null,
  historySnapshots: [],
  selectedLayerId: null,
  lockedLayerIds: [],
  guides: 'off',

  openPhoto: async (id) => {
    // 切图/关图必须先 endSession{flush:true}（协议 §5.5 / v1.0.1）
    const prev = get();
    if (prev.sessionId !== null) {
      try {
        // 必须 await 返回值：凭 sidecarMtime 确认上一张参数已落盘，不自行判断是否写入
        const end = await api.edit.endSession(prev.sessionId, true);
        if (end.flushed && end.sidecarMtime !== null) set({ xmpStatus: 'written', lastFlushMtime: end.sidecarMtime });
      } catch {
        // 旧会话清理失败不阻塞新会话，但必须可见（上一张的参数可能未落盘）
        const { useJobStore } = await import('./jobStore');
        useJobStore.getState().toast('切换照片：上一张的会话清理失败，其参数可能未写入 XMP', 'warn', 6000);
      }
    }
    set({ loading: true, photoId: id, baseImageReady: false, baseImageUrl: null, selectedMaskId: null, historyLabels: [], canUndo: false, canRedo: false, xmpStatus: 'idle', historySnapshots: [], selectedLayerId: null });
    try {
      const [session, base] = await Promise.all([api.edit.beginSession(id), api.preview.getBase(id)]);
      const normalized = normalizeParams(session.params);
      set({
        sessionId: session.sessionId,
        params: normalized,
        revision: session.revision,
        beforeParams: structuredClone(normalized),
        baseImageUrl: base.path,
        baseImageReady: true,
        loading: false,
        historyLabels: ['导入原状'],
        historySnapshots: [{ label: '导入原状', params: structuredClone(normalized), time: Date.now() }],
      });
    } catch (err) {
      set({ loading: false });
      const { useJobStore } = await import('./jobStore');
      useJobStore.getState().toast(`无法打开照片：${errText(err)}`, 'error', 8000);
    }
  },

  closePhoto: async () => {
    const { sessionId } = get();
    if (sessionId !== null) {
      try {
        // 协议 v1.0.1：退出修图台必须 await endSession{flush:true} 的返回值，
        // 用返回的 sidecarMtime 熄灭「参数已写入 XMP」指示灯；不得自行判断是否已写入。
        const end = await api.edit.endSession(sessionId, true);
        if (end.flushed && end.sidecarMtime !== null) {
          set({ xmpStatus: 'written', lastFlushMtime: end.sidecarMtime });
        } else {
          set({ lastFlushMtime: null });
        }
      } catch (err) {
        // 零静默红线：落盘失败必须可见（否则用户以为参数已保存）
        const { useJobStore } = await import('./jobStore');
        useJobStore.getState().toast(`参数落盘失败（XMP 未写入）：${errText(err)}`, 'error', 8000);
      }
    }
    if (get().xmpTimer) clearTimeout(get().xmpTimer as ReturnType<typeof setTimeout>);
    set({ photoId: null, sessionId: null, params: null, baseImageUrl: null, xmpStatus: 'idle' });
  },

  setMode: (m) => {
    // 快速↔专业切换不调引擎（同一 params，仅控件呈现），不重新解码
    set({ mode: m });
  },

  applyPatch: async (patch, commit, label) => {
    const { sessionId, params } = get();
    if (!sessionId || !params) return;
    // 本地乐观合并（与引擎 deepMerge 语义一致：对象递归、数组替换）
    const local = deepMergeLocal(params, patch);
    set({ params: local, revision: get().revision + 1 });
    if (label) {
      // 历史标签与参数快照同步记录（专业模式可点击跳转回放）
      const entry: HistoryEntry = { label, params: structuredClone(local), time: Date.now() };
      set((st) => ({
        historyLabels: [...st.historyLabels.slice(0, 200), label],
        historySnapshots: [...st.historySnapshots.slice(0, 200), entry],
        canUndo: true,
        canRedo: false,
      }));
    }
    if (commit) {
      set({ xmpStatus: 'debouncing' });
      if (get().xmpTimer) clearTimeout(get().xmpTimer as ReturnType<typeof setTimeout>);
      const timer = setTimeout(() => set({ xmpStatus: 'written' }), 600);
      set({ xmpTimer: timer });
    }
    try {
      await api.edit.setParams(sessionId, patch, commit);
    } catch (err) {
      const { useJobStore } = await import('./jobStore');
      useJobStore.getState().toast(`参数写入失败：${errText(err)}`, 'error', 8000);
    }
  },

  setPath: async (path, value, commit, label) => {
    // 点路径 → 嵌套 patch（setPath('detail.sharpness', 40) → { detail: { sharpness: 40 } }）
    const keys = path.split('.');
    let patch: Record<string, unknown> = { [keys[keys.length - 1]]: value };
    for (let i = keys.length - 2; i >= 0; i--) {
      patch = { [keys[i]]: patch };
    }
    await get().applyPatch(patch as Partial<EditParams>, commit, commit ? label ?? `${sliderLabel(keys[keys.length - 1] as keyof EditParams)} ${typeof value === 'number' ? formatEv(value) : String(value)}` : undefined);
  },

  jumpHistory: async (index) => {
    const snap = get().historySnapshots[index];
    if (!snap) return;
    // deepMerge 无删除语义：数组字段必须显式携带（快照里不存在的数组置空），否则后加的图层/蒙版不会被回退
    const patch: Partial<EditParams> = {
      ...snap.params,
      adjustmentStack: snap.params.adjustmentStack ?? [],
      masks: snap.params.masks ?? [],
    };
    await get().applyPatch(patch, true, `回放 · ${snap.label}`);
    useJobStoreToast(`已回放到「${snap.label}」`);
  },

  setNumeric: async (key, value, commit) => {
    await get().applyPatch({ [key]: value } as Partial<EditParams>, commit, commit ? `${sliderLabel(key)} ${formatEv(value)}` : undefined);
  },

  setHsl: async (band, key, value, commit) => {
    const { params } = get();
    if (!params) return;
    const next = { ...params.hsl, [band]: { ...params.hsl[band], [key]: value } };
    await get().applyPatch({ hsl: next }, commit, commit ? `混色器 · ${band} ${key} ${value}` : undefined);
  },

  undo: async () => {
    const { sessionId } = get();
    if (!sessionId) return;
    try {
      const res = await api.edit.undo(sessionId);
      const back = normalizeParams(res.params);
      set((st) => ({
        params: back,
        revision: res.revision,
        historyLabels: st.historyLabels.slice(0, -1),
        historySnapshots: st.historySnapshots.slice(0, -1),
        canUndo: st.historyLabels.length > 2,
        canRedo: true,
        xmpStatus: 'debouncing',
      }));
      const timer = setTimeout(() => set({ xmpStatus: 'written' }), 600);
      set({ xmpTimer: timer });
    } catch (err) {
      const e = err as Error & { code?: number };
      if (e.code !== 3004) {
        const { useJobStore } = await import('./jobStore');
        useJobStore.getState().toast(`撤销失败：${errText(err)}`, 'error', 6000);
      }
    }
  },

  redo: async () => {
    const { sessionId } = get();
    if (!sessionId) return;
    try {
      const res = await api.edit.redo(sessionId);
      const forward = normalizeParams(res.params);
      set((st) => ({
        params: forward,
        revision: res.revision,
        historyLabels: [...st.historyLabels, '重做'],
        historySnapshots: [...st.historySnapshots, { label: '重做', params: structuredClone(forward), time: Date.now() }],
        canRedo: false,
        canUndo: true,
      }));
    } catch (err) {
      const e = err as Error & { code?: number };
      if (e.code !== 3004) {
        const { useJobStore } = await import('./jobStore');
        useJobStore.getState().toast(`重做失败：${errText(err)}`, 'error', 6000);
      }
    }
  },

  reset: async () => {
    const { params } = get();
    if (!params) return;
    const zeroed = zeroParams(params);
    await get().applyPatch(zeroed, true, '重置');
  },

  addMask: async (type) => {
    const { params } = get();
    if (!params) return;
    const mask = buildMask(type, params.masks.length);
    await get().applyPatch({ masks: [...params.masks, mask] }, true, `新建蒙版 · ${maskName(type)}`);
    set({ selectedMaskId: mask.id });
  },

  updateMask: async (maskId, patch) => {
    const { params } = get();
    if (!params) return;
    const masks = params.masks.map((m) => (m.id === maskId ? { ...m, ...patch } : m));
    await get().applyPatch({ masks }, true);
  },

  updateMaskLocal: async (maskId, key, value, commit) => {
    const { params } = get();
    if (!params) return;
    const masks = params.masks.map((m) => (m.id === maskId ? { ...m, local: { ...m.local, [key]: value } } : m));
    await get().applyPatch({ masks }, commit, commit ? `蒙版 · ${maskLocalLabel(key)} ${formatEv(value)}` : undefined);
  },

  removeMask: async (maskId) => {
    const { params } = get();
    if (!params) return;
    const masks = params.masks.filter((m) => m.id !== maskId);
    await get().applyPatch({ masks }, true, '删除蒙版');
    set({ selectedMaskId: null });
  },

  /** 画笔落点（WP-B5）：拖动中 commit=false 不刷历史；抬笔时由调用方以 commit=true 提交一次 */
  appendStroke: async (maskId, stroke, commit) => {
    const { params } = get();
    if (!params) return;
    const masks = params.masks.map((m) =>
      m.id === maskId ? { ...m, strokes: [...(m.strokes ?? []), stroke] } : m,
    );
    await get().applyPatch({ masks }, commit, commit ? '画笔' : undefined);
  },

  selectMask: (id) => set({ selectedMaskId: id }),
  setMaskTool: (t) => set({ maskTool: t }),

  // ---- 专业模式：调整图层栈（params.adjustmentStack，协议 §4 AdjustmentLayer[]） ----

  addLayer: async (kind, name) => {
    const { params } = get();
    if (!params) return;
    const stack = params.adjustmentStack ?? [];
    const layer: AdjustmentLayer = {
      id: `layer_${Date.now().toString(36)}_${stack.length}`,
      name: name ?? `调整图层 ${stack.length + 1}`,
      kind,
      enabled: true,
      blendMode: 'normal',
      opacity: 100,
      params: {},
    };
    await get().applyPatch({ adjustmentStack: [...stack, layer] }, true, `新建图层 · ${layer.name}`);
    set({ selectedLayerId: layer.id });
  },

  updateLayer: async (id, patch) => {
    const { params } = get();
    if (!params) return;
    const stack = (params.adjustmentStack ?? []).map((l) => (l.id === id ? { ...l, ...patch } : l));
    await get().applyPatch({ adjustmentStack: stack }, true);
  },

  updateLayerParam: async (id, key, value, commit) => {
    const { params } = get();
    if (!params) return;
    const stack = (params.adjustmentStack ?? []).map((l) => (l.id === id ? { ...l, params: { ...l.params, [key]: value } } : l));
    await get().applyPatch({ adjustmentStack: stack }, commit, commit ? `图层参数 · ${sliderLabel(key)} ${formatEv(value)}` : undefined);
  },

  removeLayer: async (id) => {
    const { params, selectedLayerId } = get();
    if (!params) return;
    const stack = (params.adjustmentStack ?? []).filter((l) => l.id !== id);
    await get().applyPatch({ adjustmentStack: stack }, true, '删除图层');
    if (selectedLayerId === id) set({ selectedLayerId: null });
  },

  moveLayer: async (id, dir) => {
    const { params } = get();
    if (!params) return;
    const stack = [...(params.adjustmentStack ?? [])];
    const i = stack.findIndex((l) => l.id === id);
    const j = i + dir;
    if (i < 0 || j < 0 || j >= stack.length) return;
    [stack[i], stack[j]] = [stack[j], stack[i]];
    await get().applyPatch({ adjustmentStack: stack }, true, dir > 0 ? '图层上移' : '图层下移');
  },

  renameLayer: async (id, name) => {
    const trimmed = name.trim();
    if (!trimmed) return;
    await get().updateLayer(id, { name: trimmed });
  },

  selectLayer: (id) => set({ selectedLayerId: id }),

  toggleLayerLock: (id) =>
    set((s) => ({
      lockedLayerIds: s.lockedLayerIds.includes(id) ? s.lockedLayerIds.filter((x) => x !== id) : [...s.lockedLayerIds, id],
    })),

  setGuides: (g) => set({ guides: g }),

  applyPreset: async (presetParams, name) => {
    const { params } = get();
    if (!params) return;
    // 预设 = 参数集合（协议 §5.5），套用但保留几何与蒙版结构
    const merged: Partial<EditParams> = {
      processVersion: presetParams.processVersion,
      whiteBalance: presetParams.whiteBalance,
      temperature: presetParams.temperature,
      tint: presetParams.tint,
      exposure: presetParams.exposure,
      contrast: presetParams.contrast,
      highlights: presetParams.highlights,
      shadows: presetParams.shadows,
      whites: presetParams.whites,
      blacks: presetParams.blacks,
      clarity: presetParams.clarity,
      texture: presetParams.texture,
      dehaze: presetParams.dehaze,
      vibrance: presetParams.vibrance,
      saturation: presetParams.saturation,
      toneCurveRGB: presetParams.toneCurveRGB,
      hsl: presetParams.hsl,
      colorGrade: presetParams.colorGrade,
    };
    await get().applyPatch(merged, true, `预设 · ${name}`);
  },

  saveSnapshot: async (name) => {
    const { photoId } = get();
    if (photoId === null) return;
    try {
      await api.edit.createSnapshot(photoId, name);
      const { useJobStore } = await import('./jobStore');
      useJobStore.getState().toast(`快照「${name}」已保存`, 'success');
    } catch (err) {
      const { useJobStore } = await import('./jobStore');
      useJobStore.getState().toast(`快照保存失败：${errText(err)}`, 'error');
    }
  },

  syncToSelection: async (targetIds) => {
    const { photoId } = get();
    if (photoId === null || targetIds.length === 0) return;
    try {
      const res = await api.edit.syncToSelection(photoId, targetIds, true);
      set({ syncMessage: `${res.updated} 张已同步参数` });
      const { useJobStore } = await import('./jobStore');
      useJobStore.getState().toast(`已同步参数到所选 ${res.updated} 张`, 'success');
    } catch (err) {
      const { useJobStore } = await import('./jobStore');
      useJobStore.getState().toast(`同步失败：${errText(err)}`, 'error');
    }
  },

  syncToSequence: async (seqId) => {
    const { photoId } = get();
    if (photoId === null) return;
    try {
      const res = await api.edit.syncToSequence(photoId, seqId, true);
      set({ syncMessage: `${res.updated} 张已同步参数（序列）` });
      const { useJobStore } = await import('./jobStore');
      useJobStore.getState().toast(`已同步参数到序列 ${seqId}（${res.updated} 张）`, 'success');
    } catch (err) {
      const { useJobStore } = await import('./jobStore');
      useJobStore.getState().toast(`同步失败：${errText(err)}`, 'error');
    }
  },

  toggleBeforeAfter: () => set((st) => ({ beforeParams: st.beforeParams ? null : st.beforeParams })),
}));

// ---------- 帮助 ----------

/**
 * 完整 EditParams 缺省形状：真引擎 beginSession 只回传子集（实测 15 个键，
 * 缺 hsl/colorGrade/calibration/detail/optics/geometry/toneCurve 等），
 * UI 归一化补齐后再进 store，专业模式 7 标签面板才能直接读写；
 * 引擎侧 edit.setParams 接受任意参数键，首次设置即随 sidecar 持久化。
 */
const DEFAULT_PARAMS_SHAPE: EditParams = {
  processVersion: '15.4',
  whiteBalance: 'asShot',
  temperature: 5500,
  tint: 0,
  exposure: 0,
  contrast: 0,
  highlights: 0,
  shadows: 0,
  whites: 0,
  blacks: 0,
  clarity: 0,
  texture: 0,
  dehaze: 0,
  vibrance: 0,
  saturation: 0,
  toneCurveRGB: [
    { x: 0, y: 0 },
    { x: 255, y: 255 },
  ],
  hsl: {
    red: { hue: 0, sat: 0, lum: 0 },
    orange: { hue: 0, sat: 0, lum: 0 },
    yellow: { hue: 0, sat: 0, lum: 0 },
    green: { hue: 0, sat: 0, lum: 0 },
    aqua: { hue: 0, sat: 0, lum: 0 },
    blue: { hue: 0, sat: 0, lum: 0 },
    purple: { hue: 0, sat: 0, lum: 0 },
    magenta: { hue: 0, sat: 0, lum: 0 },
  },
  colorGrade: { shadowHue: 0, shadowSat: 0, midtoneHue: 0, midtoneSat: 0, highlightHue: 0, highlightSat: 0, blend: 50, balance: 0 },
  calibration: { shadowTint: 0, redHue: 0, redSat: 0, greenHue: 0, greenSat: 0, blueHue: 0, blueSat: 0 },
  detail: { sharpness: 25, sharpenRadius: 1, sharpenDetail: 25, sharpenMasking: 0, luminanceSmoothing: 0, colorNR: 25, colorNRDetail: 50 },
  optics: { lensProfileEnable: true, manualDistortion: 0, vignetteAmount: 0, vignetteMidpoint: 50, vignetteFeather: 50, grainAmount: 0, grainFrequency: 50, grainSize: 25 },
  geometry: { hasCrop: false, perspective: { vertical: 0, horizontal: 0, rotate: 0, scale: 1, aspect: 0, x: 0, y: 0 } },
  masks: [],
};

/** 引擎返回的会话参数 → 补齐缺省形状（引擎值优先，数组以引擎为准） */
/** 引擎回传参数归一化：真引擎只回 14~17 个键（缺 toneCurveRGB/hsl 等），
 *  直接塞进 store 会让读取这些键的组件（WebGL 曲线 LUT 等）拿到 undefined。
 *  undo/redo/快照恢复等所有"引擎参数 → store"的入口都必须过这一层。 */
export function normalizeParams(p: EditParams): EditParams {
  return deepMergeLocal(structuredClone(DEFAULT_PARAMS_SHAPE), p as unknown as Partial<EditParams>);
}

function deepMergeLocal<T>(base: T, patch: Partial<T> | Record<string, unknown> | undefined): T {
  if (patch === undefined || patch === null) return base;
  if (Array.isArray(base) || Array.isArray(patch)) return patch as T;
  if (typeof base === 'object' && base !== null && typeof patch === 'object') {
    const out: Record<string, unknown> = { ...(base as Record<string, unknown>) };
    for (const [k, v] of Object.entries(patch as Record<string, unknown>)) {
      const bv = out[k];
      if (bv !== null && typeof bv === 'object' && !Array.isArray(bv) && v !== null && typeof v === 'object' && !Array.isArray(v)) {
        out[k] = deepMergeLocal(bv, v as Record<string, unknown>);
      } else {
        out[k] = v;
      }
    }
    return out as T;
  }
  return (patch as T) ?? base;
}

function zeroParams(p: EditParams): EditParams {
  return {
    ...p,
    whiteBalance: 'asShot',
    temperature: 5500,
    tint: 0,
    exposure: 0,
    contrast: 0,
    highlights: 0,
    shadows: 0,
    whites: 0,
    blacks: 0,
    clarity: 0,
    texture: 0,
    dehaze: 0,
    vibrance: 0,
    saturation: 0,
    toneCurveRGB: [
      { x: 0, y: 0 },
      { x: 255, y: 255 },
    ],
    masks: [],
  };
}

let maskSeq = 1;
export function buildMask(type: MaskLayer['type'], _index: number): MaskLayer {
  const id = `mask_${Date.now().toString(36)}_${maskSeq++}`;
  const base: MaskLayer = {
    id,
    type,
    enabled: true,
    opacity: 100,
    amount: 0,
    local: { exposure: 0, contrast: 0, highlights: 0, shadows: 0, temperature: 0, tint: 0, saturation: 0, clarity: 0, dehaze: 0 },
  };
  if (type === 'linear') {
    base.geometry = { x1: 0.5, y1: 0, x2: 0.5, y2: 1 };
    base.amount = -30;
  } else if (type === 'radial') {
    base.geometry = { cx: 0.5, cy: 0.5, radius: 0.35, ratio: 1 };
    base.amount = -20;
  } else if (type === 'brush') {
    base.strokes = [{ x: 0.5, y: 0.5, radius: 0.08, flow: 80, density: 100, erase: false }];
  } else if (type === 'luminanceRange') {
    // 范围蒙版（协议 §4 MaskLayer.range：low/high 0..1 + samples 采样点）
    base.range = { low: 0.25, high: 0.85, samples: [] };
    base.amount = 20;
  } else if (type === 'colorRange') {
    base.range = { low: 0.2, high: 0.9, samples: [32, 74, 120] };
    base.amount = 15;
  } else if (type === 'depth') {
    base.range = { low: 0.4, high: 1, samples: [] };
    base.amount = 10;
  }
  return base;
}

export function maskName(type: MaskLayer['type']): string {
  switch (type) {
    case 'linear':
      return '渐变';
    case 'radial':
      return '径向';
    case 'brush':
      return '画笔';
    case 'aiSky':
      return 'AI 天空';
    case 'aiSubject':
      return 'AI 主体';
    case 'luminanceRange':
      return '亮度范围';
    case 'colorRange':
      return '颜色范围';
    case 'depth':
      return '深度范围';
    default:
      return type;
  }
}

function maskLocalLabel(key: keyof MaskLayer['local']): string {
  const map: Record<string, string> = {
    exposure: '曝光',
    contrast: '对比',
    highlights: '高光',
    shadows: '阴影',
    temperature: '色温',
    tint: '色调',
    saturation: '饱和',
    clarity: '清晰',
    dehaze: '去雾',
  };
  return map[key] ?? key;
}

export function sliderLabel(key: keyof EditParams): string {
  const map: Record<string, string> = {
    exposure: '曝光',
    contrast: '对比',
    highlights: '高光',
    shadows: '阴影',
    whites: '白色',
    blacks: '黑色',
    clarity: '清晰度',
    texture: '纹理',
    dehaze: '去雾',
    vibrance: '自然饱和',
    saturation: '饱和度',
    temperature: '色温',
    tint: '色调',
  };
  return map[key as string] ?? String(key);
}

function formatEv(v: number): string {
  return `${v > 0 ? '+' : ''}${Math.round(v * 100) / 100}`;
}

/** 组件层轻量提示（动态导入避免 store ↔ 组件循环依赖） */
function useJobStoreToast(text: string): void {
  void import('./jobStore').then(({ useJobStore }) => useJobStore.getState().toast(text, 'info', 2500));
}
