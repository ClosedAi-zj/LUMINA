/**
 * 画廊设置（WP-B8 · 设计稿态）。
 *
 * 事实边界（UI 上就写清楚，不假装已落库）：
 *  - 有效期 expiryDays：引擎 gallery.publish 原生支持（1..3650），**真实生效**；
 *  - 允许下载 / 下载上限 / 允许留言：引擎侧目前没有存储位，这三项只存在本机
 *    localStorage，导出到客户页时不会带着走 —— 字段设计稿见
 *    docs/ui-1.1/水印与画廊协议缺口清单.md，等引擎排期后接上。
 */

import { useGallery } from './store/galleryStore';
import { Toggle } from '../../components/primitives';

export default function GallerySettingsPanel() {
  const settings = useGallery((s) => s.settings);
  const patch = useGallery.getState().patchSettings;

  return (
    <div className="rounded-lg border border-line p-3" style={{ background: 'var(--Lumina-Tokens-panel)' }}>
      <div className="flex items-center gap-2 mb-2">
        <span className="text-2xs text-txt2 font-medium">画廊设置</span>
        <span
          className="text-2xs px-1 py-0.5 rounded-sm"
          style={{ background: 'var(--Lumina-Tokens-hover)', color: 'var(--Lumina-Tokens-textMuted)' }}
        >
          本机暂存 · 部分待引擎支持
        </span>
      </div>

      <div className="mb-2.5">
        <div className="text-2xs text-txt2 mb-1">新发布默认有效期（引擎已支持 · 真实生效）</div>
        <div className="flex gap-1 w-52">
          {[7, 30, 90, 365].map((d) => (
            <button
              key={d}
              onClick={() => patch({ defaultExpiryDays: d })}
              className={`h-6 flex-1 rounded-sm border text-2xs transition-colors ${
                settings.defaultExpiryDays === d ? 'border-accent text-accent' : 'border-line text-txt3 hover:text-txt2 hover:bg-hover'
              }`}
              style={{ background: settings.defaultExpiryDays === d ? 'var(--Lumina-Tokens-accentSoft)' : 'var(--Lumina-Tokens-card)' }}
            >
              {d} 天
            </button>
          ))}
        </div>
      </div>

      <div className="flex flex-col gap-2">
        <Toggle
          checked={settings.allowDownloads}
          onChange={(v) => patch({ allowDownloads: v })}
          label="允许客户下载原图（待引擎支持 · 当前仅本机暂存）"
        />
        {settings.allowDownloads && (
          <div className="flex items-center gap-2">
            <span className="text-2xs text-txt3">下载上限</span>
            <div className="flex gap-1">
              {[0, 10, 30, 100].map((n) => (
                <button
                  key={n}
                  onClick={() => patch({ downloadLimit: n })}
                  className={`h-6 px-2 rounded-sm border text-2xs transition-colors ${
                    settings.downloadLimit === n ? 'border-accent text-accent' : 'border-line text-txt3 hover:text-txt2 hover:bg-hover'
                  }`}
                  style={{ background: settings.downloadLimit === n ? 'var(--Lumina-Tokens-accentSoft)' : 'var(--Lumina-Tokens-card)' }}
                >
                  {n === 0 ? '不限' : `${n} 张`}
                </button>
              ))}
            </div>
          </div>
        )}
        <Toggle
          checked={settings.allowComments}
          onChange={(v) => patch({ allowComments: v })}
          label="允许客户留言/评论（待引擎支持 · 当前仅本机暂存）"
        />
      </div>

      <div className="text-2xs text-txt3 mt-2 leading-5">
        保存方式：改动即写入本机 localStorage（lumina.gallery.settings），重开应用可回读；
        「有效期」是唯一会真正传给引擎的字段，其余三项等待引擎存储位后由主理人统一排期接入。
      </div>
    </div>
  );
}
