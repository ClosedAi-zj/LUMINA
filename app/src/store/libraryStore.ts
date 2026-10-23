/**
 * 图库浏览 store：筛选/排序/分页缓存/选择模型/元数据。
 * 分页缓存按 offset 键控（页大小 200，与引擎预编译查询口径一致）。
 */

import { create } from 'zustand';
import { api } from '../api/engine';
import { errText } from '../api/rpc-client';
import type { FacetsResult, Filter, FolderNode, LibraryStats, PhotoSummary, Sort, TimelineBucket } from '../api/protocol';

export const PAGE_LIMIT = 200;
export const ZOOM_MIN = 120;
export const ZOOM_MAX = 320;

export interface FilterChip {
  key: string;
  label: string;
  remove: () => void;
}

interface LibraryStore {
  filter: Filter;
  sort: Sort;
  searchText: string;
  zoom: number;
  pages: Record<number, PhotoSummary[]>;
  thumbUrls: Record<number, string>;
  thumbsRequested: Set<number>;
  total: number;
  selectedIds: number[];
  anchorId: number | null;
  currentPhotoId: number | null;
  folderTree: FolderNode[];
  timeline: TimelineBucket[];
  facets: FacetsResult | null;
  stats: LibraryStats | null;
  activeSmartId: number | null;
  collapsedRoots: number[];
  filterVersion: number;
  detailLoading: boolean;
  /** 最近一次查询失败原因（WP-B3 零静默：页面凭它渲染失败态） */
  lastError: string | null;

  setFilter: (patch: Filter) => void;
  clearFilter: () => void;
  clearFilterKey: (key: keyof Filter) => void;
  setSort: (s: Sort) => void;
  setSearchText: (t: string) => void;
  setZoom: (z: number) => void;
  setActiveSmart: (id: number | null, filter: Filter) => void;
  toggleRootCollapse: (rootId: number) => void;
  fetchPage: (offset: number) => Promise<void>;
  refresh: () => Promise<void>;
  loadMeta: () => Promise<void>;
  ensureThumbs: (ids: number[], size: 320 | 640) => Promise<void>;
  applyThumbReady: (photoId: number, path: string) => void;
  select: (id: number, mode: 'set' | 'toggle' | 'range' | 'all', items?: PhotoSummary[]) => void;
  setCurrent: (id: number | null) => void;
  clearSelection: () => void;
}

