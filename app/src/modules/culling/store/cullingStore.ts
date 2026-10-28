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
  /** 已发出的缩略图请求占位，键 = `${photoId}:${size}`（与 libraryStore 同一命名口径）。
   *  必须带 size：320/640 是两条独立请求，只按 id 记会让先发的那个把另一档永久抑制掉。 */
  thumbsRequested: Set<string>;
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

/** 决策落盘（异步；失败必须显式 toast，禁止静默）。返回 Promise 以便调用方回滚乐观更新 */
function persistDecision(photoId: number, patch: { rating?: number; pick?: PickFlag }): Promise<void> {
  return api.library
    .updatePhotos([photoId], patch)
    .then(() => undefined)
    .catch((err) => {
      useJobStore.getState().toast(`评级落盘失败：${errText(err)}（该项已退回队列，可重试）`, 'error', 8000);
      throw err;
    });
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
  thumbsRequested: new Set<string>(),
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
    /** 占位键带尺寸：同图 320 / 640 互不抑制（与 libraryStore 同口径） */
    const keyOf = (id: number) => `${id}:${size}`;
    const pending = ids.filter((id) => !st.thumbsRequested.has(keyOf(id)) && !st.thumbUrls[id]);
    if (pending.length === 0) return;
    set((s) => {
      const next = new Set(s.thumbsRequested);
      for (const id of pending) next.add(keyOf(id));
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
      // 失败必须解除占位：旧写法只 toast 不移除，这些 id 在本次会话里永远
      // 「被认为已请求」，缩略图从此不再重试（格子永久空白）
      set((s) => {
        const next = new Set(s.thumbsRequested);
        for (const id of pending) next.delete(keyOf(id));
        return { thumbsRequested: next };
      });
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

/** 对当前张应用决策：乐观更新本地队列 → 视需要自动晋级 → 异步落盘（失败按 id 回滚） */
function applyToCurrent(
  set: (partial: Partial<CullingStore> | ((s: CullingStore) => Partial<CullingStore>)) => void,
  get: () => CullingStore,
  patch: { rating?: Rating; pick?: PickFlag },
): void {
  const st = get();
  const list = filteredPhotos(st.photos, st.filterMode);
  const photo = list[st.currentIndex];
  if (!photo) return;
  // 回滚只还原「本次写过的那个字段」（rating / pick 各自独立）
  const nextPhotos = st.photos.map((p) => (p.id === photo.id ? { ...p, ...patch } : p));
  const nextList = filteredPhotos(nextPhotos, st.filterMode);
  // 「当前张是否还在筛选结果里」：未评级筛选下评级后该项出列，后继会占用同一个下标 ——
  // 此时再 +1 就会漏掉一张（P12）。出列 → 停在原下标（= 后继）；仍在 → 前进一张。
  const stillInList = nextList[st.currentIndex]?.id === photo.id;
  let nextIndex: number;
  if (st.autoAdvance) {
    nextIndex = stillInList ? st.currentIndex + 1 : st.currentIndex;
  } else {
    nextIndex = st.currentIndex;
  }
  nextIndex = Math.min(Math.max(0, nextIndex), Math.max(0, nextList.length - 1));
  set({
    photos: nextPhotos,
    currentIndex: nextIndex,
    sessionRated: st.sessionRated + 1,
    // 决策后退出对比态，避免残留上一张画面
    compare: false,
  });
  // 落盘失败 → 按 id 单张回滚（toast 已在 persistDecision 内提示）。
  // 不能用整份 photos/currentIndex 快照：断网时连按两次评级，第一次的失败响应晚到
  // 会把第二次（已成功落盘）的决策整包抹掉，界面与磁盘从此不一致。
  void persistDecision(photo.id, patch).catch(() => {
    set((s) => {
      const cur = s.photos.find((p) => p.id === photo.id);
      // 仅当该 id 当前值仍等于本次写入值时才回滚：期间若有更新的（成功的）决策，以新值为准
      const stillOurs =
        cur !== undefined &&
        (patch.rating === undefined || cur.rating === patch.rating) &&
        (patch.pick === undefined || cur.pick === patch.pick);
      return {
        photos: stillOurs
          ? s.photos.map((p) => {
              if (p.id !== photo.id) return p;
              const merged: PhotoSummary = { ...p };
              if (patch.rating !== undefined) merged.rating = photo.rating;
              if (patch.pick !== undefined) merged.pick = photo.pick;
              return merged;
            })
          : s.photos,
        // 本次决策次数递减（与本次 +1 对称）；游标不回退，用户可能已经往下看了
        sessionRated: Math.max(0, s.sessionRated - 1),
      };
    });
  });
}
