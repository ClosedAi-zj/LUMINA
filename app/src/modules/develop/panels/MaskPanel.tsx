/**
 * 蒙版面板（帧 02 L1）：渐变 / 径向 / 画笔三件套 + 蒙版列表（增删开关）+ 选中蒙版局部滑块。
 */

import { useDevelop, maskName, sliderLabel } from '../../../store/developStore';
import { Button, Slider, Toggle } from '../../../components/primitives';
import type { MaskLayer } from '../../../api/protocol';

const TOOLS: { type: MaskLayer['type']; label: string }[] = [
  { type: 'linear', label: '⟍ 渐变' },
  { type: 'radial', label: '◎ 径向' },
  { type: 'brush', label: '✎ 画笔' },
];

/** T2.8：未实装蒙版叶 —— 置灰 + tooltip 说明（零静默：明确告知不可用与原因，不放假可用控件） */
const PENDING_TOOLS: { type: MaskLayer['type']; label: string; tip: string }[] = [
  { type: 'aiSky', label: '☁ 天空', tip: 'AI 天空蒙版未实装（引擎 AI 分割未接入），随后续里程碑交付' },
  { type: 'aiSubject', label: '☗ 主体', tip: 'AI 主体蒙版未实装（引擎 AI 分割未接入），随后续里程碑交付' },
  { type: 'aiBackground', label: '▦ 背景', tip: 'AI 背景蒙版未实装（引擎 AI 分割未接入），随后续里程碑交付' },
];

const LOCAL_SLIDERS: { key: keyof MaskLayer['local']; label: string; min: number; max: number }[] = [
  { key: 'exposure', label: '曝光', min: -5, max: 5, },
  { key: 'contrast', label: '对比', min: -100, max: 100 },
  { key: 'highlights', label: '高光', min: -100, max: 100 },
  { key: 'shadows', label: '阴影', min: -100, max: 100 },
  { key: 'temperature', label: '色温', min: -100, max: 100 },
  { key: 'saturation', label: '饱和', min: -100, max: 100 },
  { key: 'clarity', label: '清晰', min: -100, max: 100 },
  { key: 'dehaze', label: '去雾', min: -100, max: 100 },
];

/** T2.8：布尔组合标记（replace=独立区域 / intersect=交集 / subtract=减去），列表内可视化 */
const COMBINE_MARK: Record<MaskLayer['combine'], { sym: string; label: string }> = {
  replace: { sym: '+', label: '独立区域' },
  intersect: { sym: '∩', label: '与上一条取交集' },
  subtract: { sym: '−', label: '从上一条减去' },
};

/** T2.8：性能提示阈值——20+ 槽只提示不阻断（v1.1 六槽硬限已由 32 槽管线替代） */
const PERF_HINT_SLOTS = 20;

