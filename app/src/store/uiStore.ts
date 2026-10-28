/** 全局界面导航 store（模块路由 + 跨模块跳转） */

import { create } from 'zustand';

export type AppView = 'library' | 'develop' | 'ingest' | 'health' | 'culling' | 'map' | 'timelapse' | 'landscape' | 'export' | 'portfolio' | 'gallery' | 'slideshow';

/** 冷启动阶段（WP-B3）：引擎握手 → 库加载 → 缩略图就绪 → ready */
export type BootStage = 'handshake' | 'library' | 'thumbs' | 'ready';

interface UiStore {
  view: AppView;
  /** 跨模块上下文：修图台 → 序列 seqId；导入完成 → 跳库刷新 */
  navigate: (v: AppView, ctx?: Record<string, unknown>) => void;
  context: Record<string, unknown>;
  /** 全局搜索聚焦请求（Ctrl+F） */
  searchFocusTick: number;
  requestSearchFocus: () => void;
}

export const useUi = create<UiStore>((set) => ({
  view: 'library',
  context: {},
  navigate: (v, ctx) => set({ view: v, context: ctx ?? {} }),
  searchFocusTick: 0,
  requestSearchFocus: () => set((s) => ({ searchFocusTick: s.searchFocusTick + 1 })),
}));
