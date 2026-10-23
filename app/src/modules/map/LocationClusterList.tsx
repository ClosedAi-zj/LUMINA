/**
 * 左侧地点集群列表：按张数降序，点击选中联动地图与详情面板；
 * 底部「仅看收藏」开关 + 未定位照片计数。
 */

import { useMemo } from 'react';
import { useMap, visibleLocations } from './store/mapStore';
import { Toggle } from '../../components/primitives';
import { formatCount } from '../../utils/format';

export default function LocationClusterList() {
  const locations = useMap((s) => s.locations);
  const detailsById = useMap((s) => s.detailsById);
  const selectedId = useMap((s) => s.selectedId);
  const range = useMap((s) => s.range);
  const favoriteOnly = useMap((s) => s.favoriteOnly);
  const unlocatedCount = useMap((s) => s.unlocatedCount);

  const visible = useMemo(
    () => visibleLocations(locations, detailsById, range, favoriteOnly).sort((a, b) => b.count - a.count),
    [locations, detailsById, range, favoriteOnly],
  );

  return (
    <aside className="shrink-0 w-[288px] border-r border-line flex flex-col overflow-hidden" style={{ background: 'var(--Lumina-Tokens-panel)' }}>
      <div className="flex items-center h-9 px-3 border-b border-line shrink-0">
        <span className="text-2xs text-txt2 font-medium">地点</span>
        <div className="flex-1" />
        <span className="num text-2xs text-txt3">{visible.length}</span>
      </div>
      <div className="flex-1 min-h-0 overflow-y-auto py-1">
        {visible.length === 0 && <div className="py-8 text-center text-2xs text-txt3">当前筛选无地点</div>}
        {visible.map((l) => {
          const id = l.locationId ?? -1;
          const active = id === selectedId;
          const fav = l.locationId !== undefined && detailsById[l.locationId]?.favorite;
          return (
            <button
              key={id}
              onClick={() => useMap.getState().select(id)}
              className={`w-full flex items-center gap-2 px-3 h-9 text-xs text-left transition-colors ${
                active ? 'bg-hover text-txt' : 'text-txt2 hover:text-txt hover:bg-hover'
              }`}
              style={active ? { boxShadow: `inset 2px 0 0 var(--Lumina-Tokens-accent)` } : undefined}
            >
              {fav && <span className="text-accent shrink-0" title="收藏地点">♥</span>}
              <span className="truncate flex-1">{l.name}</span>
              <span className={`num text-2xs shrink-0 ${active ? 'text-accent' : 'text-txt3'}`}>{formatCount(l.count)}</span>
            </button>
          );
        })}
      </div>
      <div className="shrink-0 border-t border-line px-3 py-2.5 flex flex-col gap-2">
        <div className="flex items-center justify-between">
          <Toggle checked={favoriteOnly} onChange={() => useMap.getState().toggleFavoriteOnly()} label="仅看收藏" />
        </div>
        {/* 未定位入口：点击打开定位写入面板（setPhotoLocation / clearPhotoLocation） */}
        <button
          className="text-left text-2xs rounded-md px-1.5 py-1 -mx-1.5 transition-colors hover:bg-hover"
          title={unlocatedCount !== null && unlocatedCount > 0 ? '打开未定位列表，批量为照片指定 GPS 地点' : '查看未定位照片'}
          onClick={() => useMap.getState().openUnlocated()}
        >
          <span className="inline-flex items-center gap-1">
            <span style={{ color: unlocatedCount !== null && unlocatedCount > 0 ? 'var(--Lumina-Tokens-accent)' : undefined }}>◎ 未定位照片</span>
            <span className="num text-txt2">{unlocatedCount === null ? '…' : formatCount(unlocatedCount)}</span>
            张
            {unlocatedCount !== null && unlocatedCount > 0 && <span className="text-accent">→ 指定地点</span>}
          </span>
        </button>
      </div>
    </aside>
  );
}
