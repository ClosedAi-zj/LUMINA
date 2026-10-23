/**
 * 精选底部胶片条（84px）：横向缩略 + 决策徽标（P/X/星级）+ 手动窗口化渲染。
 * 当前张自动滚动进可视区；点击直接跳转。
 */

import { useEffect, useMemo, useRef, useState } from 'react';
import { useCulling, filteredPhotos } from './store/cullingStore';
import { fileUrl } from '../../api/rpc-client';
import { Icon } from '../../components/shell';

const ITEM_W = 64;
const GAP = 4;

export default function CullingFilmstrip() {
  const photos = useCulling((s) => s.photos);
  const filterMode = useCulling((s) => s.filterMode);
  const index = useCulling((s) => s.currentIndex);
  const thumbUrls = useCulling((s) => s.thumbUrls);
  const containerRef = useRef<HTMLDivElement>(null);
  const [range, setRange] = useState({ start: 0, end: 60 });

  const list = useMemo(() => filteredPhotos(photos, filterMode), [photos, filterMode]);

  const onScroll = () => {
    const el = containerRef.current;
    if (!el) return;
    const start = Math.max(0, Math.floor(el.scrollLeft / (ITEM_W + GAP)) - 10);
    const visible = Math.ceil(el.clientWidth / (ITEM_W + GAP)) + 20;
    setRange({ start, end: start + visible });
  };

  useEffect(onScroll, [list.length]);

  // 当前张滚动到可视区
  useEffect(() => {
    const el = containerRef.current;
    if (!el || list.length === 0) return;
    const x = index * (ITEM_W + GAP);
    if (x < el.scrollLeft || x > el.scrollLeft + el.clientWidth - ITEM_W) {
      el.scrollTo({ left: Math.max(0, x - el.clientWidth / 2), behavior: 'smooth' });
    }
  }, [index, list.length]);

  // 可视切片缩略图预取
  useEffect(() => {
    const slice = list.slice(range.start, range.end);
    if (slice.length > 0) void useCulling.getState().ensureThumbs(slice.map((p) => p.id), 320);
  }, [range, list]);

  const visible = list.slice(range.start, range.end);
  const currentPhoto = list[index] ?? null;

  return (
    <div className="shrink-0 border-t border-line flex flex-col" style={{ background: 'var(--Lumina-Tokens-panel)', height: 84 }}>
      <div className="flex items-center px-2 pt-1 text-2xs text-txt3 shrink-0">
        <span>胶片条</span>
        {currentPhoto && <span className="ml-2 num truncate max-w-40">{currentPhoto.fileName}</span>}
        <div className="flex-1" />
        <span className="num">{list.length} 帧在队列</span>
      </div>
      <div ref={containerRef} className="flex-1 overflow-x-auto overflow-y-hidden px-2 pb-1" onScroll={onScroll}>
        <div className="relative h-full" style={{ width: Math.max(list.length, 1) * (ITEM_W + GAP) }}>
          <div className="absolute top-0 flex h-full items-center" style={{ left: range.start * (ITEM_W + GAP) }}>
            {visible.map((p, i) => {
              const listIdx = range.start + i;
              const src = p.thumbPath ? fileUrl(p.thumbPath) : thumbUrls[p.id] ? fileUrl(thumbUrls[p.id]) : null;
              const active = listIdx === index;
              return (
                <div
                  key={p.id}
                  onClick={() => useCulling.getState().setCurrentIndex(listIdx)}
                  className={`relative shrink-0 rounded-sm overflow-hidden cursor-pointer border transition-colors ${
                    active ? 'border-accent' : p.pick === 'reject' ? 'border-transparent hover:border-line' : 'border-transparent hover:border-line'
                  }`}
                  style={{ width: ITEM_W, height: 54, marginRight: GAP, background: 'var(--Lumina-Tokens-card)' }}
                  title={`#${listIdx + 1} ${p.fileName}`}
                >
                  {src ? (
                    <img src={src} alt={p.fileName} draggable={false} className="w-full h-full object-cover" loading="lazy" />
                  ) : (
                    <div className="w-full h-full" style={{ background: 'var(--Lumina-Tokens-hover)' }} />
                  )}
                  {p.pick === 'reject' && <div className="absolute inset-0" style={{ background: 'var(--Lumina-Tokens-rejectOverlay)' }} />}
                  {p.pick === 'pick' && (
                    <span className="absolute top-0.5 left-0.5 text-2xs leading-none px-0.5 rounded-sm bg-accent text-onaccent font-bold">P</span>
                  )}
                  {p.pick === 'reject' && (
                    <span className="absolute top-0.5 left-0.5 text-2xs leading-none px-0.5 rounded-sm font-bold text-white" style={{ background: 'var(--Lumina-Tokens-danger)' }}>
                      X
                    </span>
                  )}
                  {p.rating >= 1 && (
                    <span className="absolute bottom-0.5 left-1/2 -translate-x-1/2 flex items-center gap-px px-0.5 rounded-sm" style={{ background: 'rgba(12,13,16,0.72)' }}>
                      {Array.from({ length: p.rating }).map((_, k) => (
                        <Icon key={k} name="star" size={7} className="text-accent" />
                      ))}
                    </span>
                  )}
                </div>
              );
            })}
          </div>
        </div>
      </div>
    </div>
  );
}
