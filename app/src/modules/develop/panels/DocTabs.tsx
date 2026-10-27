/**
 * G9 文档标签条（PS 式文档 Tab）——多开照片接力修图。
 *
 * 语义裁定（docs/v1.2.0/UI-对齐差距评估与改版清单.md §五-D1）：产品是「库 + 单图」
 * 模型，这里不是 PS 的「一个画布多文档」，而是**已打开的多个修图文档**：
 * 图库里多选几张 → 一起开进来 → Tab 切换接力修图。
 * 因此只有 1 个文档时不显示这条（单图场景它没有信息量，白占 28px 高度）。
 *
 * 切换走 developStore.openPhoto（内含 endSession{flush:true}），
 * 上一张的参数一定先落盘再切走 —— 与快捷键 Ctrl+' 切图同一条路径。
 */

import { useDevelop } from '../../../store/developStore';
import type { PhotoSummary } from '../../../api/protocol';

export default function DocTabs({ items }: { items: PhotoSummary[] }) {
  const tabs = useDevelop((s) => s.tabs);
  const photoId = useDevelop((s) => s.photoId);
  const loading = useDevelop((s) => s.loading);

  // 单文档不显示（见文件头语义裁定）
  if (tabs.length < 2) return null;

  const nameOf = (id: number): string => {
    const hit = items.find((p) => p.id === id);
    return hit?.fileName ?? `#${id}`;
  };

  return (
    <div
      className="h-7 shrink-0 flex items-center gap-0.5 px-1.5 border-b border-line overflow-x-auto scrollbar-none"
      style={{ background: 'var(--Lumina-Tokens-panel)' }}
      data-testid="doc-tabs"
      role="tablist"
      aria-label="已打开的修图文档"
    >
      {tabs.map((id) => {
        const active = id === photoId;
        const name = nameOf(id);
        return (
          <div
            key={id}
            role="tab"
            aria-selected={active}
            title={`${name}（#${id}）`}
            onClick={() => {
              if (active) return;
              void useDevelop.getState().openPhoto(id);
            }}
            className="group h-6 max-w-[176px] shrink-0 px-2 flex items-center gap-1.5 rounded-smC text-2xs cursor-pointer transition-colors"
            style={{
              background: active ? 'var(--Lumina-Tokens-hover)' : undefined,
              color: active ? 'var(--Lumina-Tokens-textPrimary)' : 'var(--Lumina-Tokens-textTertiary)',
              boxShadow: active ? 'inset 0 -2px 0 0 var(--Lumina-Tokens-accent)' : undefined,
            }}
          >
            <span className="truncate">{name}</span>
            <span
              role="button"
              aria-label={`关闭 ${name}`}
              title="关闭该文档（Ctrl+W）"
              className="shrink-0 w-3.5 h-3.5 flex items-center justify-center rounded-[2px] text-[10px] leading-none opacity-40 group-hover:opacity-80 hover:!opacity-100 hover:bg-hover"
              onClick={(e) => {
                e.stopPropagation();
                void useDevelop.getState().closeTab(id);
              }}
            >
              ×
            </span>
          </div>
        );
      })}
      {loading && <span className="ml-1 text-2xs text-txt3 shrink-0">打开中…</span>}
    </div>
  );
}
