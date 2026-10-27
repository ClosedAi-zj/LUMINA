/**
 * 左侧工具轨（v1.4.0 · PS 式可展开工具组）
 *
 * 与 m3 的单层图标列相比，这里把「同族工具」收进一个工具位：
 *   - 裁剪：裁剪框开合（C）
 *   - 蒙版组：渐变 / 径向 / 画笔（点击=用上次选中的子工具；长按 260ms 或点右下角标 = 展开 flyout）
 *   - 范围蒙版组：亮度 / 颜色（同上）
 * 组内记忆上次选择（PS 行为），选中态 = accent 左缘条 + accentSoft 底 + 图标填色；有子工具的位显示右下三角角标。
 *
 * 零假控件铁律不变：每个子工具都连真实 store 行为（addMask / setMaskTool / setPath），
 * 抓手/缩放/白平衡取色器因 store 无对应管线，仍不出现。
 */

import { useCallback, useEffect, useRef, useState } from 'react';
import { useDevelop, maskName } from '../../../store/developStore';
import { useJobStore } from '../../../store/jobStore';
import { LuIcon } from '../../../components/icons';
import type { MaskLayer } from '../../../api/protocol';

type MaskType = MaskLayer['type'];

type SubTool = { id: string; icon: string; label: string; hint?: string };
type Group = { id: string; subs: SubTool[] };

const GROUPS: Group[] = [
  {
    id: 'crop',
    subs: [{ id: 'crop', icon: 'crop', label: '裁剪', hint: 'C' }],
  },
  {
    id: 'mask',
    subs: [
      { id: 'linear', icon: 'mask-linear', label: '渐变' },
      { id: 'radial', icon: 'mask-radial', label: '径向' },
      { id: 'brush', icon: 'brush', label: '画笔' },
    ],
  },
  {
    id: 'range',
    subs: [
      { id: 'luminanceRange', icon: 'mask-luminance', label: '亮度' },
      { id: 'colorRange', icon: 'mask-color', label: '颜色' },
    ],
  },
];

