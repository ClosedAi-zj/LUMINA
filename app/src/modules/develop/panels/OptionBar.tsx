/**
 * 顶部上下文选项栏（v1.3.0-m3 PS Option Bar 形态，36px）：菜单位置 = 修图台顶栏之下、观察区之上。
 *
 * 上下文三态（专业模式）：
 *  - 裁剪激活（geometry.hasCrop）→ 比例四档（TransformBar 既有 ASPECTS，同一 setPath 写入）
 *  - 画笔工具激活（maskTool='brush'）→ 选中蒙版真实字段：主强度 amount / 不透明度 opacity /
 *    combine 组合模式（updateMask 既有动作）
 *  - 无工具 → 视图选项：前后对比（与 \ 键同一切换状态）、参考线四档（setGuides）、
 *    缩放百分比（画布 contain 实测展示）
 * 零假控件说明：羽化 / 流量 / 浓度在 store 无对应字段（笔画 flow/density 为常量）、
 * 「适合窗口」无视图缩放管线——均不出现，如实降级。
 */

import { useEffect, useState } from 'react';
import { useDevelop } from '../../../store/developStore';
import { useLibrary } from '../../../store/libraryStore';
import { useJobStore } from '../../../store/jobStore';
import { errText } from '../../../api/rpc-client';
import { Slider } from '../../../components/primitives';
import { LuIcon } from '../../../components/icons';
import { ParamSegmented } from './panelKit';
import { ASPECTS } from './TransformBar';
import type { MaskLayer } from '../../../api/protocol';

const GUIDES: { key: 'off' | 'thirds' | 'center' | 'grid'; label: string }[] = [
  { key: 'off', label: '无' },
  { key: 'thirds', label: '三分' },
  { key: 'center', label: '中心' },
  { key: 'grid', label: '网格' },
];

const COMBINE_OPTIONS: { value: MaskLayer['combine']; label: string }[] = [
  { value: 'replace', label: '独立' },
  { value: 'intersect', label: '交集' },
  { value: 'subtract', label: '减去' },
];

