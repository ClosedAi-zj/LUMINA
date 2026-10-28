/**
 * 幻灯片放映的图片缓存（独立于 libraryStore）。
 *
 * 为什么不复用 libraryStore.ensureThumbs：
 * 它的 pending 判据是 `!thumbsRequested.has(key) && !thumbUrls[id]`，thumbUrls 按 id 存。
 * 图库里一张图通常已经加载过 640，此时再按 1600 请求会被这条判据直接跳过，
 * 放映只会拿到 640 的路径，大屏上明显发虚。放映是全屏场景，必须独立缓存。
 *
 * 失败处理口径与其它 store 一致（零静默）：请求失败必须把「进行中」标记撤掉，
 * 否则这些 id 永远停在加载态，用户看到的只有转圈，没有任何原因。
 */

import { create } from 'zustand';
import { api } from '../../api/engine';
import { errText } from '../../api/rpc-client';
import type { ThumbSize } from '../../api/protocol';

/** 放映固定用 1600：足够铺满常见的大屏，又不至于为每张都解码原图而拖慢预取。 */
export const SLIDE_SIZE: ThumbSize = 1600;

interface SlideshowState {
  urls: Record<number, string>;
  /** id → 失败原因（零静默：任何失败都要能解释，不许只留空画面） */
  failures: Record<number, string>;
  requesting: Record<number, boolean>;
  ensure: (ids: number[]) => Promise<void>;
  retry: (id: number) => void;
}

export const useSlideshow = create<SlideshowState>((set, get) => ({
  urls: {},
  failures: {},
  requesting: {},

  ensure: async (ids) => {
    if (ids.length === 0) return;
    const { urls, requesting } = get();
    const pending = ids.filter((id) => !urls[id] && !requesting[id]);
    if (pending.length === 0) return;

    const nextRequesting = { ...requesting };
    for (const id of pending) nextRequesting[id] = true;
    set({ requesting: nextRequesting });

    try {
      const res = await api.thumb.ensureBatch(pending, SLIDE_SIZE);
      const nextUrls = { ...get().urls };
      const nextFailures = { ...get().failures };
      const done = { ...get().requesting };
      const returned = new Set(res.items.map((i) => i.id));
      for (const item of res.items) {
        if (item.state === 'ready' && item.path) {
          nextUrls[item.id] = item.path;
          delete nextFailures[item.id];
        } else {
          nextFailures[item.id] = item.state === 'ready' ? '引擎未返回预览路径' : `预览未就绪（${item.state}）`;
        }
      }
      // 引擎漏回的 id 也要落地一个原因：不交代就等于静默丢失某一张。
      for (const id of pending) {
        done[id] = false;
        if (!returned.has(id) && !nextFailures[id]) nextFailures[id] = '引擎响应中缺少该照片';
      }
      set({ urls: nextUrls, failures: nextFailures, requesting: done });
    } catch (err) {
      const done = { ...get().requesting };
      const nextFailures = { ...get().failures };
      const reason = errText(err);
      for (const id of pending) {
        done[id] = false;
        nextFailures[id] = reason;
      }
      set({ requesting: done, failures: nextFailures });
    }
  },

  retry: (id) => {
    const nextFailures = { ...get().failures };
    delete nextFailures[id];
    const urls = { ...get().urls };
    delete urls[id];
    set({ failures: nextFailures, urls });
    void get().ensure([id]);
  },
}));
