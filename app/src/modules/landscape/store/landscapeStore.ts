/**
 * 风光专项 store：HDR 包围组（检测/合成）+ AI 降噪（应用/前后预览）。
 * 版本纪律：引擎侧未实现的能力展示「v1.1 提供」降级卡，不算错误。
 */

import { create } from 'zustand';
import { api } from '../../../api/engine';
import { rpc } from '../../../api/rpc-client';
import { errText } from '../../../api/rpc-client';
import { useJobStore } from '../../../store/jobStore';
import type { BracketGroup, StackGroup } from '../../../api/protocol';

interface LandscapeStore {
  // HDR
  groups: BracketGroup[];
  groupsLoading: boolean;
  groupsError: string | null;
  selectedGroupId: number | null;
  deghost: boolean;
  autoAlign: boolean;
  outputFormat: 'dng' | 'tiff';
  hdrJobId: number | null;
  /** HDR 合成完成后的产物照片 id（引擎合成后已入图库索引，可进修图台） */
  hdrResultPhotoId: number | null;

  // 焦点堆栈（景深堆栈）v1.3.0
  stackGroups: StackGroup[];
  stackGroupsLoading: boolean;
  stackGroupsError: string | null;
  selectedStackGroupId: number | null;
  stackAutoAlign: boolean;
  stackOutputFormat: 'dng' | 'tiff';
  stackSmoothness: number;
  stackJobId: number | null;
  /** 焦点堆栈合成完成后的产物照片 id（引擎合成后已入图库索引，可进修图台） */
  stackResultPhotoId: number | null;

  // AI 降噪
  photoId: number | null;
  photoName: string;
  origPath: string | null;
  denoisedPath: string | null;
  denoisePreviewLoading: boolean;
  strength: number;
  luminanceMask: boolean;
  denoiseJobId: number | null;

  load: () => Promise<void>;
  selectGroup: (id: number | null) => void;
  patchComposite: (p: { deghost?: boolean; autoAlign?: boolean; outputFormat?: 'dng' | 'tiff' }) => void;
  runComposite: () => Promise<void>;
  /** 打开合成产物（修图台） */
  openHdrResult: () => Promise<void>;
  selectStackGroup: (id: number | null) => void;
  patchStackComposite: (p: { autoAlign?: boolean; outputFormat?: 'dng' | 'tiff'; smoothness?: number }) => void;
  runStackComposite: () => Promise<void>;
  openStackResult: () => Promise<void>;
  setStrength: (v: number) => void;
  setLuminanceMask: (v: boolean) => void;
  refreshDenoisePreview: () => Promise<void>;
  applyDenoise: () => Promise<void>;
}

