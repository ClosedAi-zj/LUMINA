/**
 * Web 客户画廊（管理侧）主页（对照 04 号设计稿的管理流程）：
 * 顶部概览 + 发布入口 + 已发布列表 + 发布向导。
 * 客户侧静态页由引擎 gallery 域产出，本页只做管理。
 */

import { useEffect } from 'react';
import { useGallery } from './store/galleryStore';
import { useUi } from '../../store/uiStore';
import { Button, Spinner } from '../../components/primitives';
import { Icon } from '../../components/shell';
import { formatCount } from '../../utils/format';
import PublishWizard from './PublishWizard';
import PublishedList from './PublishedList';
import GallerySettingsPanel from './GallerySettingsPanel';

export default function GalleryPage() {
  const loading = useGallery((s) => s.loading);
  const loadError = useGallery((s) => s.loadError);
  const published = useGallery((s) => s.published);
  const albums = useGallery((s) => s.albums);
  const navigate = useUi((s) => s.navigate);

  useEffect(() => {
    void useGallery.getState().load();
  }, []);

  const onlineCount = published.filter((g) => g.expiresAt > Date.now()).length;

  return (
    <div className="h-full overflow-y-auto p-5" style={{ background: 'var(--Lumina-Tokens-canvas)' }}>
      <div className="flex items-center gap-3 mb-4 max-w-4xl">
        <h2 className="text-base font-semibold">Web 画廊</h2>
        <span className="text-2xs text-txt3">客户选片 · 密码分享 · 反馈回传</span>
        <div className="flex-1" />
        <Button size="sm" onClick={() => navigate('portfolio')}>
          管理画册
        </Button>
        <Button variant="primary" size="sm" onClick={() => useGallery.getState().openWizard()}>
          <Icon name="plus" size={13} /> 发布到 Web 画廊
        </Button>
      </div>

      {loading && published.length === 0 && albums.length === 0 ? (
        <div className="flex items-center justify-center py-20">
          <Spinner size={22} />
        </div>
      ) : loadError ? (
        <div className="rounded-lg border p-4 max-w-4xl" style={{ borderColor: 'var(--Lumina-Tokens-danger)', background: 'var(--Lumina-Tokens-panel)' }}>
          <div className="text-xs font-medium mb-1" style={{ color: 'var(--Lumina-Tokens-danger)' }}>
            画廊接口不可用 · 功能已降级
          </div>
          <div className="text-2xs text-txt3 leading-5">{loadError}</div>
          <button className="mt-2 text-2xs text-accent hover:opacity-80" onClick={() => void useGallery.getState().load()}>
            重试
          </button>
        </div>
      ) : (
        <div className="max-w-4xl flex flex-col gap-4">
          {/* 概览 */}
          <div className="grid grid-cols-3 gap-3">
            <div className="rounded-lg border border-line p-3" style={{ background: 'var(--Lumina-Tokens-panel)' }}>
              <div className="text-2xs text-txt3 mb-1">画册总数</div>
              <div className="num text-xl font-semibold">{formatCount(albums.length)}</div>
            </div>
            <div className="rounded-lg border border-line p-3" style={{ background: 'var(--Lumina-Tokens-panel)' }}>
              <div className="text-2xs text-txt3 mb-1">在线画廊</div>
              <div className="num text-xl font-semibold text-accent">{formatCount(onlineCount)}</div>
            </div>
            <div className="rounded-lg border border-line p-3" style={{ background: 'var(--Lumina-Tokens-panel)' }}>
              <div className="text-2xs text-txt3 mb-1">客户收藏合计</div>
              <div className="num text-xl font-semibold">{formatCount(published.reduce((acc, g) => acc + g.stats.favorites, 0))}</div>
            </div>
          </div>

          <PublishedList />
          <GallerySettingsPanel />
        </div>
      )}

      <PublishWizard />
    </div>
  );
}