export default function MaskPanel() {
  const params = useDevelop((s) => s.params);
  const selectedMaskId = useDevelop((s) => s.selectedMaskId);
  const selectMask = useDevelop((s) => s.selectMask);
  const addMask = useDevelop((s) => s.addMask);
  const updateMask = useDevelop((s) => s.updateMask);
  const updateMaskLocal = useDevelop((s) => s.updateMaskLocal);
  const removeMask = useDevelop((s) => s.removeMask);

  if (!params) return null;
  const selected = params.masks.find((m) => m.id === selectedMaskId) ?? null;

  return (
    <div>
      <div className="flex items-center gap-1.5 mb-2 flex-wrap">
        {TOOLS.map((t) => (
          <Button key={t.type} size="sm" onClick={() => void addMask(t.type)} title={`新建${maskName(t.type)}蒙版`}>
            {t.label}
          </Button>
        ))}
        {PENDING_TOOLS.map((t) => (
          <button
            key={t.type}
            disabled
            title={t.tip}
            aria-disabled
            className="h-6 px-2 rounded-sm text-2xs text-txt3 border border-line cursor-not-allowed select-none"
          >
            {t.label}
          </button>
        ))}
      </div>

      <div className="flex flex-col gap-px mb-2 max-h-36 overflow-y-auto">
        {params.masks.length === 0 && <div className="text-2xs text-txt3 py-1">无蒙版 · 点击上方工具新建</div>}
        {params.masks.map((m, i) => {
          const desc = m.type === 'linear' ? `渐变 ${(m.amount ?? 100) > 0 ? '+' : ''}${m.amount ?? 100}` : m.type === 'radial' ? `径向 ${(m.amount ?? 100) > 0 ? '+' : ''}${m.amount ?? 100}` : `画笔 ${m.strokes?.length ?? 0} 笔画`;
          const combine = COMBINE_MARK[m.combine];
          return (
            <div
              key={m.id}
              // T2.8：布尔关系缩进可读——首条顶格；后续条缩进 + 组合符标记（与上一条的组合方式）
              className={`flex items-center gap-1.5 h-7 px-2 rounded-sm cursor-pointer ${i > 0 ? 'ml-4' : ''} ${selectedMaskId === m.id ? 'bg-[var(--Lumina-Tokens-accentSoft)]' : 'hover:bg-hover'}`}
              onClick={() => selectMask(m.id)}
              title={i > 0 ? `组合方式：${combine.label}` : undefined}
            >
              {i > 0 && <span className="text-2xs text-txt3 w-3 text-center shrink-0" aria-hidden>{combine.sym}</span>}
              <Toggle checked={m.enabled} onChange={(v) => void updateMask(m.id, { enabled: v })} />
              <span className={`flex-1 truncate text-2xs ${m.enabled ? 'text-txt2' : 'text-txt3 line-through'}`}>
                {maskName(m.type)} · {desc}
              </span>
              <button className="text-txt3 hover:text-danger text-2xs px-1" onClick={(e) => { e.stopPropagation(); void removeMask(m.id); }} title="删除蒙版">
                ✕
              </button>
              <span className="num text-2xs text-txt3">#{i + 1}</span>
            </div>
          );
        })}
      </div>

      {/* T2.8：20+ 槽性能提示——只提示不阻断（v1.1 六槽硬限与「超 6 不参与渲染」警告
          已随 32 槽 mask pass 管线作废，旧限位 UI 一并清除） */}
      {params.masks.length > PERF_HINT_SLOTS && (
        <div
          className="text-2xs rounded-md px-2 py-1 mb-2"
          style={{ background: 'var(--Lumina-Tokens-accentSoft)', color: 'var(--Lumina-Tokens-accent)' }}
        >
          当前 {params.masks.length} 个蒙版（超过 {PERF_HINT_SLOTS}）：预览性能可能逐渐下降，建议合并或清理多余蒙版（不阻断，可继续编辑）
        </div>
      )}

      {selected && (
        <div className="rounded-md border border-line p-2.5">
          <div className="text-2xs text-txt2 mb-2 flex items-center justify-between">
            <span>
              蒙版 · {maskName(selected.type)}
            </span>
            <span className="flex items-center gap-1.5">
              {/* T2.8：蒙版反相未实装——置灰 + tooltip（零静默，不放假可用控件） */}
              <button
                disabled
                aria-disabled
                title="蒙版反相未实装（引擎 invert 未接入），随后续里程碑交付"
                className="h-5 px-1.5 rounded-sm border border-line text-2xs text-txt3 cursor-not-allowed select-none"
              >
                反相
              </button>
              <span className="num text-txt3">{selected.type}</span>
            </span>
          </div>
          {(selected.type === 'linear' || selected.type === 'radial' || selected.type === 'brush') && (
            <Slider label="主强度" value={selected.amount ?? 100} min={-100} max={100} defaultValue={0} onChange={(v) => void updateMask(selected.id, { amount: v })} />
          )}
          {selected.type === 'brush' && (
            <div className="text-2xs text-txt3 mb-2">笔画数：{selected.strokes?.length ?? 0}（WP-B5 起逐笔距离场渲染；选中蒙版后红色浮层即实际作用范围，按住 Alt 拖动为擦除）</div>
          )}
          <div className="border-t border-line pt-2">
            {LOCAL_SLIDERS.map((s) => (
              <Slider
                key={s.key}
                label={sliderLabel(s.key) + '（局部）'}
                value={selected.local[s.key]}
                min={s.min}
                max={s.max}
                defaultValue={0}
                onChange={(v) => void updateMaskLocal(selected.id, s.key, v, false)}
                onCommit={(v) => void updateMaskLocal(selected.id, s.key, v, true)}
              />
            ))}
          </div>
        </div>
      )}
    </div>
  );
}
