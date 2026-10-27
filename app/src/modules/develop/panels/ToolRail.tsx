/**
 * 左侧工具轨（v1.3.0-m3 PS 工具栏形态）：40px 垂直窄条，图标按钮 + 分组分隔线。
 *
 * 零假控件铁律：每个按钮都连真实 store 行为——
 *  - 裁剪：geometry.hasCrop 开合（原 ViewerToolbar 裁剪入口收编，TransformBar 既有能力不变）
 *  - 渐变/径向：addMask(type) 新建蒙版（原 MaskPanel 同一动作）
 *  - 画笔：已有画笔蒙版 → setMaskTool 进出涂画态；没有 → addMask('brush') 后直接进入涂画
 *  - 亮度/颜色：addMask(type) 新建范围蒙版（原 RangeMaskBar 同一动作）
 * 抓手/缩放/白平衡取色器无真实实现（store 无视图缩放与取色管线），按铁律宁缺毋滥，不出现。
 * 画笔激活后 ViewerCanvas 浮层 cursor=crosshair（既有交互，未新造流程）。
 */

import { useDevelop, maskName } from '../../../store/developStore';
import { useJobStore } from '../../../store/jobStore';
import { LuIcon } from '../../../components/icons';
import type { MaskLayer } from '../../../api/protocol';

type MaskType = MaskLayer['type'];

/** 分组结构：裁剪 ｜ 蒙版三件套 ｜ 范围蒙版二件套 */
const GROUPS: { id: string; icon: string; label: string }[][] = [
  [{ id: 'crop', icon: 'crop', label: '裁剪' }],
  [
    { id: 'linear', icon: 'mask-linear', label: '渐变' },
    { id: 'radial', icon: 'mask-radial', label: '径向' },
    { id: 'brush', icon: 'brush', label: '画笔' },
  ],
  [
    { id: 'luminanceRange', icon: 'mask-luminance', label: '亮度' },
    { id: 'colorRange', icon: 'mask-color', label: '颜色' },
  ],
];

export default function ToolRail() {
  const params = useDevelop((s) => s.params);
  const maskTool = useDevelop((s) => s.maskTool);
  const selectedMaskId = useDevelop((s) => s.selectedMaskId);
  if (!params) return null;

  const selected = params.masks.find((m) => m.id === selectedMaskId) ?? null;
  const cropped = params.geometry.hasCrop;

  const activate = (id: string) => {
    const st = useDevelop.getState();
    if (id === 'crop') {
      void st.setPath('geometry.hasCrop', !cropped, true, cropped ? '关闭裁剪' : '开启裁剪');
      return;
    }
    if (id === 'brush') {
      if (selected?.type === 'brush' && st.maskTool !== 'brush') {
        st.setMaskTool('brush');
        return;
      }
      if (st.maskTool === 'brush') {
        st.setMaskTool(null);
        return;
      }
      // 没有可涂画的画笔蒙版：新建一个（addMask 完成后选中态已指向它）再进入涂画
      void st.addMask('brush').then(() => useDevelop.getState().setMaskTool('brush'));
      useJobStore.getState().toast('画笔蒙版已建 · 直接在画面上涂画（Alt+拖动=擦除）', 'info', 3000);
      return;
    }
    // linear / radial / luminanceRange / colorRange：与 MaskPanel / RangeMaskBar 同一 addMask 动作
    void st.addMask(id as MaskType);
    st.setMaskTool(null);
  };

  /** 选中态口径：画笔看 maskTool（涂画态），其余看当前选中蒙版类型 / 裁剪开关 */
  const isActive = (id: string) => {
    if (id === 'crop') return cropped;
    if (id === 'brush') return maskTool === 'brush';
    return selected?.type === id;
  };

  return (
    <div
      data-testid="tool-rail"
      className="shrink-0 w-10 flex flex-col items-center py-1 gap-0.5 border-r border-line"
      style={{ background: 'var(--Lumina-Tokens-panel)' }}
      role="toolbar"
      aria-label="工具轨"
    >
      {GROUPS.map((group, gi) => (
        <div key={gi} className="flex flex-col items-center gap-0.5 w-full">
          {gi > 0 && <div className="w-5 h-px my-1" style={{ background: 'var(--Lumina-Tokens-border)' }} />}
          {group.map((tool) => {
            const active = isActive(tool.id);
            const shortcut = tool.id === 'crop' ? '（C 开/关裁剪框）' : '';
            return (
              <button
                key={tool.id}
                onClick={() => activate(tool.id)}
                title={`${tool.label}${shortcut} · ${tool.id === 'crop' ? '开合裁剪框，比例在上方选项栏' : `新建${maskName(tool.id as MaskType)}蒙版`}`}
                aria-pressed={active}
                className="relative w-8 h-8 flex items-center justify-center rounded-smC transition-colors"
                style={{
                  color: active ? 'var(--Lumina-Tokens-accent)' : 'var(--Lumina-Tokens-textSecondary)',
                  background: active ? 'var(--Lumina-Tokens-accentSoft)' : undefined,
                }}
                onMouseEnter={(e) => {
                  if (!active) e.currentTarget.style.background = 'var(--Lumina-Tokens-hover)';
                }}
                onMouseLeave={(e) => {
                  if (!active) e.currentTarget.style.background = '';
                }}
              >
                {/* PS 式选中态：accent 左缘条 + 图标填色 */}
                {active && (
                  <span
                    className="absolute left-0 top-1.5 bottom-1.5 w-0.5 rounded-pill"
                    style={{ background: 'var(--Lumina-Tokens-accent)' }}
                  />
                )}
                <LuIcon name={tool.icon} size={16} />
              </button>
            );
          })}
        </div>
      ))}
    </div>
  );
}
