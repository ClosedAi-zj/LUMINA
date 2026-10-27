/**
 * 变换工具栏（设计稿 10）：裁剪开关 · 宽高比（自由/1:1/3:2/16:9）· 拉直 ·
 * 水平/垂直透视 · 缩放 · 参考线（三分/中心/网格，UI 叠加层）。
 * 参数存 params.geometry（协议 §4：hasCrop / cropAngle / perspective.*）。
 */

import { useDevelop } from '../../../store/developStore';
import { Slider, Toggle } from '../../../components/primitives';

/** 宽高比四档（OptionBar 裁剪上下文与工具轨共用同一份） */
export const ASPECTS: { label: string; value: number }[] = [
  { label: '自由', value: 0 },
  { label: '1:1', value: 1 },
  { label: '3:2', value: 1.5 },
  { label: '16:9', value: 16 / 9 },
];

const GEOM_LABEL: Record<string, string> = {
  rotate: '拉直',
  vertical: '水平透视',
  horizontal: '垂直透视',
  scale: '缩放',
};

export default function TransformBar() {
  const params = useDevelop((s) => s.params);
  const guides = useDevelop((s) => s.guides);
  if (!params) return null;
  const geo = params.geometry;
  const p = geo.perspective;

  return (
    <div className="shrink-0 border-t border-line px-3 py-1.5 flex items-center gap-3 overflow-x-auto" style={{ background: 'var(--Lumina-Tokens-panel)' }}>
      <span className="text-2xs text-txt3 shrink-0">变换 · GEOMETRY</span>

      <Toggle checked={geo.hasCrop} onChange={(v) => void useDevelop.getState().setPath('geometry.hasCrop', v, true, v ? '开启裁剪' : '关闭裁剪')} label="裁剪" />

      {/* 宽高比 */}
      <div className="flex items-center gap-0.5 shrink-0">
        {ASPECTS.map((a) => (
          <button
            key={a.label}
            onClick={() => void useDevelop.getState().setPath('geometry.perspective.aspect', a.value, true, `宽高比 ${a.label}`)}
            className={`h-5 px-1.5 rounded-sm border num text-2xs transition-colors ${
              Math.abs(p.aspect - a.value) < 0.01 ? 'border-accent text-accent' : 'border-line text-txt3 hover:text-txt2 hover:bg-hover'
            }`}
            style={{ background: Math.abs(p.aspect - a.value) < 0.01 ? 'var(--Lumina-Tokens-accentSoft)' : 'var(--Lumina-Tokens-card)' }}
          >
            {a.label}
          </button>
        ))}
      </div>

      <div className="w-28 shrink-0">
        <GeomSlider path="geometry.cropAngle" label="拉直角" min={-10} max={10} step={0.1} value={geo.cropAngle ?? 0} defaultValue={0} format={(v) => `${v.toFixed(1)}°`} />
      </div>
      <div className="w-28 shrink-0">
        <GeomSlider path="geometry.perspective.rotate" label="拉直" min={-45} max={45} step={0.5} value={p.rotate} defaultValue={0} />
      </div>
      <div className="w-28 shrink-0">
        <GeomSlider path="geometry.perspective.vertical" label="水平" min={-100} max={100} step={1} value={p.vertical} defaultValue={0} />
      </div>
      <div className="w-28 shrink-0">
        <GeomSlider path="geometry.perspective.horizontal" label="垂直" min={-100} max={100} step={1} value={p.horizontal} defaultValue={0} />
      </div>
      <div className="w-28 shrink-0">
        <GeomSlider path="geometry.perspective.scale" label="缩放" min={0.5} max={2} step={0.01} value={p.scale} defaultValue={1} format={(v) => `${v.toFixed(2)}×`} />
      </div>

      {/* 参考线（UI 叠加层，不落盘） */}
      <div className="flex items-center gap-0.5 shrink-0">
        <span className="text-2xs text-txt3 mr-0.5">参考线</span>
        {(
          [
            { key: 'off', label: '无' },
            { key: 'thirds', label: '三分' },
            { key: 'center', label: '中心' },
            { key: 'grid', label: '网格' },
          ] as const
        ).map((g) => (
          <button
            key={g.key}
            onClick={() => useDevelop.getState().setGuides(g.key)}
            className={`h-5 px-1.5 rounded-sm border text-2xs transition-colors ${
              guides === g.key ? 'border-accent text-accent' : 'border-line text-txt3 hover:text-txt2 hover:bg-hover'
            }`}
            style={{ background: guides === g.key ? 'var(--Lumina-Tokens-accentSoft)' : 'var(--Lumina-Tokens-card)' }}
          >
            {g.label}
          </button>
        ))}
      </div>
    </div>
  );
}

function GeomSlider({
  path,
  label,
  min,
  max,
  step,
  value,
  defaultValue,
  format,
}: {
  path: string;
  label: string;
  min: number;
  max: number;
  step: number;
  value: number;
  defaultValue: number;
  format?: (v: number) => string;
}) {
  return (
    <Slider
      label={GEOM_LABEL[path.split('.').pop() ?? path] ?? label}
      value={value}
      min={min}
      max={max}
      step={step}
      defaultValue={defaultValue}
      onChange={(v) => void useDevelop.getState().setPath(path, v, false)}
      onCommit={(v) => void useDevelop.getState().setPath(path, v, true)}
      format={format}
    />
  );
}
