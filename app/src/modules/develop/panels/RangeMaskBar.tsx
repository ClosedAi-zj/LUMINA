/**
 * 范围蒙版条（设计稿 10 中下方）：亮度 / 颜色 / 深度三种范围选择器，
 * 叠加到当前选中蒙版（协议 MaskLayer.type = luminanceRange / colorRange / depth，
 * range {low, high, samples}）。选中范围蒙版时显示 low/high 滑条。
 */

import { useDevelop, maskName } from '../../../store/developStore';
import { Slider } from '../../../components/primitives';

const RANGE_TYPES: { type: 'luminanceRange' | 'colorRange' | 'depth'; label: string }[] = [
  { type: 'luminanceRange', label: '亮度' },
  { type: 'colorRange', label: '颜色' },
  { type: 'depth', label: '深度' },
];

export default function RangeMaskBar() {
  const params = useDevelop((s) => s.params);
  const selectedMaskId = useDevelop((s) => s.selectedMaskId);
  if (!params) return null;
  const selected = params.masks.find((m) => m.id === selectedMaskId) ?? null;
  const isRange = selected !== null && (selected.type === 'luminanceRange' || selected.type === 'colorRange' || selected.type === 'depth');

  return (
    <div className="shrink-0 border-t border-line px-3 py-1.5 flex items-center gap-3 overflow-x-auto" style={{ background: 'var(--Lumina-Tokens-panel)' }}>
      <span className="text-2xs text-txt3 shrink-0">
        范围蒙版 · RANGE MASK{selected ? ` · 目标：${maskName(selected.type)}` : ''}
      </span>
      <div className="flex items-center gap-1 shrink-0">
        {RANGE_TYPES.map((t) => (
          <button
            key={t.type}
            onClick={() => void useDevelop.getState().addMask(t.type)}
            className={`h-5 px-2 rounded-sm border text-2xs transition-colors ${
              selected?.type === t.type ? 'border-accent text-accent' : 'border-line text-txt3 hover:text-txt2 hover:bg-hover'
            }`}
            style={{ background: selected?.type === t.type ? 'var(--Lumina-Tokens-accentSoft)' : 'var(--Lumina-Tokens-card)' }}
            title={`新建${t.label}范围蒙版（叠加到当前选中蒙版）`}
          >
            {t.label}
          </button>
        ))}
      </div>
      {isRange && selected?.range && (
        <div className="flex items-center gap-3 min-w-0 flex-1">
          <div className="w-40 shrink-0">
            <Slider
              label="下限"
              value={selected.range.low}
              min={0}
              max={1}
              step={0.01}
              defaultValue={0}
              onChange={(v) => void useDevelop.getState().updateMask(selected.id, { range: { ...selected.range!, low: v } })}
              format={(v) => v.toFixed(2)}
            />
          </div>
          <div className="w-40 shrink-0">
            <Slider
              label="上限"
              value={selected.range.high}
              min={0}
              max={1}
              step={0.01}
              defaultValue={1}
              onChange={(v) => void useDevelop.getState().updateMask(selected.id, { range: { ...selected.range!, high: v } })}
              format={(v) => v.toFixed(2)}
            />
          </div>
          <span className="num text-2xs text-txt3 shrink-0">采样 {selected.range.samples.length} 点</span>
        </div>
      )}
      {selected && !isRange && <span className="text-2xs text-txt3 truncate">当前选中为 {maskName(selected.type)} 蒙版 · 范围选择器将作为新蒙版叠加</span>}
      {!selected && <span className="text-2xs text-txt3 truncate">先在蒙版列表选中一个蒙版，或直接新建范围蒙版</span>}
    </div>
  );
}
