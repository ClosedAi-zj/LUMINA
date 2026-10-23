/**
 * 精选模式 store：候选队列加载、评级决策（乐观更新 + 引擎落盘）、
 * 筛选（全部/未评级/精选/淘汰）、自动晋级、缩放与对比态。
 * 数据源：api.library.queryPhotos（单页大限额）；落盘走 api.library.updatePhotos。
 */

import { create } from 'zustand';
import { api } from '../../../api/engine';
import { errText } from '../../../api/rpc-client';
import { useJobStore } from '../../../store/jobStore';
import type { PickFlag, PhotoSummary, Rating } from '../../../api/protocol';

export type CullingFilterMode = 'all' | 'unrated' | 'picked' | 'rejected';

/** 单次载入上限：精选按批处理，1000 张足够一个工作会话 */
const LOAD_LIMIT = 1000;

export interface PreviewImage {
  path: string;
  width: number;
  height: number;
}

interface CullingStore {
  photos: PhotoSummary[];
  total: number;
  loading: boolean;
  loadError: string | null;
  currentIndex: number;
  filterMode: CullingFilterMode;
  autoAdvance: boolean;
  zoomed: boolean;
  compare: boolean;
  /** 本次会话已做出的决策次数（工具条展示） */
  sessionRated: number;
  previewUrls: Record<number, PreviewImage>;
  thumbUrls: Record<number, string>;
  thumbsRequested: Set<number>;
  previewRequested: Set<number>;

  load: () => Promise<void>;
  reload: () => Promise<void>;
  setFilterMode: (m: CullingFilterMode) => void;
  setCurrentIndex: (i: number) => void;
  step: (delta: number) => void;
  rate: (n: Rating) => void;
  togglePick: () => void;
  toggleReject: () => void;
  clearDecision: () => void;
  toggleZoom: () => void;
  toggleCompare: () => void;
  setCompare: (v: boolean) => void;
  toggleAutoAdvance: () => void;
  ensureThumbs: (ids: number[], size: 320 | 640) => Promise<void>;
  ensurePreview: (id: number) => Promise<void>;
}

/** 筛选谓词（导出供组件复用，保证单一口径） */
export function matchesFilter(p: PhotoSummary, mode: CullingFilterMode): boolean {
  switch (mode) {
    case 'all':
      return true;
    case 'unrated':
      return p.rating === 0 && p.pick === 'none';
    case 'picked':
      return p.pick === 'pick';
    case 'rejected':
      return p.pick === 'reject';
    default:
      return false;
  }
}

export function filteredPhotos(photos: PhotoSummary[], mode: CullingFilterMode): PhotoSummary[] {
  if (mode === 'all') return photos;
  return photos.filter((p) => matchesFilter(p, mode));
}

/** 决策落盘（fire-and-forget；失败必须显式 toast，禁止静默） */
function persistDecision(photoId: number, patch: { rating?: number; pick?: PickFlag }): void {
  api.library
    .updatePhotos([photoId], patch)
    .catch((err) => useJobStore.getState().toast(`评级落盘失败：${errText(err)}`, 'error', 8000));
}

