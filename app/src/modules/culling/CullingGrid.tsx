/**
 * 精选左侧候选网格（可折叠）：窗口化渲染当前张 ± 邻域，点击跳转。
 * 折叠后收成细条，给观察区让位（1280 宽度预算）。
 */

import { useEffect, useMemo } from 'react';
import { useCulling, filteredPhotos } from './store/cullingStore';
import { fileUrl } from '../../api/rpc-client';
import { Icon } from '../../components/shell';

/** 当前张前后的窗口半径：共渲染 WINDOW*2 个缩略 */
const WINDOW = 80;

export default function CullingGrid({ collapsed, onToggle }: { collapsed: boolean; onToggle: () => void }) {
  const photos = useCulling((s) => s.photos);
  const filterMode = useCulling((s) => s.filterMode);
  const index = useCulling((s) => s.currentIndex);
  const thumbUrls = useCulling((s) => s.thumbUrls);

  const list = useMemo(() => filteredPhotos(photos, filterMode), [photos, filterMode]);
  const start = Math.max(0, index - WINDOW);
  const end = Math.min(list.length, index + WINDOW);
  const visible = list.slice(start, end);

  useEffect(() => {
    if (collapsed || visible.length === 0) return;
    void useCulling.getState().ensureThumbs(visible.map((p) => p.id), 320);
  }, [collapsed, visible]);

  if (collapsed) {
    return (
      <aside className="shrink-0 w-9 border-r border-line flex flex-col items-center py-2 gap-2" style={{ background: 'var(--Lumina-Tokens-panel)' }}>
        <button className="text-txt3 hover:text-txt" onClick={onToggle} title="展开候选网格" aria-label="展开候选网格">
          <Icon name="forward" size={14} />
        </button>
        <span className="text-2xs text-txt3 num" style={{ writingMode: 'vertical-rl' }}>
          候选 {list.length}
        </span>
      </aside>
    );
  }

  return (
    <aside className="shrink-0 w-60 border-r border-line flex flex-col overflow-hidden" style={{ background: 'var(--Lumina-Tokens-panel)' }}>
      <div className="flex items-center h-9 px-2.5 border-b border-line shrink-0">
        <span className="text-2xs text-txt2 font-medium">候选队列</span>
        <span className="num text-2xs text-txt3 ml-1.5">{list.length}</span>
        <div className="flex-1" />
        <button className="text-txt3 hover:text-txt" onClick={onToggle} title="折叠候选网格" aria-label="折叠候选网格">
          <Icon name="back" size={14} />
        </button>
      </div>
      <div className="flex-1 min-h-0 overflow-y-auto p-1.5">
        <div className="grid grid-cols-3 gap-1.5">
          {visible.map((p, i) => {
            const listIdx = start + i;
            const src = p.thumbPath ? fileUrl(p.thumbPath) : thumbUrls[p.id] ? fileUrl(thumbUrls[p.id]) : null;
            const active = listIdx === index;
            return (
              <div
                key={p.id}
                onClick={() => useCulling.getState().setCurrentIndex(listIdx)}
                className={`relative aspect-[3/2] rounded-sm overflow-hidden cursor-pointer border transition-colors ${
                  active ? 'border-accent' : p.pick === 'reject' ? 'border-line' : 'border-transparent hover:border-line'
                }`}
                style={{ background: 'var(--Lumina-Tokens-card)' }}
                title={`#${listIdx + 1} ${p.fileName}`}
              >
                {src ? (
                  <img src={src} alt={p.fileName} draggable={false} className="w-full h-full object-cover" loading="lazy" />
                ) : (
                  <div className="w-full h-full" style={{ background: 'var(--Lumina-Tokens-hover)' }} />
                )}
                {p.pick === 'reject' && <div className="absolute inset-0" style={{ background: 'var(--Lumina-Tokens-rejectOverlay)' }} />}
                {active && <div className="absolute inset-0 border-2 pointer-events-none" style={{ borderColor: 'var(--Lumina-Tokens-accent)' }} />}
                {p.pick === 'pick' && (
                  <span className="absolute top-0.5 left-0.5 text-2xs leading-none px-0.5 rounded-sm bg-accent text-onaccent font-bold">P</span>
                )}
                {p.rating >= 1 && (
                  <span className="absolute bottom-0.5 right-0.5 num text-2xs leading-none px-0.5 rounded-sm" style={{ background: 'rgba(12,13,16,0.72)', color: 'var(--Lumina-Tokens-accent)' }}>
                    {p.rating}★
                  </span>
                )}
              </div>
            );
          })}
        </div>
        {list.length === 0 && <div className="py-10 text-center text-2xs text-txt3">当前筛选无候选</div>}
      </div>
    </aside>
  );
}