export const useLandscape = create<LandscapeStore>((set, get) => ({
  groups: [],
  groupsLoading: false,
  groupsError: null,
  selectedGroupId: null,
  deghost: true,
  autoAlign: true,
  outputFormat: 'dng',
  hdrJobId: null,
  hdrResultPhotoId: null,

  stackGroups: [],
  stackGroupsLoading: false,
  stackGroupsError: null,
  selectedStackGroupId: null,
  stackAutoAlign: true,
  stackOutputFormat: 'dng',
  stackSmoothness: 4,
  stackJobId: null,
  stackResultPhotoId: null,

  photoId: null,
  photoName: '',
  origPath: null,
  denoisedPath: null,
  denoisePreviewLoading: false,
  strength: 50,
  luminanceMask: true,
  denoiseJobId: null,

  load: async () => {
    if (get().groupsLoading || get().stackGroupsLoading) return;
    bindHdrEvents();
    bindStackEvents();
    set({ groupsLoading: true, groupsError: null, hdrResultPhotoId: null, stackGroupsLoading: true, stackGroupsError: null, stackResultPhotoId: null });
    try {
      const groups = await api.hdr.detectBracketGroups();
      set({ groups, groupsLoading: false, groupsError: null, selectedGroupId: groups[0]?.groupId ?? null });
    } catch (err) {
      set({ groupsLoading: false, groupsError: errText(err) });
    }
    // 焦点堆栈组检测（v1.3.0，独立于 HDR，失败不阻断页面）
    try {
      const stackGroups = await api.stack.detectStackGroups();
      set({ stackGroups, stackGroupsLoading: false, stackGroupsError: null, selectedStackGroupId: stackGroups[0]?.groupId ?? null });
    } catch (err) {
      set({ stackGroupsLoading: false, stackGroupsError: errText(err) });
    }
    // 降噪样张：取一张 RW2 作为演示对象
    try {
      const res = await api.library.queryPhotos({ formats: ['RW2'] }, { field: 'rating', order: 'desc' }, { offset: 0, limit: 1 });
      const photo = res.items[0];
      if (photo) {
        set({ photoId: photo.id, photoName: photo.fileName });
        try {
          const base = await api.preview.getBase(photo.id);
          set({ origPath: base.path });
        } catch {
          // 无预览时滑块只显示降噪侧
        }
        await get().refreshDenoisePreview();
      }
    } catch {
      // 样张缺失不阻断页面，降噪卡显示空态
    }
  },

  selectGroup: (id) => set({ selectedGroupId: id }),

  patchComposite: (p) => set((s) => ({ ...s, ...p })),

  runComposite: async () => {
    const st = get();
    if (st.selectedGroupId === null) return;
    try {
      const { jobId } = await api.hdr.composite(st.selectedGroupId, {
        deghost: st.deghost,
        outputFormat: st.outputFormat,
        autoAlign: st.autoAlign,
      });
      set({ hdrJobId: jobId });
      set({ hdrResultPhotoId: null });
      useJobStore.getState().toast(`HDR 合成已启动（任务 ${jobId}）· 完成后自动入库`, 'info');
    } catch (err) {
      useJobStore.getState().toast(`HDR 合成失败：${errText(err)}`, 'error', 10000);
    }
  },

  openHdrResult: async () => {
    const photoId = get().hdrResultPhotoId;
    if (photoId === null) return;
    const { useDevelop } = await import('../../../store/developStore');
    const { useUi } = await import('../../../store/uiStore');
    await useDevelop.getState().openPhoto(photoId);
    useUi.getState().navigate('develop');
  },

  selectStackGroup: (id) => set({ selectedStackGroupId: id }),

  patchStackComposite: (p) => set((s) => ({ ...s, ...p })),

  runStackComposite: async () => {
    const st = get();
    if (st.selectedStackGroupId === null) return;
    try {
      const { jobId } = await api.stack.composite(st.selectedStackGroupId, {
        autoAlign: st.stackAutoAlign,
        outputFormat: st.stackOutputFormat,
        smoothness: st.stackSmoothness,
      });
      set({ stackJobId: jobId });
      set({ stackResultPhotoId: null });
      useJobStore.getState().toast(`焦点堆栈合成已启动（任务 ${jobId}）· 完成后自动入库`, 'info');
    } catch (err) {
      useJobStore.getState().toast(`焦点堆栈合成失败：${errText(err)}`, 'error', 10000);
    }
  },

  openStackResult: async () => {
    const photoId = get().stackResultPhotoId;
    if (photoId === null) return;
    const { useDevelop } = await import('../../../store/developStore');
    const { useUi } = await import('../../../store/uiStore');
    await useDevelop.getState().openPhoto(photoId);
    useUi.getState().navigate('develop');
  },

  setStrength: (v) => set({ strength: v }),

  setLuminanceMask: (v) => set({ luminanceMask: v }),

  refreshDenoisePreview: async () => {
    const st = get();
    if (st.photoId === null) return;
    set({ denoisePreviewLoading: true });
    try {
      // 演示观察窗：取画面中央 512×512 区域
      const res = await api.denoise.preview(st.photoId, st.strength, { x: 0.25, y: 0.25, w: 0.5, h: 0.5 });
      set({ denoisedPath: res.path, denoisePreviewLoading: false });
    } catch (err) {
      set({ denoisePreviewLoading: false });
      useJobStore.getState().toast(`降噪预览失败：${errText(err)}`, 'error', 8000);
    }
  },

  applyDenoise: async () => {
    const st = get();
    if (st.photoId === null) return;
    try {
      const { jobId } = await api.denoise.apply(st.photoId, st.strength, st.luminanceMask);
      set({ denoiseJobId: jobId });
      useJobStore.getState().toast(`AI 降噪已启动（任务 ${jobId}）· 完成后生成 DNG 衍生件`, 'info');
    } catch (err) {
      useJobStore.getState().toast(`AI 降噪失败：${errText(err)}`, 'error', 10000);
    }
  },
}));

/** HDR 合成完成事件绑定（load 时绑定一次）：result.photoId 即合成产物（引擎已入图库索引） */
let hdrEventsBound = false;
function bindHdrEvents(): void {
  if (hdrEventsBound) return;
  hdrEventsBound = true;
  rpc.on('event.job.completed', (params) => {
    const e = params as { jobId: number; kind: string; state: string; result?: { photoId?: number } };
    if (e.state !== 'done' || e.jobId !== useLandscape.getState().hdrJobId) return;
    const photoId = e.result?.photoId;
    if (typeof photoId === 'number' && photoId > 0) {
      useLandscape.setState({ hdrResultPhotoId: photoId });
      useJobStore.getState().toast(`HDR 合成完成 · 产物（photoId=${photoId}）已入图库，可在修图台打开`, 'success', 8000);
      // 刷新图库元数据，让新产物立即可见
      void import('../../../store/libraryStore').then(({ useLibrary }) => useLibrary.getState().loadMeta());
    } else {
      // 零静默：完成但无产物 id 属于异常返回，显式提示
      useJobStore.getState().toast('HDR 合成任务完成，但引擎未返回产物照片 id，请检查引擎日志', 'warn', 10000);
    }
  });
}

/** 焦点堆栈合成完成事件绑定（load 时绑定一次）：result.photoId 即合成产物（引擎已入图库索引） */
let stackEventsBound = false;
function bindStackEvents(): void {
  if (stackEventsBound) return;
  stackEventsBound = true;
  rpc.on('event.job.completed', (params) => {
    const e = params as { jobId: number; kind: string; state: string; result?: { photoId?: number } };
    if (e.state !== 'done' || e.jobId !== useLandscape.getState().stackJobId) return;
    const photoId = e.result?.photoId;
    if (typeof photoId === 'number' && photoId > 0) {
      useLandscape.setState({ stackResultPhotoId: photoId });
      useJobStore.getState().toast(`焦点堆栈合成完成 · 产物（photoId=${photoId}）已入图库，可在修图台打开`, 'success', 8000);
      // 刷新图库元数据，让新产物立即可见
      void import('../../../store/libraryStore').then(({ useLibrary }) => useLibrary.getState().loadMeta());
    } else {
      // 零静默：完成但无产物 id 属于异常返回，显式提示
      useJobStore.getState().toast('焦点堆栈合成任务完成，但引擎未返回产物照片 id，请检查引擎日志', 'warn', 10000);
    }
  });
}