export const useCulling = create<CullingStore>((set, get) => ({
  photos: [],
  total: 0,
  loading: false,
  loadError: null,
  currentIndex: 0,
  filterMode: 'all',
  autoAdvance: true,
  zoomed: false,
  compare: false,
  sessionRated: 0,
  previewUrls: {},
  thumbUrls: {},
  thumbsRequested: new Set(),
  previewRequested: new Set(),

  load: async () => {
    const st = get();
    if (st.loading || st.photos.length > 0) return;
    set({ loading: true, loadError: null });
    try {
      const res = await api.library.queryPhotos(undefined, { field: 'capturedAt', order: 'asc' }, { offset: 0, limit: LOAD_LIMIT });
      set({ photos: res.items, total: res.total, loading: false, currentIndex: 0 });
    } catch (err) {
      set({ loading: false, loadError: errText(err) });
    }
  },

  reload: async () => {
    set({ photos: [], currentIndex: 0 });
    await get().load();
  },

  setFilterMode: (m) => set({ filterMode: m, currentIndex: 0, zoomed: false, compare: false }),

  setCurrentIndex: (i) => {
    const st = get();
    const n = filteredPhotos(st.photos, st.filterMode).length;
    set({ currentIndex: Math.max(0, Math.min(n - 1, i)) });
  },

  step: (delta) => {
    const st = get();
    const n = filteredPhotos(st.photos, st.filterMode).length;
    set({ currentIndex: Math.max(0, Math.min(n - 1, st.currentIndex + delta)) });
  },

  rate: (n) => {
    const st = get();
    const photo = filteredPhotos(st.photos, st.filterMode)[st.currentIndex];
    if (!photo) return;
    // 同星再按一次 = 取消评分
    const next: Rating = photo.rating === n ? 0 : n;
    applyToCurrent(set, get, { rating: next });
  },

  togglePick: () => {
    const st = get();
    const photo = filteredPhotos(st.photos, st.filterMode)[st.currentIndex];
    if (!photo) return;
    applyToCurrent(set, get, { pick: photo.pick === 'pick' ? 'none' : 'pick' });
  },

  toggleReject: () => {
    const st = get();
    const photo = filteredPhotos(st.photos, st.filterMode)[st.currentIndex];
    if (!photo) return;
    applyToCurrent(set, get, { pick: photo.pick === 'reject' ? 'none' : 'reject' });
  },

  clearDecision: () => applyToCurrent(set, get, { rating: 0, pick: 'none' }),

  toggleZoom: () => set((s) => ({ zoomed: !s.zoomed })),
  toggleCompare: () => set((s) => ({ compare: !s.compare })),
  setCompare: (v) => set({ compare: v }),
  toggleAutoAdvance: () => set((s) => ({ autoAdvance: !s.autoAdvance })),

  ensureThumbs: async (ids, size) => {
    const st = get();
    const pending = ids.filter((id) => !st.thumbsRequested.has(id) && !st.thumbUrls[id]);
    if (pending.length === 0) return;
    set((s) => {
      const next = new Set(s.thumbsRequested);
      for (const id of pending) next.add(id);
      return { thumbsRequested: next };
    });
    try {
      const res = await api.thumb.ensureBatch(pending, size);
      set((s) => {
        const urls = { ...s.thumbUrls };
        for (const item of res.items) {
          if (item.state === 'ready' && item.path) urls[item.id] = item.path;
        }
        return { thumbUrls: urls };
      });
    } catch (err) {
      useJobStore.getState().toast(`缩略图加载失败：${errText(err)}`, 'error', 6000);
    }
  },

  ensurePreview: async (id) => {
    const st = get();
    if (st.previewRequested.has(id) || st.previewUrls[id]) return;
    set((s) => {
      const next = new Set(s.previewRequested);
      next.add(id);
      return { previewRequested: next };
    });
    try {
      const res = await api.preview.getBase(id);
      set((s) => ({ previewUrls: { ...s.previewUrls, [id]: { path: res.path, width: res.width, height: res.height } } }));
    } catch {
      // 大图降级：回退 640 缩略图，保证观察区不空转
      try {
        const res = await api.thumb.ensureBatch([id], 640);
        const item = res.items.find((i) => i.id === id);
        if (item?.state === 'ready' && item.path) {
          set((s) => ({ previewUrls: { ...s.previewUrls, [id]: { path: item.path as string, width: 640, height: 427 } } }));
        }
      } catch (err2) {
        useJobStore.getState().toast(`大图加载失败：${errText(err2)}`, 'error', 6000);
      }
    }
  },
}));

/** 对当前张应用决策：乐观更新本地队列 → 视需要自动晋级 → 异步落盘 */
function applyToCurrent(
  set: (partial: Partial<CullingStore> | ((s: CullingStore) => Partial<CullingStore>)) => void,
  get: () => CullingStore,
  patch: { rating?: Rating; pick?: PickFlag },
): void {
  const st = get();
  const list = filteredPhotos(st.photos, st.filterMode);
  const photo = list[st.currentIndex];
  if (!photo) return;
  const nextPhotos = st.photos.map((p) => (p.id === photo.id ? { ...p, ...patch } : p));
  const nextList = filteredPhotos(nextPhotos, st.filterMode);
  let nextIndex: number;
  if (st.autoAdvance) {
    // 自动晋级：落在"下一张"上；筛选导致当前张出列时，索引自然指向后继
    nextIndex = Math.min(nextList.length - 1, st.currentIndex + 1);
  } else {
    nextIndex = Math.min(st.currentIndex, nextList.length - 1);
  }
  set({
    photos: nextPhotos,
    currentIndex: Math.max(0, nextIndex),
    sessionRated: st.sessionRated + 1,
    // 决策后退出对比态，避免残留上一张画面
    compare: false,
  });
  persistDecision(photo.id, patch);
}
