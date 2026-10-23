/**
 * 底部拍摄时间轴：按年月聚合（library.getTimeline），
 * 拖动选择区间 → 筛选地图散点（与各地点 timeSpan 求交）；双击复位。
 */

import { useMemo, useRef, useState } from 'react';
import { useMap } from './store/mapStore';
import type { TimelineBucket } from '../../api/protocol';
import { formatCount } from '../../utils/format';

interface Props {
  timeline: TimelineBucket[];
  onRangeChange: (range: { from: number; to: number } | null) => void;
}

function monthStart(ym: string): number {
  const [y, m] = ym.split('-').map(Number);
  return new Date(y, m - 1, 1).getTime();
}
function monthEnd(ym: string): number {
  const [y, m] = ym.split('-').map(Number);
  return new Date(y, m, 1).getTime();
}
function monthLabel(ym: string): string {
  const [y, m] = ym.split('-');
  return `${y}.${m}`;
}

export default function LocationTimeline({ timeline, onRangeChange }: Props) {
  const range = useMap((s) => s.range);
  const containerRef = useRef<HTMLDivElement>(null);
  const [drag, setDrag] = useState<{ from: number; to: number } | null>(null);
  const draggingRef = useRef(false);

  const months = useMemo(
    () =>
      timeline.map((t) => ({
        ym: t.month,
        count: t.count,
        start: monthStart(t.month),
        end: monthEnd(t.month),
      })),
    [timeline],
  );
  const maxCount = Math.max(1, ...months.map((m) => m.count));

  const active = drag ?? rangeIdx();
  function rangeIdx(): { from: number; to: number } | null {
    if (!range) return null;
    const from = months.findIndex((m) => m.end > range.from);
    const to = months.findIndex((m) => m.end >= range.to);
    if (from < 0) return null;
    return { from, to: to < 0 ? months.length - 1 : to };
  }

  const idxFromEvent = (clientX: number): number => {
    const el = containerRef.current;
    if (!el || months.length === 0) return 0;
    const rect = el.getBoundingClientRect();
    const ratio = Math.max(0, Math.min(0.999, (clientX - rect.left) / rect.width));
    return Math.floor(ratio * months.length);
  };

  const onPointerDown = (e: React.PointerEvent) => {
    if (months.length === 0) return;
    draggingRef.current = true;
    const i = idxFromEvent(e.clientX);
    setDrag({ from: i, to: i });
    (e.target as Element).setPointerCapture?.(e.pointerId);
  };
  const onPointerMove = (e: React.PointerEvent) => {
    if (!draggingRef.current) return;
    const i = idxFromEvent(e.clientX);
    setDrag((d) => (d ? { from: Math.min(d.from, i), to: Math.max(d.to, i) } : { from: i, to: i }));
  };
  const onPointerUp = () => {
    if (!draggingRef.current) return;
    draggingRef.current = false;
    if (drag) {
      const lo = Math.min(drag.from, drag.to);
      const hi = Math.max(drag.from, drag.to);
      onRangeChange({ from: months[lo].start, to: months[hi].end });
    }
    setDrag(null);
  };

  const onDoubleClick = () => {
    onRangeChange(null);
  };

  const rangeText = range
    ? `${monthLabel(months[active?.from ?? 0]?.ym ?? months[0]?.ym ?? '')} → ${monthLabel(months[active?.to ?? 0]?.ym ?? months[months.length - 1]?.ym ?? '')}`
    : '全部时间';

  return (
    <div className="shrink-0 border-t border-line flex flex-col" style={{ background: 'var(--Lumina-Tokens-panel)', height: 84 }}>
      <div className="flex items-center px-3 pt-1.5 text-2xs text-txt3 shrink-0">
        <span>拍摄时间轴</span>
        <span className="ml-2 num text-txt2">{rangeText}</span>
        {range && (
          <button className="ml-2 text-accent hover:opacity-80" onClick={() => onRangeChange(null)}>
            清除区间
          </button>
        )}
        <div className="flex-1" />
        <span className="text-2xs">拖动选择月份区间 · 双击复位</span>
      </div>
      <div
        ref={containerRef}
        className="flex-1 flex items-end gap-px px-3 pb-2 cursor-pointer touch-none"
        onPointerDown={onPointerDown}
        onPointerMove={onPointerMove}
        onPointerUp={onPointerUp}
        onDoubleClick={onDoubleClick}
      >
        {months.length === 0 && <div className="w-full text-center text-2xs text-txt3 pb-3">暂无时间数据</div>}
        {months.map((m, i) => {
          const inRange = active !== null && i >= active.from && i <= active.to;
          return (
            <div
              key={m.ym}
              className="flex-1 flex flex-col items-center justify-end gap-0.5 min-w-0"
              title={`${monthLabel(m.ym)} · ${formatCount(m.count)} 张`}
            >
              <div
                className={`w-full rounded-sm transition-colors ${inRange ? 'bg-accent' : 'bg-card'}`}
                style={{ height: `${Math.max(6, (m.count / maxCount) * 42)}px`, opacity: inRange ? 1 : 0.75 }}
              />
              {(i === 0 || i === months.length - 1 || i % 6 === 0) && (
                <span className={`num text-2xs leading-none ${inRange ? 'text-accent' : 'text-txt3'}`}>{monthLabel(m.ym)}</span>
              )}
            </div>
          );
        })}
      </div>
    </div>
  );
}
