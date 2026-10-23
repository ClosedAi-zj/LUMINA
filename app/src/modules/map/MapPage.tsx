/**
 * 地图主页：左地点集群列表 + 中央 SVG 抽象底图（恒 #08090B 观察区）+
 * 右地点详情面板 + 底部拍摄时间轴（区间筛选联动散点）。
 * 不发起任何网络请求：全部数据走引擎 RPC（mock 或真实引擎）。
 */

import { useEffect, useMemo } from 'react';
import { useMap, visibleLocations } from './store/mapStore';
import { Button, Spinner } from '../../components/primitives';
import { EmptyState } from '../../components/shell';
import MapCanvas from './MapCanvas';
import LocationClusterList from './LocationClusterList';
import LocationDetailPanel from './LocationDetailPanel';
import LocationTimeline from './LocationTimeline';
import UnlocatedPanel from './UnlocatedPanel';

export default function MapPage() {
  const loading = useMap((s) => s.loading);
  const loadError = useMap((s) => s.loadError);
  const locations = useMap((s) => s.locations);
  const detailsById = useMap((s) => s.detailsById);
  const selectedId = useMap((s) => s.selectedId);
  const range = useMap((s) => s.range);
  const favoriteOnly = useMap((s) => s.favoriteOnly);
  const timeline = useMap((s) => s.timeline);
  const unlocatedOpen = useMap((s) => s.unlocatedOpen);

  useEffect(() => {
    void useMap.getState().load();
  }, []);

  const visible = useMemo(
    () => visibleLocations(locations, detailsById, range, favoriteOnly),
    [locations, detailsById, range, favoriteOnly],
  );

  if (loading && locations.length === 0) {
    return (
      <div className="h-full flex items-center justify-center">
        <Spinner size={22} />
      </div>
    );
  }

  if (loadError) {
    return (
      <div className="h-full flex items-center justify-center">
        <EmptyState
          icon={<span className="text-danger">⚠</span>}
          title="地图数据加载失败"
          desc={`${loadError}（地图模块不联网：请确认引擎 map.* 接口可用）`}
          action={<Button onClick={() => void useMap.getState().load()}>重试</Button>}
        />
      </div>
    );
  }

  return (
    <div className="h-full flex flex-col">
      <div className="flex-1 min-h-0 flex relative">
        <LocationClusterList />
        <div className="flex-1 min-w-0 flex flex-col">
          <MapCanvas
            locations={visible}
            selectedId={selectedId}
            onSelect={(id) => useMap.getState().select(id)}
          />
          <LocationTimeline timeline={timeline} onRangeChange={(r) => useMap.getState().setRange(r)} />
        </div>
        <LocationDetailPanel />
        {unlocatedOpen && <UnlocatedPanel />}
      </div>
    </div>
  );
}