export const useLibrary = create<LibraryStore>((set, get) => ({
  filter: {},
  sort: { field: 'capturedAt', order: 'asc' },
  searchText: '',
  zoom: 216,
  pages: {},
  thumbUrls: {},
  thumbsRequested: new Set(),
  total: 0,
  selectedIds: [],
  anchorId: null,
  currentPhotoId: null,
  folderTree: [],
  timeline: [],
  facets: null,
  stats: null,
  activeSmartId: null,
  collapsedRoots: [],
  filterVersion: 0,
  detailLoading: false,
  lastError: null,

  setFilter: (patch) => {
    set((st) => ({ filter: { ...st.filter, ...patch }, activeSmartId: null, filterVersion: st.filterVersion + 1, pages: {}, total: 0 }));
    void get().refresh();
  },

  clearFilter: () => {
    set((st) => ({ filter: {}, searchText: '', activeSmartId: null, filterVersion: st.filterVersion + 1, pages: {}, total: 0 }));
    void get().refresh();
  },

  clearFilterKey: (key) => {
    const { [key]: _removed, ...rest } = get().filter;
    void _removed;
    set((st) => ({ filter: rest, filterVersion: st.filterVersion + 1, pages: {}, total: 0 }));
    void get().refresh();
  },

  setSort: (s) => {
    set({ sort: s, pages: {}, total: 0, filterVersion: get().filterVersion + 1 });
    void get().refresh();
  },

  setSearchText: (t) => set({ searchText: t }),

  setZoom: (z) => set({ zoom: Math.max(ZOOM_MIN, Math.min(ZOOM_MAX, z)) }),

  setActiveSmart: (id, filter) => {
    set({ activeSmartId: id, filter, filterVersion: get().filterVersion + 1, pages: {}, total: 0 });
    void get().refresh();
  },

  toggleRootCollapse: (rootId) =>
    set((st) => ({
      collapsedRoots: st.collapsedRoots.includes(rootId) ? st.collapsedRoots.filter((r) => r !== rootId) : [...st.collapsedRoots, rootId],
    })),

  fetchPage: async (offset) => {
    const st = get();
    if (st.pages[offset]) return;
    try {
      const text = st.searchText.trim();
      const res = text
        ? await api.library.search(text, { offset, limit: PAGE_LIMIT })
        : await api.library.queryPhotos(st.filter, st.sort, { offset, limit: PAGE_LIMIT });
      // 响应返回时筛选条件可能已变化：以 filterVersion 简单防竞态（仅当期间无变更才写入）
      set((s) => {
        if (s.filterVersion !== st.filterVersion) return s;
        return { pages: { ...s.pages, [offset]: res.items }, total: res.total, lastError: null };
      });
    } catch (err) {
      // 错误纪律：查询失败必须可见（toast + 页面失败态），不允许返回空数组冒充
      const { useJobStore } = await import('./jobStore');
      const msg = errText(err);
      useJobStore.getState().toast(`图库查询失败：${msg}`, 'error', 8000);
      set((s) => (s.filterVersion === st.filterVersion ? { lastError: msg } : s));
    }
  },

  refresh: async () => {
    set({ pages: {}, total: 0 });
    await get().fetchPage(0);
    await get().loadMeta();
  },

  loadMeta: async () => {
    const st = get();
    try {
      const [tree, timeline, facets, stats] = await Promise.all([
        api.library.getFolderTree(),
        api.library.getTimeline(),
        api.library.getFacets(st.filter),
        api.library.getStats(),
      ]);
      set({ folderTree: tree, timeline, facets, stats });
    } catch (err) {
      const { useJobStore } = await import('./jobStore');
      useJobStore.getState().toast(`图库元数据加载失败：${errText(err)}`, 'error', 8000);
    }
  },

  ensureThumbs: async (ids, size) => {
    const { thumbsRequested, thumbUrls } = get();
    const pending = ids.filter((id) => !thumbsRequested.has(id) && !thumbUrls[id]);
    if (pending.length === 0) return;
    set((st) => {
      const next = new Set(st.thumbsRequested);
      for (const id of pending) next.add(id);
      return { thumbsRequested: next };
    });
    try {
      const res = await api.thumb.ensureBatch(pending, size);
      const urls = { ...get().thumbUrls };
      for (const item of res.items) {
        if (item.state === 'ready' && item.path) urls[item.id] = item.path;
      }
      if ((window as unknown as { __LUMINA_SMOKE__?: boolean }).__LUMINA_SMOKE__) {
        console.log(`[smoke-thumb] requested=${pending.length} ready=${res.items.filter((i) => i.state === 'ready').length} urls=${Object.keys(urls).length}`);
      }
      set((st) => {
        // 同步回写分页缓存中的 PhotoSummary.thumbPath（协议字段），保持数据流一致
        const pages: Record<number, PhotoSummary[]> = {};
        for (const [off, page] of Object.entries(st.pages)) {
          pages[Number(off)] = page.map((p) => {
            const path = urls[p.id];
            return path ? { ...p, thumbPath: path, thumbState: 'ready' as const } : p;
          });
        }
        return { thumbUrls: urls, pages };
      });
    } catch (err) {
      const { useJobStore } = await import('./jobStore');
      useJobStore.getState().toast(`缩略图请求失败：${errText(err)}`, 'error', 6000);
    }
  },

  applyThumbReady: (photoId, path) => {
    set((st) => ({ thumbUrls: { ...st.thumbUrls, [photoId]: path } }));
  },

  select: (id, mode, items) => {
    const st = get();
    if (mode === 'set') {
      set({ selectedIds: [id], anchorId: id, currentPhotoId: id });
      return;
    }
    if (mode === 'all') {
      const flat = (items ?? Object.values(st.pages).flat()).map((p) => p.id);
      set({ selectedIds: flat });
      return;
    }
    if (mode === 'toggle') {
      set({ selectedIds: st.selectedIds.includes(id) ? st.selectedIds.filter((i) => i !== id) : [...st.selectedIds, id], anchorId: id });
      return;
    }
    // range：anchor → id 范围全选（按当前排序结果线性位置）
    if (st.anchorId !== null && items) {
      const ids = items.map((p) => p.id);
      const a = ids.indexOf(st.anchorId);
      const b = ids.indexOf(id);
      if (a >= 0 && b >= 0) {
        const [lo, hi] = a < b ? [a, b] : [b, a];
        set({ selectedIds: ids.slice(lo, hi + 1) });
        return;
      }
    }
    set({ selectedIds: [id], anchorId: id });
  },

  setCurrent: (id) => {
    if (id === null) {
      set({ currentPhotoId: null });
      return;
    }
    set({ currentPhotoId: id, selectedIds: [id], anchorId: id });
  },
  clearSelection: () => set({ selectedIds: [] }),
}));
