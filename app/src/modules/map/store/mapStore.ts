/**
 * 地图模块 store：地点集群（map.getLocations）、地点详情缓存、
 * 拍摄时间轴（library.getTimeline）与时间区间筛选、收藏、未定位计数。
 * 底图不联网：全部为引擎 RPC + 本地 SVG 投影（v1.0.0 设计决策）。
 */

import { create } from 'zustand';
import { api } from '../../../api/engine';
import { errText } from '../../../api/rpc-client';
import { useJobStore } from '../../../store/jobStore';
import type { LocationDetail, MapLocation, TimelineBucket } from '../../../api/protocol';

/** 时间区间（epoch ms） */
export interface TimeRange {
  from: number;
  to: number;
}

/** 定位写入结果上报（零静默红线：sidecarFailed>0 必须可见，不允许只报成功） */
export interface MapWriteReport {
  action: string;
  updated: number;
  sidecarFailed: number;
  warnings: string[];
  at: number;
}

interface MapStore {
  locations: MapLocation[];
  /** locationId → 详情（时间区间筛选与右侧详情面板共用） */
  detailsById: Record<number, LocationDetail>;
  timeline: TimelineBucket[];
  unlocatedCount: number | null;
  loading: boolean;
  /** 引擎侧不可用时的显式降级提示（零静默红线） */
  loadError: string | null;
  /** 引擎集群层级（map.getLocations 的 zoom 参数；>7 返回子散点） */
  zoom: number;
  selectedId: number | null;
  range: TimeRange | null;
  favoriteOnly: boolean;
  /** 未定位列表面板开关（定位写入入口） */
  unlocatedOpen: boolean;
  /** 最近一次定位写入的上报（面板内展开明细；null=无） */
  writeReport: MapWriteReport | null;
  /** 定位写入进行中（按钮禁用态） */
  assigning: boolean;

  load: () => Promise<void>;
  setZoom: (z: number) => void;
  select: (id: number | null) => void;
  setRange: (r: TimeRange | null) => void;
  toggleFavoriteOnly: () => void;
  toggleFavorite: () => Promise<void>;
  openUnlocated: () => void;
  closeUnlocated: () => void;
  /** 批量把照片指到某地点（写 sidecar exif: 命名空间，原片零写入） */
  assignLocation: (ids: number[], lat: number, lng: number, altitude?: number, locationName?: string) => Promise<boolean>;
  /** 清除某地点全部照片的定位（危险操作，调用方负责二次确认） */
  clearLocationPhotos: (locationId: number) => Promise<boolean>;
  clearWriteReport: () => void;
}

export const ZOOM_ENGINE_MIN = 4;
export const ZOOM_ENGINE_MAX = 12;

