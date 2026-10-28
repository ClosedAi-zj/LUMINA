/**
 * 作品集 store：基于 albums 域（kind: 'portfolio' | 'collection'）。
 * 成员清单经 albums.getItems 回显（引擎按 position 排序返回 id 列表）；
 * 挑选器按拖拽顺序 addPhotos 落库，顺序即画册内顺序。
 */

import { create } from 'zustand';
import { api } from '../../../api/engine';
import { errText } from '../../../api/rpc-client';
import { useJobStore } from '../../../store/jobStore';
import type { AlbumInfo, PhotoSummary } from '../../../api/protocol';

interface PortfolioStore {
  albums: AlbumInfo[];
  loading: boolean;
  loadError: string | null;
  selectedId: number | null;
  thumbUrls: Record<number, string>;
  /** 已发出的缩略图请求占位，键 = `${photoId}:${size}`（与 libraryStore / cullingStore 同一命名口径）。
   *  必须带 size：320/640 是两条独立请求，只按 id 记会让先发的那个把另一档永久抑制掉。 */
  thumbsRequested: Set<string>;
  /** 当前选中画册的成员（albums.getItems 返回的 position 顺序） */
  members: number[];
  membersLoading: boolean;

  /** 挑选器 */
  pickerOpen: boolean;
  /** 挑选器候选（图库 pick 照片） */
  pickerCandidates: PhotoSummary[];
  pickerLoading: boolean;
  /** 已挑选成员（有序，即未来作品集内顺序） */
  pickedIds: number[];
  adding: boolean;

  load: () => Promise<void>;
  select: (id: number | null) => void;
  loadMembers: () => Promise<void>;
  /** 缩略图预取；size 默认 320（封面/成员/挑选器同档），接口与 libraryStore 对齐便于日后统一抽取 */
  ensureThumbs: (ids: number[], size?: 320 | 640) => Promise<void>;
  createAlbum: (name: string, kind: 'portfolio' | 'collection') => Promise<void>;
  openPicker: () => Promise<void>;
  closePicker: () => void;
  togglePick: (id: number) => void;
  movePick: (id: number, targetId: number) => void;
  addPicked: () => Promise<void>;
  removeMember: (photoId: number) => Promise<void>;
}

export const usePortfolio = create<PortfolioStore>((set, get) => ({
  albums: [],
  loading: false,
  loadError: null,
  selectedId: null,
  thumbUrls: {},
  thumbsRequested: new Set<string>(),
  members: [],
  membersLoading: false,

  pickerOpen: false,
  pickerCandidates: [],
  pickerLoading: false,
  pickedIds: [],
  adding: false,

  load: async () => {
    if (get().loading) return;
    set({ loading: true, loadError: null });
    try {
      const albums = await api.albums.list();
      set((s) => ({
        albums,
        loading: false,
        loadError: null,
        selectedId: s.selectedId ?? albums[0]?.id ?? null,
      }));
      // 封面缩略
      const coverIds = albums.map((a) => a.coverPhotoId).filter((id) => id > 0);
      if (coverIds.length > 0) void get().ensureThumbs(coverIds);
      await get().loadMembers();
    } catch (err) {
      set({ loading: false, loadError: errText(err) });
    }
  },

  select: (id) => {
    set({ selectedId: id, pickerOpen: false });
    void get().loadMembers();
  },

  /** 成员清单回显（albums.getItems，position 顺序）+ 顺手拉缩略图 */
  loadMembers: async () => {
    const albumId = get().selectedId;
    if (albumId === null) {
      set({ members: [] });
      return;
    }
    set({ membersLoading: true });
    try {
      const res = await api.albums.getItems(albumId);
      set({ members: res.items, membersLoading: false });
      if (res.items.length > 0) void get().ensureThumbs(res.items);
    } catch (err) {
      set({ membersLoading: false });
      useJobStore.getState().toast(`成员清单加载失败：${errText(err)}`, 'error', 6000);
    }
  },

  removeMember: async (photoId) => {
    const albumId = get().selectedId;
    if (albumId === null) return;
    try {
      await api.albums.removePhotos(albumId, [photoId]);
      set((s) => ({ members: s.members.filter((m) => m !== photoId) }));
      await get().load();
    } catch (err) {
      useJobStore.getState().toast(`移除成员失败：${errText(err)}`, 'error', 6000);
    }
  },

  createAlbum: async (name, kind) => {
    try {
      await api.albums.create(name, kind);
      useJobStore.getState().toast(`已创建${kind === 'portfolio' ? '作品集' : '集合'}「${name}」`, 'success');
      await get().load();
    } catch (err) {
      useJobStore.getState().toast(`创建失败：${errText(err)}`, 'error', 8000);
    }
  },

  openPicker: async () => {
    set({ pickerOpen: true, pickerLoading: true, pickedIds: [] });
    try {
      const res = await api.library.queryPhotos({ pick: 'pick' }, { field: 'capturedAt', order: 'asc' }, { offset: 0, limit: 300 });
      set({ pickerCandidates: res.items, pickerLoading: false });
      void get().ensureThumbs(res.items.slice(0, 120).map((p) => p.id));
    } catch (err) {
      set({ pickerLoading: false });
      useJobStore.getState().toast(`候选照片加载失败：${errText(err)}`, 'error', 8000);
    }
  },

  closePicker: () => set({ pickerOpen: false }),

  togglePick: (id) =>
    set((s) => ({
      pickedIds: s.pickedIds.includes(id) ? s.pickedIds.filter((i) => i !== id) : [...s.pickedIds, id],
    })),

  /** HTML5 拖拽排序：把 id 移动到 targetId 之前/之后（按拖放目标位置） */
  movePick: (id, targetId) => {
    if (id === targetId) return;
    set((s) => {
      const list = s.pickedIds.filter((i) => i !== id);
      const idx = list.indexOf(targetId);
      if (idx < 0) return { pickedIds: [...list, id] };
      list.splice(idx, 0, id);
      return { pickedIds: list };
    });
  },

  addPicked: async () => {
    const st = get();
    const albumId = st.selectedId;
    if (albumId === null || st.pickedIds.length === 0) return;
    set({ adding: true });
    try {
      const { added } = await api.albums.addPhotos(albumId, st.pickedIds);
      useJobStore.getState().toast(`已按挑选顺序加入 ${added} 张`, 'success');
      set({ adding: false, pickerOpen: false, pickedIds: [] });
      await get().load();
    } catch (err) {
      set({ adding: false });
      useJobStore.getState().toast(`加入作品集失败：${errText(err)}`, 'error', 8000);
    }
  },

  ensureThumbs: async (ids, size = 320) => {
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
      // 「被认为已请求」，封面/成员缩略图从此不再重试（永久空白）
      set((s) => {
        const next = new Set(s.thumbsRequested);
        for (const id of pending) next.delete(keyOf(id));
        return { thumbsRequested: next };
      });
      useJobStore.getState().toast(`封面缩略加载失败：${errText(err)}`, 'error', 6000);
    }
  },
}));
