/**
 * 胶片条（92px 高，帧 01）：横向缩略，选中态与网格联动。
 * 手动窗口化：按 scrollLeft 计算可视切片，避免全量渲染。
 */

import { useEffect, useRef, useState } from 'react';
import type { PhotoSummary } from '../../api/protocol';
import { fileUrl } from '../../api/rpc-client';
import { useLibrary } from '../../store/libraryStore';
import { formatCount } from '../../utils/format';

const ITEM_W = 64;
const GAP = 4;

export default function Filmstrip({ items }: { items: PhotoSummary[] }) {
  const containerRef = useRef<HTMLDivElement>(null);
  const currentPhotoId = useLibrary((s) => s.currentPhotoId);
  const setCurrent = useLibrary((s) => s.setCurrent);
  const thumbUrls = useLibrary((s) => s.thumbUrls);
  const [range, setRange] = useState({ start: 0, end: 60 });

  const onScroll = () => {
    const el = containerRef.current;
    if (!el) return;
    const start = Math.max(0, Math.floor(el.scrollLeft / (ITEM_W + GAP)) - 10);
    const visible = Math.ceil(el.clientWidth / (ITEM_W + GAP)) + 20;
    setRange({ start, end: start + visible });
  };

  useEffect(onScroll, [items.length]);

  // 选中项滚动到可视区
  useEffect(() => {
    if (currentPhotoId === null) return;
    const idx = items.findIndex((p) => p.id === currentPhotoId);
    const el = containerRef.current;
    if (idx >= 0 && el) {
      const x = idx * (ITEM_W + GAP);
      if (x < el.scrollLeft || x > el.scrollLeft + el.clientWidth - ITEM_W) {
        el.scrollTo({ left: Math.max(0, x - el.clientWidth / 2), behavior: 'smooth' });
      }
    }
  }, [currentPhotoId, items]);

  const slice = items.slice(range.start, range.end);

  return (
    <div className="shrink-0 border-t border-line flex flex-col" style={{ background: 'var(--Lumina-Tokens-panel)', height: 92 }}>
      <div className="flex items-center px-2 pt-1 text-2xs text-txt3">
        <span>胶片条</span>
        {currentPhotoId !== null && <span className="ml-2 num">#{currentPhotoId}</span>}
        <div className="flex-1" />
        <span className="num">{formatCount(items.length)} 帧（当前筛选已加载）</span>
      </div>
      <div ref={containerRef} className="flex-1 overflow-x-auto overflow-y-hidden px-2" onScroll={onScroll}>
        <div className="relative h-full" style={{ width: items.length * (ITEM_W + GAP) }}>
          <div className="absolute top-0 flex h-full items-center" style={{ left: range.start * (ITEM_W + GAP) }}>
            {slice.map((p) => {
              const src = p.thumbPath ? fileUrl(p.thumbPath) : thumbUrls[p.id] ? fileUrl(thumbUrls[p.id]) : null;
              const active = p.id === currentPhotoId;
              return (
                <div
                  key={p.id}
                  onClick={() => setCurrent(p.id)}
                  className={`shrink-0 rounded-sm overflow-hidden cursor-pointer border ${active ? 'border-accent' : 'border-transparent hover:border-line'}`}
                  style={{ width: ITEM_W, height: 56, marginRight: GAP, background: 'var(--Lumina-Tokens-card)' }}
                >
                  {src ? <img src={src} alt={p.fileName} draggable={false} className="w-full h-full object-cover" /> : <div className="w-full h-full" style={{ background: 'var(--Lumina-Tokens-hover)' }} />}
                </div>
              );
            })}
          </div>
        </div>
      </div>
    </div>
  );
}
