/**
 * 精选大图观察区（ViewerSurface 恒色 #08090B · 红线 R5）：
 * object-contain 浏览 / Z 键 1:1 放大（内部滚动）/ Space 快速对比上一张。
 */

import { useEffect, useMemo } from 'react';
import { useCulling, filteredPhotos } from './store/cullingStore';
import { fileUrl } from '../../api/rpc-client';
import { Icon, EmptyState } from '../../components/shell';
import { Spinner } from '../../components/primitives';

export default function CullingViewer() {
  const photos = useCulling((s) => s.photos);
  const filterMode = useCulling((s) => s.filterMode);
  const index = useCulling((s) => s.currentIndex);
  const zoomed = useCulling((s) => s.zoomed);
  const compare = useCulling((s) => s.compare);
  const previewUrls = useCulling((s) => s.previewUrls);
  const thumbUrls = useCulling((s) => s.thumbUrls);

  const list = useMemo(() => filteredPhotos(photos, filterMode), [photos, filterMode]);
  const photo = list[index] ?? null;
  const prevPhoto = list[index - 1] ?? null;

  const pid = photo?.id ?? null;
  useEffect(() => {
    if (pid !== null) void useCulling.getState().ensurePreview(pid);
  }, [pid]);

  const preview = photo ? previewUrls[photo.id] : undefined;
  const src = preview?.path ?? (photo ? thumbUrls[photo.id] : undefined);
  const prevPreview = prevPhoto ? previewUrls[prevPhoto.id] : undefined;
  const prevSrc = prevPreview?.path ?? (prevPhoto ? thumbUrls[prevPhoto.id] : undefined);

  if (!photo) {
    return (
      <div className="flex-1 min-h-0 flex items-center justify-center" style={{ background: 'var(--Lumina-Tokens-viewer)' }}>
        <EmptyState title="队列为空" desc="当前筛选下没有候选照片，切换筛选或返回图库调整范围。" />
      </div>
    );
  }

  const starRow = (
    <span className="inline-flex items-center gap-0.5">
      {Array.from({ length: photo.rating }).map((_, i) => (
        <Icon key={i} name="star" size={11} className="text-accent" />
      ))}
      {photo.rating === 0 && <span className="text-2xs text-txt3">未评分</span>}
    </span>
  );

  const image = (p: string) =>
    zoomed && preview ? (
      <img src={fileUrl(p)} alt={photo.fileName} draggable={false} className="max-w-none block" style={{ width: preview.width, height: preview.height }} />
    ) : (
      <img src={fileUrl(p)} alt={photo.fileName} draggable={false} className="w-full h-full object-contain" />
    );

  return (
    <div className="relative flex-1 min-h-0 overflow-hidden" style={{ background: 'var(--Lumina-Tokens-viewer)' }}>
      {src ? (
        zoomed ? (
          <div className="w-full h-full overflow-auto">{image(src)}</div>
        ) : compare && prevSrc ? (
          /* Space 快速对比：当前张 / 上一张 各占一半 */
          <div className="w-full h-full flex">
            <div className="w-1/2 h-full flex items-center justify-center p-2">
              {comparePane(src, photo.fileName, '当前')}
            </div>
            <div className="w-0.5 shrink-0 my-4" style={{ background: 'var(--Lumina-Tokens-border)' }} />
            <div className="w-1/2 h-full flex items-center justify-center p-2">
              {comparePane(prevSrc, prevPhoto?.fileName ?? '', '上一张')}
            </div>
          </div>
        ) : (
          image(src)
        )
      ) : (
        <div className="w-full h-full flex items-center justify-center">
          <Spinner size={22} />
        </div>
      )}

      {/* 左下：文件名 / EXIF 摘要 */}
      <div className="absolute left-3 bottom-3 flex flex-col gap-1 pointer-events-none">
        <span className="num text-2xs text-txt2" style={{ textShadow: '0 1px 3px rgba(0,0,0,0.8)' }}>
          {photo.fileName}
        </span>
        <span className="text-2xs text-txt3 num" style={{ textShadow: '0 1px 3px rgba(0,0,0,0.8)' }}>
          {photo.camera} · ISO {photo.iso} · {photo.aperture} · {photo.shutter} · {photo.focalLength}
        </span>
      </div>

      {/* 右下：评级 / 决策徽标 */}
      <div className="absolute right-3 bottom-3 flex items-center gap-2 pointer-events-none">
        {starRow}
        {photo.pick === 'pick' && (
          <span className="text-2xs leading-none px-1.5 py-0.5 rounded-sm bg-accent text-onaccent font-bold">精选</span>
        )}
        {photo.pick === 'reject' && (
          <span className="text-2xs leading-none px-1.5 py-0.5 rounded-sm font-bold text-white" style={{ background: 'var(--Lumina-Tokens-danger)' }}>
            淘汰
          </span>
        )}
      </div>

      {/* 右上：序号 + 视图状态 */}
      <div className="absolute right-3 top-3 flex items-center gap-2 num text-2xs text-txt2 pointer-events-none" style={{ textShadow: '0 1px 3px rgba(0,0,0,0.8)' }}>
        {zoomed && <span className="px-1.5 py-0.5 rounded-sm bg-card border border-line">1:1 · 滚轮/滚动条查看</span>}
        {compare && <span className="px-1.5 py-0.5 rounded-sm bg-card border border-line text-accent">对比上一张</span>}
        <span>
          {index + 1} / {list.length}
        </span>
      </div>

      {/* 淘汰态叠加 */}
      {photo.pick === 'reject' && <div className="absolute inset-0 pointer-events-none" style={{ background: 'var(--Lumina-Tokens-rejectOverlay)' }} />}
    </div>
  );
}

function comparePane(src: string, name: string, tag: string) {
  return (
    <div className="relative w-full h-full flex items-center justify-center">
      <img src={fileUrl(src)} alt={name} draggable={false} className="max-w-full max-h-full object-contain" />
      <span className="absolute top-1 left-1 text-2xs px-1.5 py-0.5 rounded-sm bg-card border border-line text-txt2">{tag}</span>
    </div>
  );
}
