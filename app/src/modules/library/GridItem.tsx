/**
 * 网格单元徽标体系（帧 01 / M1 规格 §1.2）：
 * RW2 徽标（RAW 系格式左下）· 编辑点（右上）· GPS 图钉 · 评分星（≥1 才显）
 * · 选中环 · 淘汰态叠加 · 缺失红框 · 视频标记。
 */

import { useLibrary } from '../../store/libraryStore';
import { useDevelop } from '../../store/developStore';
import { useUi } from '../../store/uiStore';
import { fileUrl } from '../../api/rpc-client';
import type { PhotoSummary } from '../../api/protocol';
import { Icon } from '../../components/shell';

const RAW_FORMATS = new Set(['RW2', 'DNG', 'ARW']);

export default function GridItem({ photo }: { photo: PhotoSummary }) {
  const currentPhotoId = useLibrary((s) => s.currentPhotoId);
  const selectedIds = useLibrary((s) => s.selectedIds);
  const thumbUrls = useLibrary((s) => s.thumbUrls);
  const src = photo.thumbPath ? fileUrl(photo.thumbPath) : thumbUrls[photo.id] ? fileUrl(thumbUrls[photo.id]) : null;
  const selected = selectedIds.includes(photo.id);
  const isCurrent = currentPhotoId === photo.id;

  const onClick = (e: React.MouseEvent) => {
    e.stopPropagation();
    const st = useLibrary.getState();
    st.select(photo.id, e.ctrlKey || e.metaKey ? 'toggle' : e.shiftKey ? 'range' : 'set');
  };
  const onDoubleClick = () => {
    void useDevelop.getState().openPhoto(photo.id);
    useUi.getState().navigate('develop');
  };

  return (
    <div
      data-photo-cell={photo.id}
      onClick={onClick}
      onDoubleClick={onDoubleClick}
      className={`relative w-full h-full rounded-md overflow-hidden cursor-pointer border transition-shadow ${
        photo.isMissing ? 'border-danger' : selected ? 'border-accent' : 'border-transparent hover:border-line'
      }`}
      style={{
        outline: isCurrent ? '2px solid var(--Lumina-Tokens-accent)' : undefined,
        outlineOffset: isCurrent ? 1 : undefined,
        background: 'var(--Lumina-Tokens-card)',
      }}
    >
      {src ? (
        <img src={src} alt={photo.fileName} draggable={false} className="w-full h-full object-cover" loading="lazy" />
      ) : (
        <div
          className="w-full h-full flex items-center justify-center"
          style={{ background: 'linear-gradient(135deg, var(--Lumina-Tokens-card) 0%, var(--Lumina-Tokens-hover) 100%)' }}
        >
          <span className="text-txt3 text-2xs">{photo.format}</span>
        </div>
      )}

      {/* 淘汰态叠加（rejectOverlay 令牌 50%） */}
      {photo.pick === 'reject' && <div className="absolute inset-0" style={{ background: 'var(--Lumina-Tokens-rejectOverlay)' }} />}

      {/* 编辑点（sidecar crs: 非空） */}
      {photo.hasEdits && (
        <span className="absolute top-1 right-1 w-2 h-2 rounded-full" title="已编辑（XMP）" style={{ background: 'var(--Lumina-Tokens-accent)' }} />
      )}

      {/* pick 旗标 */}
      {photo.pick === 'pick' && (
        <span className="absolute top-1 left-1 text-2xs leading-none px-1 rounded-sm bg-accent text-onaccent font-bold">P</span>
      )}

      {/* GPS 图钉 */}
      {photo.hasGps && (
        <span className="absolute bottom-1 right-1 text-accent" title="GPS 已定位">
          <Icon name="pin" size={12} />
        </span>
      )}

      {/* RAW 徽标（左下） */}
      {RAW_FORMATS.has(photo.format) && (
        <span className="absolute bottom-1 left-1 text-2xs leading-none px-1 py-0.5 rounded-sm num" style={{ background: 'rgba(12,13,16,0.72)', color: 'var(--Lumina-Tokens-textSecondary)' }}>
          {photo.format}
        </span>
      )}

      {/* 视频标记 */}
      {photo.isVideo && (
        <span className="absolute top-1 left-1 px-1 rounded-sm text-2xs" style={{ background: 'rgba(12,13,16,0.72)', color: 'var(--Lumina-Tokens-textPrimary)' }}>
          ▶
        </span>
      )}

      {/* 评分星（rating≥1 才显） */}
      {photo.rating >= 1 && (
        <span className="absolute bottom-1 left-1/2 -translate-x-1/2 flex items-center gap-0.5 px-1 rounded-sm" style={{ background: 'rgba(12,13,16,0.72)' }}>
          {Array.from({ length: photo.rating }).map((_, i) => (
            <Icon key={i} name="star" size={8} className="text-accent" />
          ))}
        </span>
      )}

      {/* 缺失标记 */}
      {photo.isMissing && (
        <span className="absolute top-1 right-4 text-2xs px-1 rounded-sm font-medium" style={{ background: 'var(--Lumina-Tokens-danger)', color: '#fff' }}>
          缺失
        </span>
      )}
    </div>
  );
}