export default function ToolRail() {
  const params = useDevelop((s) => s.params);
  const maskTool = useDevelop((s) => s.maskTool);
  const selectedMaskId = useDevelop((s) => s.selectedMaskId);
  /** 组内记忆上次选择（PS 行为）：初始 = 各组第一个子工具 */
  const [lastUsed, setLastUsed] = useState<Record<string, string>>(() =>
    Object.fromEntries(GROUPS.map((g) => [g.id, g.subs[0].id])),
  );
  const [openGroup, setOpenGroup] = useState<string | null>(null);
  const pressTimer = useRef<number | null>(null);
  const railRef = useRef<HTMLDivElement | null>(null);

  // Esc 关闭 flyout；点外部关闭（与 PS 一致，flyout 是瞬态面板）
  useEffect(() => {
    if (!openGroup) return;
    const onKey = (e: KeyboardEvent) => {
      if (e.key === 'Escape') {
        e.stopPropagation();
        setOpenGroup(null);
      }
    };
    const onDown = (e: MouseEvent) => {
      if (!railRef.current?.contains(e.target as Node)) setOpenGroup(null);
    };
    window.addEventListener('keydown', onKey, true);
    window.addEventListener('mousedown', onDown);
    return () => {
      window.removeEventListener('keydown', onKey, true);
      window.removeEventListener('mousedown', onDown);
    };
  }, [openGroup]);

  const activate = useCallback((id: string) => {
    const st = useDevelop.getState();
    if (id === 'crop') {
      const cropped = st.params?.geometry.hasCrop ?? false;
      void st.setPath('geometry.hasCrop', !cropped, true, cropped ? '关闭裁剪' : '开启裁剪');
      return;
    }
    if (id === 'brush') {
      const sel = st.params?.masks.find((m) => m.id === st.selectedMaskId) ?? null;
      if (sel?.type === 'brush' && st.maskTool !== 'brush') {
        st.setMaskTool('brush');
        return;
      }
      if (st.maskTool === 'brush') {
        st.setMaskTool(null);
        return;
      }
      void st.addMask('brush').then(() => useDevelop.getState().setMaskTool('brush'));
      useJobStore.getState().toast('画笔蒙版已建 · 直接在画面上涂画（Alt+拖动=擦除）', 'info', 3000);
      return;
    }
    void st.addMask(id as MaskType);
    st.setMaskTool(null);
  }, []);

  if (!params) return null;

  const selected = params.masks.find((m) => m.id === selectedMaskId) ?? null;
  const cropped = params.geometry.hasCrop;

  /** 选中态口径：画笔看 maskTool（涂画态），其余看当前选中蒙版类型 / 裁剪开关 */
  const isActive = (id: string) => {
    if (id === 'crop') return cropped;
    if (id === 'brush') return maskTool === 'brush';
    return selected?.type === id;
  };

  const pickSub = (groupId: string, id: string) => {
    setLastUsed((m) => ({ ...m, [groupId]: id }));
    setOpenGroup(null);
    activate(id);
  };

  return (
    <div
      ref={railRef}
      data-testid="tool-rail"
      className="shrink-0 w-12 flex flex-col items-center py-2 gap-1.5 border-r border-line relative z-20"
      style={{ background: 'var(--Lumina-Tokens-toolrail)' }}
      role="toolbar"
      aria-label="工具轨"
    >
      {GROUPS.map((group, gi) => {
        const currentId = lastUsed[group.id];
        const current = group.subs.find((s) => s.id === currentId) ?? group.subs[0];
        const active = isActive(current.id);
        const hasSubs = group.subs.length > 1;
        const isOpen = openGroup === group.id;

        return (
          <div key={group.id} className="flex flex-col items-center w-full">
            {gi > 0 && <div className="w-6 h-px my-1.5" style={{ background: 'var(--Lumina-Tokens-border)' }} />}

            <div className="relative">
              <button
                onClick={() => {
                  // 短按 = 用当前子工具；已有 flyout 展开则收起
                  if (isOpen) {
                    setOpenGroup(null);
                    return;
                  }
                  activate(current.id);
                }}
                onContextMenu={(e) => {
                  e.preventDefault();
                  if (hasSubs) setOpenGroup(isOpen ? null : group.id);
                }}
                onMouseDown={() => {
                  if (!hasSubs) return;
                  pressTimer.current = window.setTimeout(() => setOpenGroup(group.id), 260);
                }}
                onMouseUp={() => {
                  if (pressTimer.current) {
                    window.clearTimeout(pressTimer.current);
                    pressTimer.current = null;
                  }
                }}
                onMouseLeave={(e) => {
                  if (pressTimer.current) {
                    window.clearTimeout(pressTimer.current);
                    pressTimer.current = null;
                  }
                  if (!active && !isOpen) e.currentTarget.style.background = '';
                }}
                title={`${current.label}${hasSubs ? `（长按或右键展开 ${group.subs.length} 个子工具）` : '（C 开/关裁剪框）'}`}
                aria-pressed={active}
                aria-haspopup={hasSubs ? 'menu' : undefined}
                aria-expanded={hasSubs ? isOpen : undefined}
                data-tool={current.id}
                className="mac-transition relative w-8 h-8 flex items-center justify-center rounded-smC"
                style={{
                  color: active ? 'var(--Lumina-Tokens-accent)' : 'var(--Lumina-Tokens-textSecondary)',
                  background: active || isOpen ? 'var(--Lumina-Tokens-accentSoft)' : undefined,
                }}
                onMouseEnter={(e) => {
                  if (!active && !isOpen) e.currentTarget.style.background = 'var(--Lumina-Tokens-hover)';
                }}
              >
                {/* PS 式选中态：accent 左缘条 */}
                {active && (
                  <span
                    className="absolute left-0 top-1.5 bottom-1.5 w-0.5 rounded-pill"
                    style={{ background: 'var(--Lumina-Tokens-accent)' }}
                  />
                )}
                <LuIcon name={current.icon} size={20} />
                {/* 子工具角标（PS 工具位右下三角） */}
                {hasSubs && (
                  <span
                    className="absolute right-0.5 bottom-0.5"
                    style={{
                      width: 0,
                      height: 0,
                      borderLeft: '4px solid transparent',
                      borderBottom: '4px solid var(--Lumina-Tokens-textMuted)',
                    }}
                  />
                )}
              </button>

              {/* flyout：玻璃浮层，图标 + 名称 + 快捷键 */}
              {isOpen && hasSubs && (
                <div
                  role="menu"
                  className="glass-m2 absolute left-full top-0 ml-1.5 py-1 rounded-mdC border border-line min-w-[132px]"
                  style={{ background: 'var(--Lumina-Tokens-glass)' }}
                >
                  {group.subs.map((sub) => {
                    const subActive = isActive(sub.id);
                    return (
                      <button
                        key={sub.id}
                        role="menuitem"
                        onClick={() => pickSub(group.id, sub.id)}
                        className="mac-transition w-full flex items-center gap-2 px-2.5 h-7 text-xs text-left hover:bg-hover"
                        style={{ color: subActive ? 'var(--Lumina-Tokens-accent)' : 'var(--Lumina-Tokens-textPrimary)' }}
                      >
                        <LuIcon name={sub.icon} size={14} />
                        <span className="flex-1">{sub.label}</span>
                        {sub.hint && <span className="text-[10px] text-txt3">{sub.hint}</span>}
                        {subActive && <span className="text-[10px]">●</span>}
                      </button>
                    );
                  })}
                </div>
              )}
            </div>
          </div>
        );
      })}

      {/* 底部：环境提示（真实状态，不放占位按钮） */}
      <div className="mt-auto flex flex-col items-center gap-1 pb-1">
        <span className="text-[9px] text-txt3" title="当前蒙版数量">
          {params.masks.length}
        </span>
      </div>
    </div>
  );
}
