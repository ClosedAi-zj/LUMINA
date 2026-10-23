/**
 * 发布向导（对照 04 号设计稿的管理侧流程）：选集 → 标题/客户 → 密码（可选）→ 发布。
 * 成功后展示可复制预览链接 + 有效期；重复发布（6001）等错误显式提示。
 */

import { useGallery } from './store/galleryStore';
import { Button, Spinner } from '../../components/primitives';
import { Dialog } from '../../components/shell';
import { formatCount, formatDateTime } from '../../utils/format';

export default function PublishWizard() {
  const wizardOpen = useGallery((s) => s.wizardOpen);
  const albums = useGallery((s) => s.albums);
  const albumId = useGallery((s) => s.albumId);
  const title = useGallery((s) => s.title);
  const customerName = useGallery((s) => s.customerName);
  const password = useGallery((s) => s.password);
  const expiryDays = useGallery((s) => s.expiryDays);
  const publishing = useGallery((s) => s.publishing);
  const lastResult = useGallery((s) => s.lastResult);

  const canPublish = albumId !== null && title.trim() !== '' && customerName.trim() !== '' && !publishing;
  const selectedAlbum = albums.find((a) => a.id === albumId) ?? null;

  const copyLink = async (url: string) => {
    try {
      await navigator.clipboard.writeText(url);
      safeToast('链接已复制到剪贴板');
    } catch {
      safeToast('复制失败 · 请手动选择链接文本');
    }
  };

  return (
    <Dialog open={wizardOpen} title="发布到 Web 画廊" onClose={() => useGallery.getState().closeWizard()} width={460}>
      {/* 成功态：可复制链接 */}
      {lastResult ? (
        <div className="flex flex-col gap-3">
          <div className="text-xs" style={{ color: 'var(--Lumina-Tokens-success)' }}>发布成功</div>
          <div className="rounded-md border border-line p-2.5" style={{ background: 'var(--Lumina-Tokens-card)' }}>
            <div className="text-2xs text-txt3 mb-1">客户预览链接（浏览器打开）</div>
            <div className="num text-2xs text-accent break-all select-text">{lastResult.previewUrl}</div>
          </div>
          <div className="num text-2xs text-txt3">
            有效期至 {formatDateTime(lastResult.expiresAt)} · 客户可标记 favorites 并回传反馈
          </div>
          <div className="flex justify-end gap-2">
            <Button size="sm" onClick={() => void copyLink(lastResult.previewUrl)}>复制链接</Button>
            <Button size="sm" variant="primary" onClick={() => useGallery.getState().closeWizard()}>完成</Button>
          </div>
        </div>
      ) : (
        <div className="flex flex-col gap-3">
          {/* ① 选集 */}
          <div>
            <div className="text-2xs text-txt2 mb-1">① 选择画册</div>
            {albums.length === 0 && <div className="text-2xs text-txt3 py-1">暂无画册 · 先在作品集模块创建</div>}
            <div className="flex flex-col gap-1 max-h-32 overflow-y-auto">
              {albums.map((a) => (
                <button
                  key={a.id}
                  onClick={() => useGallery.getState().patchWizard({ albumId: a.id })}
                  className={`flex items-center gap-2 h-8 px-2.5 rounded-md border text-left transition-colors ${
                    a.id === albumId ? 'border-accent' : 'border-line hover:border-txt3'
                  }`}
                  style={{ background: a.id === albumId ? 'var(--Lumina-Tokens-accentSoft)' : 'var(--Lumina-Tokens-card)' }}
                >
                  <span className="text-2xs text-txt2 truncate flex-1">{a.name}</span>
                  <span className="num text-2xs text-txt3">{formatCount(a.itemCount)} 张</span>
                  {a.published && <span className="text-2xs" style={{ color: 'var(--Lumina-Tokens-success)' }}>已发布</span>}
                </button>
              ))}
            </div>
          </div>

          {/* ② 标题 / 客户 */}
          <div className="grid grid-cols-2 gap-2">
            <div>
              <div className="text-2xs text-txt2 mb-1">② 画廊标题</div>
              <input
                value={title}
                onChange={(e) => useGallery.getState().patchWizard({ title: e.target.value })}
                placeholder="如：川西 · 秋色"
                className="w-full h-8 px-2.5 rounded-md border border-line bg-card text-xs focus:outline-none focus:border-accent"
              />
            </div>
            <div>
              <div className="text-2xs text-txt2 mb-1">客户名称</div>
              <input
                value={customerName}
                onChange={(e) => useGallery.getState().patchWizard({ customerName: e.target.value })}
                placeholder="如：王女士"
                className="w-full h-8 px-2.5 rounded-md border border-line bg-card text-xs focus:outline-none focus:border-accent"
              />
            </div>
          </div>

          {/* ③ 密码 / 有效期 */}
          <div className="grid grid-cols-2 gap-2 items-end">
            <div>
              <div className="text-2xs text-txt2 mb-1">③ 访问密码（可选）</div>
              <input
                type="password"
                value={password}
                onChange={(e) => useGallery.getState().patchWizard({ password: e.target.value })}
                placeholder="留空 = 凭链接访问"
                className="w-full h-8 px-2.5 rounded-md border border-line bg-card text-xs focus:outline-none focus:border-accent"
              />
            </div>
            <div>
              <div className="text-2xs text-txt2 mb-1">④ 有效期（天）</div>
              <div className="flex gap-1">
                {[7, 30, 90, 365].map((d) => (
                  <button
                    key={d}
                    onClick={() => useGallery.getState().patchWizard({ expiryDays: d })}
                    className={`h-8 flex-1 rounded-md border text-2xs transition-colors ${
                      expiryDays === d ? 'border-accent text-accent' : 'border-line text-txt3 hover:text-txt2 hover:bg-hover'
                    }`}
                    style={{ background: expiryDays === d ? 'var(--Lumina-Tokens-accentSoft)' : 'var(--Lumina-Tokens-card)' }}
                  >
                    {d} 天
                  </button>
                ))}
              </div>
            </div>
          </div>

          {selectedAlbum && selectedAlbum.published && (
            <div className="text-2xs" style={{ color: 'var(--Lumina-Tokens-accent)' }}>
              注意：该画册已发布过 · 重复发布将被拒绝（需先撤销）
            </div>
          )}

          <div className="flex justify-end gap-2 mt-1">
            <Button onClick={() => useGallery.getState().closeWizard()}>取消</Button>
            <Button variant="primary" disabled={!canPublish} onClick={() => void useGallery.getState().publish()}>
              {publishing ? (
                <>
                  <Spinner size={12} /> 发布中…
                </>
              ) : (
                '发布'
              )}
            </Button>
          </div>
        </div>
      )}
    </Dialog>
  );
}

/** 组件内 toast 捷径（动态导入避免环） */
function safeToast(text: string) {
  void import('../../store/jobStore').then(({ useJobStore }) => useJobStore.getState().toast(text, 'info'));
}
