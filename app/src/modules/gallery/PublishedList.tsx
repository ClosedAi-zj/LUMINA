/**
 * 已发布画廊列表：客户链接（复制）· 反馈统计（张数/收藏/完成率）· 到期时间 · 撤销发布。
 */

import { useGallery } from './store/galleryStore';
import { Button } from '../../components/primitives';
import { formatCount, formatDateTime } from '../../utils/format';

export default function PublishedList() {
  const published = useGallery((s) => s.published);
  const albums = useGallery((s) => s.albums);

  const albumName = (albumId: number) => albums.find((a) => a.id === albumId)?.name ?? `画册 #${albumId}`;

  const copyLink = async (url: string) => {
    try {
      await navigator.clipboard.writeText(url);
      void import('../../store/jobStore').then(({ useJobStore }) => useJobStore.getState().toast('链接已复制到剪贴板', 'info'));
    } catch {
      void import('../../store/jobStore').then(({ useJobStore }) => useJobStore.getState().toast('复制失败 · 请手动选择链接', 'warn'));
    }
  };

  if (published.length === 0) {
    return (
      <div className="rounded-lg border border-line p-6 text-center" style={{ background: 'var(--Lumina-Tokens-panel)' }}>
        <div className="text-txt3 text-3xl mb-2">◎</div>
        <div className="text-xs text-txt2 mb-1">暂无已发布画廊</div>
        <div className="text-2xs text-txt3">通过「发布到 Web 画廊」向导，把画册变成客户的专属选片页</div>
      </div>
    );
  }

  return (
    <div className="rounded-lg border border-line overflow-hidden" style={{ background: 'var(--Lumina-Tokens-panel)' }}>
      <div className="flex items-center px-3 h-9 border-b border-line">
        <span className="text-2xs text-txt2 font-medium">已发布画廊</span>
        <div className="flex-1" />
        <span className="num text-2xs text-txt3">{published.length}</span>
      </div>
      {published.map((g) => {
        const expired = g.expiresAt < Date.now();
        return (
          <div key={`${g.albumId}_${g.token}`} className="flex items-center gap-3 px-3 py-2.5 border-b border-line last:border-b-0 hover:bg-hover">
            <div className="min-w-0 flex-1">
              <div className="flex items-center gap-2">
                <span className="text-xs text-txt truncate">{albumName(g.albumId)}</span>
                <span
                  className="text-2xs px-1 py-0.5 rounded-sm shrink-0"
                  style={expired ? { background: 'var(--Lumina-Tokens-hover)', color: 'var(--Lumina-Tokens-textMuted)' } : { background: 'var(--Lumina-Tokens-accentSoft)', color: 'var(--Lumina-Tokens-accent)' }}
                >
                  {expired ? '已过期' : '在线'}
                </span>
              </div>
              <div className="num text-2xs text-txt3 mt-0.5 truncate select-text" title={g.previewUrl}>
                {g.previewUrl}
              </div>
              <div className="num text-2xs text-txt3 mt-0.5">
                {formatCount(g.stats.photos)} 张 · 收藏 {formatCount(g.stats.favorites)} · 回传率 {Math.round(g.stats.completionRate * 100)}% · 有效期至 {formatDateTime(g.expiresAt)}
              </div>
              {/* WP-B8：下载计数与留言 引擎侧无存储位，这里只做占位并明确标注，不伪造数字 */}
              <div className="text-2xs text-txt3 mt-0.5">
                下载计数 · 客户评论：待引擎支持（字段设计稿见 docs/ui-1.1/水印与画廊协议缺口清单.md）
              </div>
            </div>
            <div className="flex items-center gap-1.5 shrink-0">
              <Button size="sm" onClick={() => void copyLink(g.previewUrl)}>复制链接</Button>
              <Button size="sm" variant="danger" onClick={() => void useGallery.getState().revoke(g.albumId)}>
                撤销发布
              </Button>
            </div>
          </div>
        );
      })}
    </div>
  );
}
