/**
 * G1/G11（UI-GAP-1.0 M2b-1）：自绘全局菜单栏（PS 形态）。
 *
 * - 自绘而非 Electron 原生菜单：frame:false 无边框窗口的自定义标题栏内实现，不破坏拖拽区；
 *   hover 整项填色（顶栏按钮 + 下拉项整行）是 PS 特征，原生菜单做不到（R6 v2-B1 / 裁定 D3）。
 * - 只接线代码里已存在的真实功能（文件=导入/导出、编辑=undo/redo/复制粘贴设置、
 *   视图=面板显隐/前后对比/参考线、帮助=快捷键速查）；没有真实功能的菜单不出现，不放假控件。
 * - 视图/帮助仅在修图台（develop）出现：面板显隐、参考线、速查浮层都承载在修图台，
 *   其他视图下没有对应真实行为，宁可少一项也不挂空菜单。
 */

import { useEffect, useRef, useState } from 'react';
import { useUi } from '../store/uiStore';
import { useDevelop } from '../store/developStore';
import { useJobStore } from '../store/jobStore';
import { copyDevelopSettings, pasteDevelopSettings } from '../modules/develop/clipboard';

type MenuEntry =
  | { kind: 'item'; label: string; hint?: string; checked?: boolean; disabled?: boolean; action: () => void }
  | { kind: 'sep' };

interface MenuDef {
  label: string;
  entries: MenuEntry[];
}

/** 跨组件命令通道：菜单 → 修图台（面板显隐/速查浮层的真相源在 DevelopPage） */
function dispatchCmd(cmd: string): void {
  window.dispatchEvent(new CustomEvent('lumina:cmd', { detail: cmd }));
}

/** 前后对比（\）：ViewerCanvas 在 window keydown 链路上持有 compare 本地态，合成键走同一链路 */
function toggleCompare(): void {
  window.dispatchEvent(new KeyboardEvent('keydown', { key: '\\', bubbles: true }));
}

