/**
 * 精选决策工具条（顶部）：筛选分段 / 自动晋级开关 / 进度与统计 /
 * 评级按钮组（1-5 / P / X / Z / Space），与键盘流共用同一 store action。
 */

import { useMemo } from 'react';
import { useCulling, filteredPhotos } from './store/cullingStore';
import { Button, Segmented, Toggle } from '../../components/primitives';
import { Icon } from '../../components/shell';

export default function CullingToolbar() {
  const photos = useCulling((s) => s.photos);
  const filterMode = useCulling((s) => s.filterMode);
  const index = useCulling((s) => s.currentIndex);
  const autoAdvance = useCulling((s) => s.autoAdvance);
  const zoomed = useCulling((s) => s.zoomed);
  const compare = useCulling((s) => s.compare);

  const list = useMemo(() => filteredPhotos(photos, filterMode), [photos, filterMode]);
  const stats = useMemo(
    () => ({
      picked: photos.filter((p) => p.pick === 'pick').length,
      rejected: photos.filter((p) => p.pick === 'reject').length,
      rated: photos.filter((p) => p.rating > 0).length,
    }),
    [photos],
  );
  const current = list[index] ?? null;

  return (
    <div className="shrink-0 h-11 flex items-center gap-3 px-3 border-b border-line" style={{ background: 'var(--Lumina-Tokens-browserBar)' }}>
      <span className="text-xs font-medium shrink-0">精选模式</span>

      <Segmented
        value={filterMode}
        onChange={(v) => useCulling.getState().setFilterMode(v)}
        options={[
          { value: 'all', label: `全部 ${photos.length}` },
          { value: 'unrated', label: `未评级 ${photos.length - stats.rated}` },
          { value: 'picked', label: `精选 ${stats.picked}` },
          { value: 'rejected', label: `淘汰 ${stats.rejected}` },
        ]}
      />

      <Toggle checked={autoAdvance} onChange={() => useCulling.getState().toggleAutoAdvance()} label="打分后自动晋级下一张" />

      <div className="flex-1" />

      {/* 进度统计 */}
      <span className="num text-2xs text-txt3 shrink-0">
        第 <span className="text-txt">{list.length > 0 ? index + 1 : 0}</span> / {list.length} 张
      </span>
      <span className="num text-2xs text-accent shrink-0">已选 {stats.picked}</span>
      <span className="num text-2xs shrink-0" style={{ color: 'var(--Lumina-Tokens-danger)' }}>
        淘汰 {stats.rejected}
      </span>

      <div className="w-px h-5 shrink-0" style={{ background: 'var(--Lumina-Tokens-border)' }} />

      {/* 评级决策组 */}
      <div className="flex items-center gap-0.5 shrink-0">
        {[1, 2, 3, 4, 5].map((n) => (
          <button
            key={n}
            onClick={() => useCulling.getState().rate(n as 1 | 2 | 3 | 4 | 5)}
            title={`${n} 星（快捷键 ${n}）`}
            className={`w-6 h-6 rounded-sm flex items-center justify-center transition-colors ${
              current && current.rating >= n ? 'text-accent' : 'text-txt3 hover:text-txt2 hover:bg-hover'
            }`}
          >
            <Icon name="star" size={13} />
          </button>
        ))}
      </div>
      <Button
        size="sm"
        variant={current?.pick === 'pick' ? 'primary' : 'default'}
        onClick={() => useCulling.getState().togglePick()}
        title="设为精选（快捷键 P，再按取消）"
      >
        精选 P
      </Button>
      <Button
        size="sm"
        variant={current?.pick === 'reject' ? 'danger' : 'default'}
        onClick={() => useCulling.getState().toggleReject()}
        title="标记淘汰（快捷键 X，再按取消）"
      >
        淘汰 X
      </Button>
      <Button size="sm" variant={zoomed ? 'primary' : 'default'} onClick={() => useCulling.getState().toggleZoom()} title="1:1 放大（快捷键 Z）">
        放大 Z
      </Button>
      <Button size="sm" variant={compare ? 'primary' : 'default'} onClick={() => useCulling.getState().toggleCompare()} title="与上一张快速对比（快捷键 Space）">
        对比 Space
      </Button>
    </div>
  );
}
