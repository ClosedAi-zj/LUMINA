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

/** 着色器只有 6 个蒙版槽位（GLSL ES 1.0 常量展开，见 shaders.ts） */
const MASK_SLOTS = 6;

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
      <div className="flex items-center gap-1.5 mb-2">
        {TOOLS.map((t) => (
          <Button key={t.type} size="sm" onClick={() => void addMask(t.type)} title={`新建${maskName(t.type)}蒙版`}>
            {t.label}
          </Button>
        ))}
      </div>

      <div className="flex flex-col gap-px mb-2 max-h-36 overflow-y-auto">
        {params.masks.length === 0 && <div className="text-2xs text-txt3 py-1">无蒙版 · 点击上方工具新建</div>}
        {params.masks.map((m, i) => {
          const desc = m.type === 'linear' ? `渐变 ${m.amount > 0 ? '+' : ''}${m.amount}` : m.type === 'radial' ? `径向 ${m.amount > 0 ? '+' : ''}${m.amount}` : `画笔 ${m.strokes?.length ?? 0} 笔画`;
          return (
            <div
              key={m.id}
              className={`flex items-center gap-1.5 h-7 px-2 rounded-sm cursor-pointer ${selectedMaskId === m.id ? 'bg-[var(--Lumina-Tokens-accentSoft)]' : 'hover:bg-hover'}`}
              onClick={() => selectMask(m.id)}
            >
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

      {/* 槽位上限必须可见：超过 6 个蒙版时，后面的蒙版不参与渲染（着色器槽位），
          否则用户会以为"蒙版画了但没效果"——这正是冒烟里 brush-atlas 抓到的静默失效 */}
      {params.masks.filter((m) => m.enabled).length > MASK_SLOTS && (
        <div
          className="text-2xs rounded-md px-2 py-1 mb-2"
          style={{ background: 'rgba(185,138,46,0.14)', color: 'var(--Lumina-Tokens-accent)' }}
        >
          渲染槽位上限 {MASK_SLOTS}：当前 {params.masks.filter((m) => m.enabled).length} 个启用蒙版，
          超出部分（列表尾部）不参与预览渲染
        </div>
      )}

      {selected && (
        <div className="rounded-md border border-line p-2.5">
          <div className="text-2xs text-txt2 mb-2 flex items-center justify-between">
            <span>
              蒙版 · {maskName(selected.type)}
            </span>
            <span className="num text-txt3">{selected.type}</span>
          </div>
          {(selected.type === 'linear' || selected.type === 'radial') && (
            <Slider label="主强度" value={selected.amount} min={-100} max={100} defaultValue={0} onChange={(v) => void updateMask(selected.id, { amount: v })} />
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