export function MenuBar() {
  const view = useUi((s) => s.view);
  const navigate = useUi((s) => s.navigate);
  const sessionId = useDevelop((s) => s.sessionId);
  const canUndo = useDevelop((s) => s.canUndo);
  const canRedo = useDevelop((s) => s.canRedo);
  const guides = useDevelop((s) => s.guides);
  const toast = useJobStore((s) => s.toast);
  const [openIdx, setOpenIdx] = useState<number | null>(null);
  /** 修图台面板可见性（勾选态），由 DevelopPage 广播（lumina:panels） */
  const [panels, setPanels] = useState({ left: true, right: true });
  const rootRef = useRef<HTMLDivElement>(null);

  useEffect(() => {
    const onPanels = (e: Event) => setPanels((e as CustomEvent<{ left: boolean; right: boolean }>).detail);
    window.addEventListener('lumina:panels', onPanels);
    return () => window.removeEventListener('lumina:panels', onPanels);
  }, []);

  // 菜单打开期间：点外部关闭；Esc 只关菜单（capture 阶段拦截，避免同时触发修图台「Esc 返回图库」）
  useEffect(() => {
    if (openIdx === null) return;
    const onPointerDown = (e: PointerEvent) => {
      if (!rootRef.current?.contains(e.target as Node)) setOpenIdx(null);
    };
    const onKey = (e: KeyboardEvent) => {
      if (e.key === 'Escape') {
        e.preventDefault();
        e.stopImmediatePropagation();
        setOpenIdx(null);
      }
    };
    document.addEventListener('pointerdown', onPointerDown);
    window.addEventListener('keydown', onKey, true);
    return () => {
      document.removeEventListener('pointerdown', onPointerDown);
      window.removeEventListener('keydown', onKey, true);
    };
  }, [openIdx]);

  const sessionLive = sessionId !== null;
  const inDevelop = view === 'develop';

  const menus: MenuDef[] = [
    {
      label: '文件',
      entries: [
        { kind: 'item', label: '导入照片…', action: () => navigate('ingest') },
        { kind: 'item', label: '导出…', action: () => navigate('export') },
        { kind: 'sep' },
        {
          kind: 'item',
          label: '打开 PSD…',
          action: async () => {
            const p = await window.lumina.pickFile();
            if (p) void useDevelop.getState().openPath(p);
          },
        },
      ],
    },
    {
      label: '编辑',
      entries: [
        {
          kind: 'item',
          label: '撤销',
          hint: 'Ctrl+Z',
          disabled: !sessionLive || !canUndo,
          action: () => void useDevelop.getState().undo(),
        },
        {
          kind: 'item',
          label: '重做',
          hint: 'Ctrl+⇧Z',
          disabled: !sessionLive || !canRedo,
          action: () => void useDevelop.getState().redo(),
        },
        { kind: 'sep' },
        {
          kind: 'item',
          label: '复制设置',
          hint: 'Ctrl+C',
          disabled: !sessionLive,
          action: () => {
            if (copyDevelopSettings()) toast('已复制当前设置（Ctrl+V 粘贴到其他照片）', 'info', 2500);
          },
        },
        {
          kind: 'item',
          label: '粘贴设置',
          hint: 'Ctrl+V',
          disabled: !sessionLive,
          action: () => void pasteDevelopSettings(),
        },
      ],
    },
    ...(inDevelop
      ? [
          {
            label: '视图',
            entries: [
              { kind: 'item', label: '显示左栏', checked: panels.left, action: () => dispatchCmd('develop:toggle-left') },
              { kind: 'item', label: '显示右栏', checked: panels.right, action: () => dispatchCmd('develop:toggle-right') },
              { kind: 'sep' },
              { kind: 'item', label: '前后对比', hint: '\\', action: toggleCompare },
              { kind: 'sep' },
              { kind: 'item', label: '参考线：关闭', checked: guides === 'off', action: () => useDevelop.getState().setGuides('off') },
              { kind: 'item', label: '参考线：三分构图', checked: guides === 'thirds', action: () => useDevelop.getState().setGuides('thirds') },
              { kind: 'item', label: '参考线：中心线', checked: guides === 'center', action: () => useDevelop.getState().setGuides('center') },
              { kind: 'item', label: '参考线：网格', checked: guides === 'grid', action: () => useDevelop.getState().setGuides('grid') },
            ] as MenuEntry[],
          },
          {
            label: '帮助',
            entries: [
              { kind: 'item', label: '快捷键速查', hint: '?', action: () => dispatchCmd('develop:show-shortcuts') },
            ] as MenuEntry[],
          },
        ]
      : []),
  ];

  return (
    <div ref={rootRef} className="app-no-drag flex items-center gap-0.5 shrink-0 self-stretch">
      {menus.map((m, i) => {
        const open = openIdx === i;
        return (
          <div key={m.label} className="relative self-stretch flex items-center">
            <button
              onClick={() => setOpenIdx(open ? null : i)}
              onMouseEnter={() => {
                // PS 行为：已有下拉展开时，扫过其他菜单名直接切换
                if (openIdx !== null && openIdx !== i) setOpenIdx(i);
              }}
              aria-expanded={open}
              aria-haspopup="menu"
              // G11：hover 整项填色（整块底色，不是只文字变色）；展开态保持填色
              className={`h-7 px-2 rounded-mdC text-2xs transition-colors ${
                open ? 'bg-hover text-txt' : 'text-txt2 hover:text-txt hover:bg-hover'
              }`}
            >
              {m.label}
            </button>
            {open && (
              <div
                role="menu"
                className="rounded-lgC border border-line animate-float-in absolute left-0 top-full mt-1 z-[100] py-1 min-w-[200px] max-h-[calc(100vh-72px)] overflow-y-auto"
                style={{ background: 'var(--Lumina-Tokens-panel)', boxShadow: 'var(--Lumina-Tokens-shadow2)' }}
              >
                {m.entries.map((en, j) =>
                  en.kind === 'sep' ? (
                    <div key={j} className="my-1 border-t border-line" />
                  ) : (
                    <button
                      key={j}
                      role="menuitem"
                      disabled={en.disabled}
                      onClick={() => {
                        setOpenIdx(null);
                        en.action();
                      }}
                      // G11：下拉项整行填色（w-full + hover 底色）
                      className={`w-full flex items-center gap-2 h-7 px-3 text-left text-2xs transition-colors ${
                        en.disabled ? 'text-txt3 cursor-default' : 'text-txt2 hover:text-txt hover:bg-hover'
                      }`}
                    >
                      <span className="w-3 shrink-0 text-accent">{en.checked ? '✓' : ''}</span>
                      <span className="flex-1 truncate">{en.label}</span>
                      {en.hint && <span className="num text-txt3 shrink-0">{en.hint}</span>}
                    </button>
                  ),
                )}
              </div>
            )}
          </div>
        );
      })}
    </div>
  );
}
