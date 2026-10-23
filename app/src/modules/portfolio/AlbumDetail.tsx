/**
 * 画册详情（对照 03 号设计稿中部+右栏）：封面横幅（名称/张数）+ 成员说明 +
 * 右侧交付设置卡（客户/交付说明/发布到 Web 画廊）+ 挑选照片叠加层
 * （HTML5 拖拽排序 = 成员顺序，albums.addPhotos 按序落库）。
 */

import { usePortfolio } from './store/portfolioStore';
import { useUi } from '../../store/uiStore';
import { Button, Spinner } from '../../components/primitives';
import { fileUrl } from '../../api/rpc-client';
import { formatCount } from '../../utils/format';

export default function AlbumDetail() {
  const albums = usePortfolio((s) => s.albums);
  const selectedId = usePortfolio((s) => s.selectedId);
  const thumbUrls = usePortfolio((s) => s.thumbUrls);
  const members = usePortfolio((s) => s.members);
  const membersLoading = usePortfolio((s) => s.membersLoading);
  const pickerOpen = usePortfolio((s) => s.pickerOpen);
  const pickerCandidates = usePortfolio((s) => s.pickerCandidates);
  const pickerLoading = usePortfolio((s) => s.pickerLoading);
  const pickedIds = usePortfolio((s) => s.pickedIds);
  const adding = usePortfolio((s) => s.adding);
  const navigate = useUi((s) => s.navigate);

  const album = albums.find((a) => a.id === selectedId) ?? null;
  const cover = album && album.coverPhotoId > 0 ? thumbUrls[album.coverPhotoId] : null;

  if (!album) {
    return (
      <section className="flex-1 min-w-0 flex items-center justify-center" style={{ background: 'var(--Lumina-Tokens-canvas)' }}>
        <div className="text-2xs text-txt3">选择或新建一个画册</div>
      </section>
    );
  }

  return (
    <section className="relative flex-1 min-w-0 flex flex-col overflow-hidden" style={{ background: 'var(--Lumina-Tokens-canvas)' }}>
      {/* 封面横幅 */}
      <div className="shrink-0 relative h-44 overflow-hidden border-b border-line" style={{ background: 'var(--Lumina-Tokens-viewer)' }}>
        {cover && <img src={fileUrl(cover)} alt={album.name} draggable={false} className="absolute inset-0 w-full h-full object-cover opacity-80" />}
        <div className="absolute inset-0 flex flex-col justify-end p-4" style={{ background: 'linear-gradient(to top, var(--Lumina-Tokens-viewerOverlay), transparent 60%)' }}>
          <div className="text-lg font-semibold text-white" style={{ textShadow: '0 1px 4px rgba(0,0,0,0.8)' }}>{album.name}</div>
          <div className="num text-2xs text-txt2 mt-0.5">
            {album.kind === 'portfolio' ? '作品集' : '集合'} · {formatCount(album.itemCount)} 张{album.published ? ' · 已发布' : ''}
          </div>
        </div>
        {album.published && (
          <span className="absolute top-3 left-3 text-2xs px-1.5 py-0.5 rounded-sm font-medium" style={{ background: 'var(--Lumina-Tokens-success)', color: 'var(--Lumina-Tokens-onAccent)' }}>
            已发布
          </span>
        )}
      </div>

      {/* 成员区（albums.getItems 回显，按 position 顺序；悬停可移除） */}
      <div className="flex-1 min-h-0 overflow-y-auto p-4">
        <div className="flex items-center gap-3 mb-2.5">
          <div className="text-xs font-medium">画册成员</div>
          <span className="num text-2xs text-txt3">{members.length} 张 · 顺序即画册内顺序</span>
          <div className="flex-1" />
          <Button size="sm" variant="primary" onClick={() => void usePortfolio.getState().openPicker()}>
            挑选照片（{pickerOpen ? '进行中' : '开始'}）
          </Button>
          <Button size="sm" onClick={() => navigate('gallery', { albumId: album.id })}>
            发布到 Web 画廊
          </Button>
        </div>
        {membersLoading && (
          <div className="flex items-center justify-center py-8">
            <Spinner size={18} />
          </div>
        )}
        {!membersLoading && members.length === 0 && (
          <div className="rounded-lg border border-line p-3 max-w-2xl text-2xs text-txt3 leading-5" style={{ background: 'var(--Lumina-Tokens-panel)' }}>
            还没有成员。点击「挑选照片」→ 多选 → 拖拽排序 → 按序加入（加入顺序即画册内顺序）。
          </div>
        )}
        {!membersLoading && members.length > 0 && (
          <div className="flex flex-wrap gap-2">
            {members.map((pid, i) => {
              const src = thumbUrls[pid] ? fileUrl(thumbUrls[pid]) : null;
              return (
                <div
                  key={pid}
                  className="group relative w-28 aspect-[3/2] rounded-md overflow-hidden border border-line"
                  style={{ background: 'var(--Lumina-Tokens-card)' }}
                  title={`第 ${i + 1} 张 · photoId=${pid}`}
                >
                  {src ? (
                    <img src={src} alt={`成员 ${i + 1}`} draggable={false} className="w-full h-full object-cover" loading="lazy" />
                  ) : (
                    <div className="w-full h-full" style={{ background: 'var(--Lumina-Tokens-hover)' }} />
                  )}
                  <span className="absolute top-1 left-1 w-5 h-5 rounded-full bg-accent text-onaccent num text-2xs font-bold flex items-center justify-center">
                    {i + 1}
                  </span>
                  <button
                    onClick={() => void usePortfolio.getState().removeMember(pid)}
                    className="absolute top-1 right-1 w-5 h-5 rounded-full hidden group-hover:flex items-center justify-center text-2xs"
                    style={{ background: 'var(--Lumina-Tokens-viewerOverlay)', color: 'var(--Lumina-Tokens-viewerAccent)' }}
                    title="从画册移除（不影响图库照片）"
                  >
                    ×
                  </button>
                </div>
              );
            })}
          </div>
        )}
      </div>

      {/* 挑选照片叠加层 */}
      {pickerOpen && (
        <div className="absolute inset-0 z-40 flex flex-col" style={{ background: 'var(--Lumina-Tokens-canvas)' }}>
          <div className="flex items-center gap-3 px-3 h-10 border-b border-line shrink-0" style={{ background: 'var(--Lumina-Tokens-panel)' }}>
            <span className="text-xs font-medium">挑选照片 · 拖拽缩略图排序（顺序 = 画册内顺序）</span>
            <span className="num text-2xs text-accent">已选 {pickedIds.length}</span>
            <div className="flex-1" />
            <Button size="sm" onClick={() => usePortfolio.getState().closePicker()}>取消</Button>
            <Button size="sm" variant="primary" disabled={pickedIds.length === 0 || adding} onClick={() => void usePortfolio.getState().addPicked()}>
              {adding ? '加入中…' : `按此顺序加入（${pickedIds.length}）`}
            </Button>
          </div>
          <div className="flex-1 min-h-0 overflow-y-auto p-3">
            {pickerLoading && (
              <div className="flex items-center justify-center py-10">
                <Spinner size={20} />
              </div>
            )}
            <div className="grid grid-cols-6 gap-2">
              {pickerCandidates.map((p) => {
                const idx = pickedIds.indexOf(p.id);
                const src = p.thumbPath ? fileUrl(p.thumbPath) : thumbUrls[p.id] ? fileUrl(thumbUrls[p.id]) : null;
                return (
                  <div
                    key={p.id}
                    draggable
                    onDragStart={(e) => e.dataTransfer.setData('text/lumina-photo', String(p.id))}
                    onDragOver={(e) => e.preventDefault()}
                    onDrop={(e) => {
                      e.preventDefault();
                      const dragId = Number(e.dataTransfer.getData('text/lumina-photo'));
                      if (dragId) usePortfolio.getState().movePick(dragId, p.id);
                    }}
                    onClick={() => usePortfolio.getState().togglePick(p.id)}
                    className={`relative aspect-[3/2] rounded-md overflow-hidden cursor-pointer border-2 transition-colors ${
                      idx >= 0 ? 'border-accent' : 'border-transparent hover:border-line'
                    }`}
                    style={{ background: 'var(--Lumina-Tokens-card)' }}
                  >
                    {src ? (
                      <img src={src} alt={p.fileName} draggable={false} className="w-full h-full object-cover" loading="lazy" />
                    ) : (
                      <div className="w-full h-full" style={{ background: 'var(--Lumina-Tokens-hover)' }} />
                    )}
                    {idx >= 0 && (
                      <span className="absolute top-1 left-1 w-5 h-5 rounded-full bg-accent text-onaccent num text-2xs font-bold flex items-center justify-center">
                        {idx + 1}
                      </span>
                    )}
                  </div>
                );
              })}
            </div>
            {!pickerLoading && pickerCandidates.length === 0 && (
              <div className="py-10 text-center text-2xs text-txt3">图库中没有「精选（P）」照片 · 先在图库或精选模式打旗标</div>
            )}
          </div>
        </div>
      )}
    </section>
  );
}
