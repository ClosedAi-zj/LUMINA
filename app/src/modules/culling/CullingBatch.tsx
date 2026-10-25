/**
 * 精选网格批量模式（阶段5 B1，对照 05-8/05-9）：
 * 主区 = 132px 网格卡片（图库卡片语言）+ 焦点高亮；右栏 = 焦点图放大审阅；
 * 底部状态条 = 晋级/拒绝/未判 计数。标记直接写 cullingStore（乐观更新 + 引擎落盘），
 * 与单张审阅子模式（CullingViewer 流水线）共享同一队列与决策动作。
 */

import { useEffect } from 'react';
import { useCulling, filteredPhotos } from './store/cullingStore';
import { fileUrl } from '../../api/rpc-client';
import { formatCount } from '../../utils/format';

export default function CullingBatch() {
  const photos = useCulling((s) => s.photos);
  const filterMode = useCulling((s) => s.filterMode);
  const currentIndex = useCulling((s) => s.currentIndex);
  const thumbUrls = useCulling((s) => s.thumbUrls);
  const previewUrls = useCulling((s) => s.previewUrls);
  const setCurrentIndex = useCulling((s) => s.setCurrentIndex);
  const ensureThumbs = useCulling((s) => s.ensureThumbs);
  const ensurePreview = useCulling((s) => s.ensurePreview);

  const list = filteredPhotos(photos, filterMode);
  const focused = list[currentIndex] ?? null;

  // 可视范围缩略图批量预取（简单全量 320，队列内部有 requested 去重）
  useEffect(() => {
    if (list.length > 0) void ensureThumbs(list.slice(0, 400).map((p) => p.id), 320);
  }, [list, ensureThumbs]);

  // 焦点图大图预取
  useEffect(() => {
    if (focused) void ensurePreview(focused.id);
  }, [focused, ensurePreview]);

  const picked = photos.filter((p) => p.pick === 'pick').length;
  const rejected = photos.filter((p) => p.pick === 'reject').length;
  const unjudged = photos.length - picked - rejected;

  return (
    <div className="flex-1 min-h-0 flex">
      {/* 主网格（132px 卡片语言） */}
      <div className="flex-1 min-w-0 overflow-y-auto p-2">
        <div className="grid gap-3" style={{ gridTemplateColumns: 'repeat(auto-fill, minmax(132px, 1fr))' }}>
          {list.map((p, i) => {
            const src = thumbUrls[p.id] ? fileUrl(thumbUrls[p.id]) : null;
            const focusedCard = i === currentIndex;
            return (
              <button
                key={p.id}
                onClick={() => setCurrentIndex(i)}
                className={`relative aspect-[3/2] rounded-mdC overflow-hidden text-left transition-colors ${
                  focusedCard ? 'ring-2 ring-accent' : 'ring-1 ring-line hover:ring-txt3'
                }`}
                style={{ background: 'var(--Lumina-Tokens-card)' }}
                title={p.fileName}
              >
                {src ? (
                  <img src={src} alt={p.fileName} className="w-full h-full object-cover" loading="lazy" draggable={false} />
                ) : (
                  <div className="w-full h-full" style={{ background: 'var(--Lumina-Tokens-hover)' }} />
                )}
                {/* 决策角标：P = 晋级（accent），X = 拒绝（danger），未判不标 */}
                {p.pick === 'pick' && (
                  <span className="absolute top-1 left-1 px-1 rounded-sm text-2xs font-bold bg-accent text-onaccent">P</span>
                )}
                {p.pick === 'reject' && (
                  <span className="absolute top-1 left-1 px-1 rounded-sm text-2xs font-bold" style={{ background: 'var(--Lumina-Tokens-danger)', color: 'var(--Lumina-Tokens-onAccent)' }}>
                    X
                  </span>
                )}
                {p.pick === 'reject' && <div className="absolute inset-0 pointer-events-none" style={{ background: 'var(--Lumina-Tokens-rejectOverlay)' }} />}
                {p.rating > 0 && (
                  <span className="absolute bottom-1 right-1 num text-2xs px-1 rounded-sm" style={{ background: 'var(--Lumina-Tokens-viewerOverlay)', color: 'var(--Lumina-Tokens-viewerAccent)' }}>
                    {'★'.repeat(p.rating)}
                  </span>
                )}
              </button>
            );
          })}
          {list.length === 0 && (
            <div className="col-span-full py-10 text-center text-2xs text-txt3">当前筛选下无候选照片</div>
          )}
        </div>
      </div>

      {/* 右栏：焦点图放大审阅 */}
      <aside className="w-[320px] shrink-0 border-l border-line flex flex-col overflow-hidden" style={{ background: 'var(--Lumina-Tokens-panel)' }}>
        <div className="flex items-center h-8 px-3 border-b border-line shrink-0">
          <span className="text-2xs text-txt2 font-medium">审阅</span>
          <div className="flex-1" />
          <span className="num text-2xs text-txt3">
            {list.length > 0 ? `${currentIndex + 1} / ${list.length}` : '—'}
          </span>
        </div>
        {focused ? (
          <>
            <div className="flex-1 min-h-0 flex items-center justify-center p-2" style={{ background: 'var(--Lumina-Tokens-viewer)' }}>
              {previewUrls[focused.id] ? (
                <img
                  src={fileUrl(previewUrls[focused.id].path)}
                  alt={focused.fileName}
                  className="max-w-full max-h-full object-contain"
                  draggable={false}
                />
              ) : (
                <span className="text-2xs text-txt3">解码预览…</span>
              )}
            </div>
            <div className="shrink-0 border-t border-line px-3 py-2 text-2xs flex flex-col gap-0.5">
              <span className="text-txt2 truncate num" title={focused.fileName}>{focused.fileName}</span>
              <span className="text-txt3">
                {focused.camera || '未知机身'} · {focused.lens || '—'}
              </span>
              <span className="num text-txt3">
                ISO {focused.iso || '—'} · {focused.shutter || '—'} · {focused.aperture || '—'}
              </span>
              <span className="text-txt3">
                评分 {focused.rating} · 旗标 {focused.pick === 'pick' ? '晋级' : focused.pick === 'reject' ? '拒绝' : '未判'}
              </span>
            </div>
          </>
        ) : (
          <div className="flex-1 flex items-center justify-center text-2xs text-txt3">无焦点照片</div>
        )}
      </aside>
    </div>
  );
}

/** 底部状态条（B1）：晋级 / 拒绝 / 未判 计数（全队列口径，不随筛选变化） */
export function CullingStatusBar() {
  const photos = useCulling((s) => s.photos);
  const picked = photos.filter((p) => p.pick === 'pick').length;
  const rejected = photos.filter((p) => p.pick === 'reject').length;
  const unjudged = photos.length - picked - rejected;
  return (
    <div className="h-8 shrink-0 border-t border-line px-3 flex items-center gap-4 text-2xs" style={{ background: 'var(--Lumina-Tokens-panel)' }}>
      <span className="num text-txt2">{formatCount(photos.length)} 张队列</span>
      <span className="num text-accent">晋级 {formatCount(picked)}</span>
      <span className="num" style={{ color: 'var(--Lumina-Tokens-danger)' }}>拒绝 {formatCount(rejected)}</span>
      <span className="num text-txt3">未判 {formatCount(unjudged)}</span>
    </div>
  );
}
