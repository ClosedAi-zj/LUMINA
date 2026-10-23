/**
 * 左侧作品集/集合列表 + 新建对话框入口（对照 03 号设计稿左栏）。
 */

import { useState } from 'react';
import { usePortfolio } from './store/portfolioStore';
import { useUi } from '../../store/uiStore';
import { Button, Spinner } from '../../components/primitives';
import { Dialog, Icon } from '../../components/shell';
import { fileUrl } from '../../api/rpc-client';
import { formatCount } from '../../utils/format';

export default function AlbumList() {
  const albums = usePortfolio((s) => s.albums);
  const loading = usePortfolio((s) => s.loading);
  const loadError = usePortfolio((s) => s.loadError);
  const selectedId = usePortfolio((s) => s.selectedId);
  const thumbUrls = usePortfolio((s) => s.thumbUrls);
  const navigate = useUi((s) => s.navigate);
  const [createOpen, setCreateOpen] = useState(false);
  const [name, setName] = useState('');
  const [kind, setKind] = useState<'portfolio' | 'collection'>('portfolio');

  return (
    <aside className="shrink-0 w-[236px] border-r border-line flex flex-col overflow-hidden" style={{ background: 'var(--Lumina-Tokens-panel)' }}>
      <div className="flex items-center h-9 px-3 border-b border-line shrink-0">
        <span className="text-2xs text-txt2 font-medium">画册</span>
        <div className="flex-1" />
        <span className="num text-2xs text-txt3">{albums.length}</span>
      </div>

      {loading && (
        <div className="flex items-center justify-center py-8">
          <Spinner size={16} />
        </div>
      )}
      {!loading && loadError && (
        <div className="mx-2 mt-2 rounded-md border p-2.5 text-2xs leading-5" style={{ borderColor: 'var(--Lumina-Tokens-danger)' }}>
          <span className="font-medium" style={{ color: 'var(--Lumina-Tokens-danger)' }}>画册接口不可用</span>
          <div className="text-txt3 mt-0.5">{loadError}</div>
          <button className="mt-1 text-accent hover:opacity-80" onClick={() => void usePortfolio.getState().load()}>重试</button>
        </div>
      )}

      <div className="flex-1 min-h-0 overflow-y-auto p-2 flex flex-col gap-1">
        {albums.map((a) => {
          const active = a.id === selectedId;
          const cover = a.coverPhotoId > 0 ? thumbUrls[a.coverPhotoId] : null;
          return (
            <button
              key={a.id}
              onClick={() => usePortfolio.getState().select(a.id)}
              className={`flex items-center gap-2 rounded-md border p-1.5 text-left transition-colors ${active ? 'border-accent' : 'border-transparent hover:border-line'}`}
              style={{ background: active ? 'var(--Lumina-Tokens-accentSoft)' : 'transparent' }}
            >
              <span className="w-9 h-7 rounded-sm overflow-hidden shrink-0" style={{ background: 'var(--Lumina-Tokens-card)' }}>
                {cover ? <img src={fileUrl(cover)} alt="" draggable={false} className="w-full h-full object-cover" /> : null}
              </span>
              <span className="min-w-0 flex-1">
                <span className={`block text-2xs truncate ${active ? 'text-txt font-medium' : 'text-txt2'}`}>{a.name}</span>
                <span className="num block text-2xs text-txt3">
                  {formatCount(a.itemCount)} 张{a.published ? ' · 已发布' : ''}
                </span>
              </span>
              {a.published && <span className="w-1.5 h-1.5 rounded-full shrink-0" style={{ background: 'var(--Lumina-Tokens-success)' }} />}
            </button>
          );
        })}
        <button
          onClick={() => setCreateOpen(true)}
          className="flex items-center gap-1.5 text-2xs text-txt3 hover:text-txt2 px-1.5 py-1.5 rounded-md hover:bg-hover"
        >
          <Icon name="plus" size={12} /> 新建画册
        </button>
      </div>

      <div className="shrink-0 border-t border-line px-3 py-2.5">
        <div className="text-2xs text-txt3 mb-1">客户交付</div>
        <button className="flex items-center gap-1.5 text-2xs text-accent hover:opacity-80" onClick={() => navigate('gallery')}>
          <Icon name="forward" size={12} /> 打开画廊交付
        </button>
      </div>

      <Dialog open={createOpen} title="新建画册" onClose={() => setCreateOpen(false)}>
        <input
          autoFocus
          value={name}
          onChange={(e) => setName(e.target.value)}
          placeholder="名称，如：川西 · 秋色"
          className="w-full h-8 px-2.5 rounded-md border border-line bg-card text-xs focus:outline-none focus:border-accent"
        />
        <div className="flex items-center gap-2 mt-3">
          {(['portfolio', 'collection'] as const).map((k) => (
            <button
              key={k}
              onClick={() => setKind(k)}
              className={`h-7 px-3 rounded-md border text-2xs transition-colors ${kind === k ? 'border-accent text-accent' : 'border-line text-txt3 hover:text-txt2'}`}
              style={{ background: kind === k ? 'var(--Lumina-Tokens-accentSoft)' : 'var(--Lumina-Tokens-card)' }}
            >
              {k === 'portfolio' ? '作品集' : '集合'}
            </button>
          ))}
        </div>
        <div className="flex justify-end gap-2 mt-4">
          <Button onClick={() => setCreateOpen(false)}>取消</Button>
          <Button
            variant="primary"
            disabled={!name.trim()}
            onClick={() => {
              void usePortfolio.getState().createAlbum(name.trim(), kind);
              setCreateOpen(false);
              setName('');
            }}
          >
            创建
          </Button>
        </div>
      </Dialog>
    </aside>
  );
}