export default function OptionBar({ zoomPct, floating = false }: { zoomPct: number | null; floating?: boolean }) {
  const params = useDevelop((s) => s.params);
  const maskTool = useDevelop((s) => s.maskTool);
  const selectedMaskId = useDevelop((s) => s.selectedMaskId);
  const guides = useDevelop((s) => s.guides);
  // 前后对比：状态真相源在 ViewerCanvas（\ 键同款 state），经窗口事件同步选中态
  const [compareOn, setCompareOn] = useState(false);
  useEffect(() => {
    const onState = (e: Event) => setCompareOn(Boolean((e as CustomEvent<{ on: boolean }>).detail?.on));
    window.addEventListener('lumina:compare-state', onState);
    return () => window.removeEventListener('lumina:compare-state', onState);
  }, []);

  if (!params) return null;
  const selected = params.masks.find((m) => m.id === selectedMaskId) ?? null;
  const cropping = params.geometry.hasCrop;
  const brushMode = maskTool === 'brush' && selected?.type === 'brush';

  const ctxBtn = (active: boolean) =>
    `h-5 px-1.5 rounded-smC border text-2xs transition-colors ${
      active ? 'border-accent text-accent' : 'border-line text-txt3 hover:text-txt2 hover:bg-hover'
    }`;

  return (
    <div
      data-testid="option-bar"
      className={
        floating
          ? 'glass-m2 h-9 px-2.5 flex items-center gap-3 rounded-mdC border border-line overflow-x-auto scrollbar-none whitespace-nowrap'
          : 'h-9 px-2 flex items-center gap-3 border-b border-line shrink-0 overflow-x-auto scrollbar-none whitespace-nowrap'
      }
      style={floating ? undefined : { background: 'var(--Lumina-Tokens-panel)' }}
    >
      {/* —— 上下文 ①：裁剪 → 比例四档 —— */}
      {cropping && (
        <>
          <span className="text-2xs text-txt3 shrink-0 flex items-center gap-1">
            <LuIcon name="crop" size={12} /> 裁剪 · 比例
          </span>
          <div className="flex items-center gap-0.5 shrink-0">
            {ASPECTS.map((a) => {
              const active = Math.abs(params.geometry.perspective.aspect - a.value) < 0.01;
              return (
                <button
                  key={a.label}
                  onClick={() => void useDevelop.getState().setPath('geometry.perspective.aspect', a.value, true, `宽高比 ${a.label}`)}
                  className={`${ctxBtn(active)} num`}
                  style={{ background: active ? 'var(--Lumina-Tokens-accentSoft)' : 'var(--Lumina-Tokens-card)' }}
                >
                  {a.label}
                </button>
              );
            })}
          </div>
          <span className="text-2xs text-txt3 shrink-0">拉直 / 透视 / 参考线在画面下方变换条</span>
        </>
      )}

      {/* —— 上下文 ②：画笔工具 → 选中蒙版参数（真实字段读写） —— */}
      {!cropping && brushMode && selected && (
        <>
          <span className="text-2xs text-txt3 shrink-0 flex items-center gap-1">
            <LuIcon name="brush" size={12} /> 画笔 · 在画面涂画（Alt=擦除）
          </span>
          <div className="w-32 shrink-0">
            <Slider
              label="主强度"
              value={selected.amount ?? 100}
              min={-100}
              max={100}
              defaultValue={0}
              // 拖动中不写历史，松手提交一次（与蒙版面板同一口径）
              onChange={(v) => void useDevelop.getState().updateMaskField(selected.id, { amount: v }, false)}
              onCommit={(v) => void useDevelop.getState().updateMaskField(selected.id, { amount: v }, true, '蒙版 · 主强度')}
            />
          </div>
          <div className="w-32 shrink-0">
            <Slider
              label="不透明度"
              value={selected.opacity}
              min={0}
              max={100}
              defaultValue={100}
              onChange={(v) => void useDevelop.getState().updateMaskField(selected.id, { opacity: v }, false)}
              onCommit={(v) => void useDevelop.getState().updateMaskField(selected.id, { opacity: v }, true, '蒙版 · 不透明度')}
            />
          </div>
          <ParamSegmented
            value={selected.combine}
            options={COMBINE_OPTIONS}
            onChange={(v) => void useDevelop.getState().updateMask(selected.id, { combine: v })}
          />
        </>
      )}

      {/* —— 上下文 ③：无工具 → 视图选项 —— */}
      {!cropping && !brushMode && (
        <>
          <button
            onClick={() => window.dispatchEvent(new CustomEvent('lumina:compare-toggle'))}
            className="inline-flex items-center gap-1.5 h-6 px-2 rounded-smC border text-2xs transition-colors"
            style={{
              borderColor: compareOn ? 'var(--Lumina-Tokens-accent)' : 'var(--Lumina-Tokens-border)',
              color: compareOn ? 'var(--Lumina-Tokens-accent)' : 'var(--Lumina-Tokens-textSecondary)',
              background: compareOn ? 'var(--Lumina-Tokens-accentSoft)' : 'var(--Lumina-Tokens-card)',
            }}
            title="前后对比（\\ 或 V 同效）"
          >
            <LuIcon name="eye" size={12} />
            前后对比
          </button>
          <div className="flex items-center gap-0.5 shrink-0">
            <span className="text-2xs text-txt3 mr-0.5">参考线</span>
            {GUIDES.map((g) => {
              const active = guides === g.key;
              return (
                <button
                  key={g.key}
                  onClick={() => useDevelop.getState().setGuides(g.key)}
                  className={ctxBtn(active)}
                  style={{ background: active ? 'var(--Lumina-Tokens-accentSoft)' : 'var(--Lumina-Tokens-card)' }}
                >
                  {g.label}
                </button>
              );
            })}
          </div>
          <span className="num text-2xs text-txt3 shrink-0" title="缩放 = 画布显示宽 / 位图宽（contain 适配实测）">
            {zoomPct !== null ? `${zoomPct.toFixed(0)}%` : '—'}
          </span>
        </>
      )}

      <div className="flex-1" />
      {/* 右侧常驻动作（原 ViewerToolbar 收编，行为不变） */}
      <OptionBarActions />
    </div>
  );
}

/** 同步 / 复位（原 ViewerToolbar 右侧两钮原样迁移，接线不变） */
function OptionBarActions() {
  const photoId = useDevelop((s) => s.photoId);
  const toast = useJobStore((s) => s.toast);
  const btn = 'inline-flex items-center gap-1 h-6 px-2 rounded-smC text-2xs text-txt2 hover:text-txt hover:bg-hover transition-colors';
  return (
    <>
      <button
        className={btn}
        onClick={async () => {
          if (photoId === null) return;
          const targets = useLibrary.getState().selectedIds.filter((id) => id !== photoId);
          if (targets.length === 0) {
            toast('先在素材库选中要应用的目标照片', 'warn', 4000);
            return;
          }
          try {
            const { api } = await import('../../../api/engine');
            const res = await api.edit.syncToSelection(photoId, targets, true);
            toast(`已同步到 ${res.updated} 张`, 'success', 3000);
          } catch (err) {
            toast(`同步失败：${errText(err)}`, 'error', 6000);
          }
        }}
        title="把当前参数同步到图库所选照片"
      >
        <LuIcon name="copy" size={12} />
        同步
      </button>
      <button className={btn} onClick={() => void useDevelop.getState().reset()} title="复位全部参数（可撤销）">
        <LuIcon name="reset" size={12} />
        复位
      </button>
    </>
  );
}
