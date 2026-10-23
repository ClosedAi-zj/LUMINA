/**
 * Web 画廊（管理侧）store：发布向导（选集→标题/客户/密码→发布）、
 * 已发布列表（链接复制/撤销发布/反馈统计）。静态客户页由引擎 gallery 域产出。
 */

import { create } from 'zustand';
import { api } from '../../../api/engine';
import { errText } from '../../../api/rpc-client';
import { useJobStore } from '../../../store/jobStore';
import type { AlbumInfo, PublishResult, PublishedGallery } from '../../../api/protocol';

/**
 * 画廊设置（WP-B8 设计稿态）。
 * ⚠️ 引擎侧目前没有有效期默认值/下载计数/评论的存储字段——这三项是
 * 「纯前端状态（localStorage 暂存）」，供主理人评审字段设计后统一排期引擎落库。
 * 唯一已落库的是 gallery.publish 的 expiryDays（1..3650），发布时真实生效。
 */
export interface GallerySettings {
  /** 新发布默认有效期（天） */
  defaultExpiryDays: number;
  /** 允许客户下载原图（待引擎支持；当前仅本机暂存） */
  allowDownloads: boolean;
  /** 下载上限张数，0 = 不限（待引擎支持） */
  downloadLimit: number;
  /** 允许客户留言（待引擎支持） */
  allowComments: boolean;
}

const SETTINGS_KEY = 'lumina.gallery.settings';

const DEFAULT_SETTINGS: GallerySettings = {
  defaultExpiryDays: 30,
  allowDownloads: false,
  downloadLimit: 0,
  allowComments: false,
};

function loadSettings(): GallerySettings {
  try {
    const raw = localStorage.getItem(SETTINGS_KEY);
    if (!raw) return { ...DEFAULT_SETTINGS };
    return { ...DEFAULT_SETTINGS, ...(JSON.parse(raw) as Partial<GallerySettings>) };
  } catch {
    return { ...DEFAULT_SETTINGS };
  }
}

function saveSettings(s: GallerySettings): void {
  try {
    localStorage.setItem(SETTINGS_KEY, JSON.stringify(s));
  } catch {
    // localStorage 写失败（隐私模式等）：内存态仍可用，不静默吞掉
    void import('../../../store/jobStore').then(({ useJobStore }) =>
      useJobStore.getState().toast('画廊设置仅本次会话有效（本地存储写入失败）', 'warn', 6000),
    );
  }
}

interface GalleryStore {
  albums: AlbumInfo[];
  published: PublishedGallery[];
  loading: boolean;
  /** gallery.* 不可用时的降级提示（零静默） */
  loadError: string | null;

  // 发布向导
  wizardOpen: boolean;
  albumId: number | null;
  title: string;
  customerName: string;
  password: string;
  /** 有效期（天）：引擎 gallery.publish 原生支持 expiryDays 1..3650，此前 UI 写死 30 */
  expiryDays: number;
  /** 画廊设置（前端暂存；下载/评论待引擎字段，见 SETTINGS 注释） */
  settings: GallerySettings;
  publishing: boolean;
  /** 最近一次发布结果（成功卡展示可复制链接） */
  lastResult: PublishResult | null;

  load: () => Promise<void>;
  openWizard: (albumId?: number) => void;
  closeWizard: () => void;
  patchWizard: (p: { albumId?: number; title?: string; customerName?: string; password?: string; expiryDays?: number }) => void;
  patchSettings: (p: Partial<GallerySettings>) => void;
  publish: () => Promise<void>;
  revoke: (albumId: number) => Promise<void>;
  clearResult: () => void;
}

export const useGallery = create<GalleryStore>((set, get) => ({
  albums: [],
  published: [],
  loading: false,
  loadError: null,

  wizardOpen: false,
  albumId: null,
  title: '',
  customerName: '',
  password: '',
  expiryDays: 30,
  settings: loadSettings(),
  publishing: false,
  lastResult: null,

  load: async () => {
    if (get().loading) return;
    set({ loading: true, loadError: null });
    try {
      const [albums, published] = await Promise.all([api.albums.list(), api.gallery.listPublished()]);
      set({ albums, published, loading: false, loadError: null });
    } catch (err) {
      set({ loading: false, loadError: errText(err) });
    }
  },

  openWizard: (albumId) =>
    set((s) => ({
      wizardOpen: true,
      lastResult: null,
      albumId: albumId ?? s.albums.find((a) => !a.published)?.id ?? s.albums[0]?.id ?? null,
      title: '',
      customerName: '',
      password: '',
      expiryDays: get().settings.defaultExpiryDays,
    })),

  closeWizard: () => set({ wizardOpen: false }),

  patchWizard: (p) => set((s) => ({ ...s, ...p })),

  // 保存到 localStorage（引擎无对应存储位，仅本机暂存）；内存态立即生效，便于"保存后回读一致"验证
  patchSettings: (p) =>
    set((s) => {
      const next = { ...s.settings, ...p };
      saveSettings(next);
      return { settings: next };
    }),

  publish: async () => {
    const st = get();
    if (st.albumId === null || !st.title.trim() || !st.customerName.trim()) return;
    set({ publishing: true });
    try {
      const result = await api.gallery.publish(st.albumId, st.password, st.title.trim(), st.customerName.trim(), st.expiryDays);
      set({ publishing: false, lastResult: result });
      useJobStore.getState().toast(`画廊发布成功 · 链接 ${st.expiryDays} 天内有效`, 'success');
      await get().load();
    } catch (err) {
      set({ publishing: false });
      // 6001 重复发布等错误显式提示（零静默）
      useJobStore.getState().toast(`发布失败：${errText(err)}`, 'error', 10000);
    }
  },

  revoke: async (albumId) => {
    try {
      await api.gallery.revoke(albumId);
      useJobStore.getState().toast('已撤销发布 · 客户链接即刻失效', 'info');
      set({ lastResult: null });
      await get().load();
    } catch (err) {
      useJobStore.getState().toast(`撤销失败：${errText(err)}`, 'error', 8000);
    }
  },

  clearResult: () => set({ lastResult: null }),
}));