export const useMap = create<MapStore>((set, get) => ({
  locations: [],
  detailsById: {},
  timeline: [],
  unlocatedCount: null,
  loading: false,
  loadError: null,
  zoom: 6,
  selectedId: null,
  range: null,
  favoriteOnly: false,
  unlocatedOpen: false,
  writeReport: null,
  assigning: false,

  load: async () => {
    if (get().loading) return;
    set({ loading: true, loadError: null });
    try {
      const [locs, unlocated, timeline] = await Promise.all([
        api.map.getLocations(get().zoom),
        api.map.listUnlocated({ offset: 0, limit: 1 }).catch(() => null),
        api.library.getTimeline().catch(() => [] as TimelineBucket[]),
      ]);
      // 详情并发拉取（地点数量 = 拍摄地名去重数，个位数量级）；失败不阻断，仅缺时间筛选用途
      const detailsById: Record<number, LocationDetail> = {};
      await Promise.all(
        locs.map(async (l) => {
          if (l.locationId === undefined) return;
          try {
            detailsById[l.locationId] = await api.map.getLocationDetail(l.locationId);
          } catch {
            // 降级：无详情仍可显示集群点，仅时间筛选/收藏对它失效
          }
        }),
      );
      set({
        locations: locs,
        detailsById,
        timeline,
        unlocatedCount: unlocated ? unlocated.total : null,
        loading: false,
        loadError: null,
      });
    } catch (err) {
      set({ loading: false, loadError: errText(err) });
    }
  },

  setZoom: (z) => {
    const clamped = Math.max(ZOOM_ENGINE_MIN, Math.min(ZOOM_ENGINE_MAX, z));
    if (clamped === get().zoom) return;
    set({ zoom: clamped });
    // 层级变化影响引擎聚合（children 散点），重新拉取集群
    void (async () => {
      try {
        const locs = await api.map.getLocations(clamped);
        set({ locations: locs });
      } catch (err) {
        useJobStore.getState().toast(`地点集群刷新失败：${errText(err)}`, 'error', 6000);
      }
    })();
  },

  select: (id) => set({ selectedId: id }),

  setRange: (r) => set({ range: r }),

  toggleFavoriteOnly: () => set((s) => ({ favoriteOnly: !s.favoriteOnly })),

  toggleFavorite: async () => {
    const st = get();
    const id = st.selectedId;
    const detail = id !== null ? st.detailsById[id] : undefined;
    if (id === null || !detail) return;
    const next = !detail.favorite;
    // 乐观更新，失败回滚
    set((s) => ({ detailsById: { ...s.detailsById, [id]: { ...detail, favorite: next } } }));
    try {
      await api.map.toggleFavorite(id, next);
    } catch (err) {
      set((s) => ({ detailsById: { ...s.detailsById, [id]: { ...detail, favorite: !next } } }));
      useJobStore.getState().toast(`收藏切换失败：${errText(err)}`, 'error', 6000);
    }
  },

  openUnlocated: () => set({ unlocatedOpen: true }),

  closeUnlocated: () => set({ unlocatedOpen: false }),

  clearWriteReport: () => set({ writeReport: null }),

  assignLocation: async (ids, lat, lng, altitude, locationName) => {
    if (ids.length === 0) {
      useJobStore.getState().toast('请先选择要定位的照片', 'warn', 4000);
      return false;
    }
    if (!locationName || !locationName.trim()) {
      useJobStore.getState().toast('请填写地点名称', 'warn', 4000);
      return false;
    }
    set({ assigning: true });
    try {
      const res = await api.map.setPhotoLocation(ids, lat, lng, altitude, locationName.trim());
      reportWrite('指定地点', res);
      // 写库/写 sidecar 都会改变集群与未定位计数，整包刷新
      await get().load();
      set({ assigning: false });
      return res.sidecarFailed === 0;
    } catch (err) {
      set({ assigning: false });
      useJobStore.getState().toast(`定位写入失败：${errText(err)}`, 'error', 10000);
      return false;
    }
  },

  clearLocationPhotos: async (locationId) => {
    set({ assigning: true });
    try {
      // 影响面：按 locationIds 过滤分页取全量照片 id（引擎单页上限 500，循环取尽）
      const ids: number[] = [];
      for (let offset = 0; ; offset += 500) {
        const page = await api.library.queryPhotos({ locationIds: [locationId] }, { field: 'capturedAt', order: 'asc' }, { offset, limit: 500 });
        ids.push(...page.items.map((p) => p.id));
        if (ids.length >= page.total || page.items.length === 0) break;
      }
      if (ids.length === 0) {
        useJobStore.getState().toast('该地点下没有可清除定位的照片', 'warn', 5000);
        set({ assigning: false });
        return false;
      }
      const res = await api.map.clearPhotoLocation(ids);
      reportWrite('清除定位', res);
      await get().load();
      set({ assigning: false });
      return true;
    } catch (err) {
      set({ assigning: false });
      useJobStore.getState().toast(`清除定位失败：${errText(err)}`, 'error', 10000);
      return false;
    }
  },
}));

/**
 * 定位写入结果统一上报（零静默红线）：
 * - 全部成功 → success toast；
 * - sidecarFailed>0 → warn toast（定位可能只在库、未进 sidecar，换机即丢），
 *   并把 warnings[] 全量存入 writeReport 供面板展开明细，不允许任何一条被吞。
 */
function reportWrite(
  action: string,
  res: { updated: number; locationId?: number; sidecarFailed: number; warnings: string[] },
): void {
  const warnings = res.warnings ?? [];
  useMap.setState({ writeReport: { action, updated: res.updated, sidecarFailed: res.sidecarFailed, warnings, at: Date.now() } });
  const toast = useJobStore.getState().toast;
  if (res.sidecarFailed > 0) {
    toast(
      `${action}：${res.updated} 张已入库，但 ${res.sidecarFailed} 张未同步到 XMP sidecar（换机将丢失，请展开警告明细）`,
      'warn',
      12000,
    );
    for (const w of warnings) {
      toast(`sidecar 同步警告 · ${w}`, 'warn', 12000);
    }
  } else {
    toast(`${action}：已更新 ${res.updated} 张（GPS 已写入库与 XMP sidecar）`, 'success', 5000);
  }
}

/** 可见地点过滤：时间区间（与 detail.timeSpan 求交）+ 仅看收藏 */
export function visibleLocations(
  locations: MapLocation[],
  detailsById: Record<number, LocationDetail>,
  range: TimeRange | null,
  favoriteOnly: boolean,
): MapLocation[] {
  return locations.filter((l) => {
    if (l.locationId === undefined) return true;
    const d = detailsById[l.locationId];
    if (favoriteOnly && d && !d.favorite) return false;
    if (range && d) {
      // 区间相交判定：地点拍摄期与所选时间轴区间有重叠才显示
      if (d.timeSpan.to < range.from || d.timeSpan.from > range.to) return false;
    }
    return true;
  });
}
