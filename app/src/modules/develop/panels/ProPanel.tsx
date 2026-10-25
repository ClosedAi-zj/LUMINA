/**
 * 修图工作区右栏（v1.2.0 阶段 3 皮肤）：8 组折叠卡片 + 直方图。
 *
 * 叶数归属（与架构文档 §4 的 93 叶表一致，逐组标注）：
 *   基础 14 ｜ 曲线 11 ｜ 色彩 32 ｜ 细节 7 ｜ 镜头 2 ｜ 几何 13 ｜ 效果 6 ｜ 校准 7 ｜ 蒙版 1（容器）
 * 合计 14+11+32+7+2+13+6+7+1 = 93。
 *
 * 诚实性：引擎当前只有 39 叶参与渲染（team-lead 口径），其余 54 叶**仍可编辑并写入 XMP**，
 * 但在顶部常驻提示里明确告知未参与渲染；整组未渲染的组在标题右侧标「未渲染」，不置灰、不静默。
 */

import { useState } from 'react';
import { useDevelop, sliderLabel } from '../../../store/developStore';
import CurveEditor from '../CurveEditor';
import MaskPanel from './MaskPanel';
import HistogramPanel, { type Histogram } from './HistogramPanel';
import { PanelCard, GroupLabel, ParamSlider, ParamToggle, ParamSegmented, PendingDot } from './panelKit';
import { PanelEntries } from './LeftRail';
import type { CurvePoint, EditParams, HslBand } from '../../../api/protocol';

/** 叶子总数（便于自检：与架构文档 93 对齐） */
export const TOTAL_LEAF_COUNT = 93;

/**
 * 真实渲染口径（来源：docs/param-leaves.json，引擎 parseCurve P0 修复后实测）：
 * 93 叶中预览管线渲染 39 项、导出管线渲染 27 项；其余随 v1.2.0 M1 求值器交付。
 * M1 交付后此常量替换为运行时读取 param-leaves.json。
 */
export const PENDING_RENDER_COUNT: { preview: number; export: number } = { preview: 39, export: 27 };
/** 提示文案：与 PENDING_RENDER_COUNT 同源（docs/param-leaves.json），改数字须两处同步 */
export const PENDING_RENDER_HINT = `预览管线已渲染 ${PENDING_RENDER_COUNT.preview} / ${TOTAL_LEAF_COUNT} 项参数，其余随 v1.2.0 M1 求值器交付`;

const HSL_BANDS: { value: HslBand; label: string }[] = [
  { value: 'red', label: '红' },
  { value: 'orange', label: '橙' },
  { value: 'yellow', label: '黄' },
  { value: 'green', label: '绿' },
  { value: 'aqua', label: '青' },
  { value: 'blue', label: '蓝' },
  { value: 'purple', label: '紫' },
  { value: 'magenta', label: '洋红' },
];

type CurveKey = 'toneCurveRGB' | 'toneCurveR' | 'toneCurveG' | 'toneCurveB';

const IDENTITY_CURVE: CurvePoint[] = [
  { x: 0, y: 0 },
  { x: 255, y: 255 },
];

function getIn(obj: unknown, path: string): number {
  let cur: unknown = obj;
  for (const k of path.split('.')) {
    if (cur === null || typeof cur !== 'object') return 0;
    cur = (cur as Record<string, unknown>)[k];
  }
  return typeof cur === 'number' ? cur : 0;
}

function fmtEv(v: number): string {
  return `${v > 0 ? '+' : ''}${Math.round(v * 100) / 100}`;
}

/** 参数行 → store.setPath（拖动不提交历史，抬起提交一次） */
function Row(props: { params: EditParams; path: string; label?: string; min: number; max: number; step?: number; defaultValue?: number; format?: (v: number) => string; pending?: boolean }) {
  const { params, path, label, min, max, step = 1, defaultValue = 0, format, pending } = props;
  return (
    <ParamSlider
      label={label ?? sliderLabel(path.split('.').pop() as keyof EditParams)}
      value={getIn(params, path)}
      min={min}
      max={max}
      step={step}
      defaultValue={defaultValue}
      format={format}
      pending={pending}
      onChange={(v) => void useDevelop.getState().setPath(path, v, false)}
      onCommit={(v) => void useDevelop.getState().setPath(path, v, true)}
    />
  );
}

export default function ProPanel({ histogram }: { histogram?: Histogram | null }) {
  const params = useDevelop((s) => s.params);
  if (!params) return null;

  return (
    <div className="h-full overflow-y-auto p-2 flex flex-col gap-1.5" style={{ background: 'var(--Lumina-Tokens-canvas)' }}>
      {/* 右上入口（02-6）：快照 / 图层 */}
      <PanelEntries />

      {/* 常驻诚实提示：不写未核实的数字（等引擎 param-leaves.json 给出事实后再显示两个真实数字） */}
      <div className="px-1 text-xs text-txt3 leading-5">
        {PENDING_RENDER_HINT}
      </div>

      <BasicGroup params={params} />
      <CurveGroup params={params} />
      <ColorGroup params={params} />
      <DetailGroup params={params} />
      <LensGroup params={params} />
      <GeometryGroup params={params} />
      <EffectsGroup params={params} />
      <CalibrationGroup params={params} />

      {/* 02-B3：已建蒙版必须常驻可见（设计依据 LR 蒙版面板常驻列出），默认展开 */}
      <PanelCard title="蒙版" defaultOpen hint="局部调整 · 已建列表常驻">
        <MaskPanel />
      </PanelCard>

      {/* 02-5：直方图移到右栏下部，RGB 三色叠（chartRed/Green/Blue 令牌） */}
      <PanelCard title="直方图" defaultOpen hint="RGB 叠加 + 亮度">
        <HistogramPanel histogram={histogram ?? null} bare />
      </PanelCard>
    </div>
  );
}

// ---------- 基础（14 叶） ----------

function BasicGroup({ params }: { params: EditParams }) {
  const applyPatch = useDevelop((s) => s.applyPatch);
  return (
    <PanelCard title="基础" defaultOpen hint="14 项">
      <div className="px-2 pb-1 flex items-center gap-2">
        <ParamSegmented
          value={params.whiteBalance}
          onChange={(v) => void applyPatch({ whiteBalance: v }, true, `白平衡 · ${v === 'asShot' ? '拍摄时' : '自定义'}`)}
          options={[
            { value: 'asShot' as const, label: '拍摄时' },
            { value: 'custom' as const, label: '自定义' },
          ]}
        />
        <span className="num text-2xs text-txt3">{params.whiteBalance === 'custom' ? `${params.temperature ?? 5500}K` : 'As Shot'}</span>
      </div>
      <Row params={params} path="exposure" min={-5} max={5} step={0.01} format={fmtEv} />
      <Row params={params} path="contrast" min={-100} max={100} />
      <Row params={params} path="highlights" min={-100} max={100} />
      <Row params={params} path="shadows" min={-100} max={100} />
      <Row params={params} path="whites" min={-100} max={100} />
      <Row params={params} path="blacks" min={-100} max={100} />
      <Row params={params} path="temperature" label="色温" min={2000} max={12000} defaultValue={5500} format={(v) => `${Math.round(v)}K`} />
      <Row params={params} path="tint" label="色调" min={-100} max={100} />
      <Row params={params} path="vibrance" label="自然饱和度" min={-100} max={100} />
      <Row params={params} path="saturation" label="饱和度" min={-100} max={100} />
      <Row params={params} path="texture" label="纹理" min={-100} max={100} pending />
      <Row params={params} path="clarity" label="清晰度" min={-100} max={100} />
      <Row params={params} path="dehaze" label="去雾" min={-100} max={100} />
    </PanelCard>
  );
}

// ---------- 曲线（11 叶：复合曲线 4 + 参数曲线 7） ----------

function CurveGroup({ params }: { params: EditParams }) {
  const [which, setWhich] = useState<CurveKey>('toneCurveRGB');
  const pts = params[which] ?? IDENTITY_CURVE;
  return (
    <PanelCard title="曲线" defaultOpen hint="11 项">
      <div className="px-2 pb-1">
        <ParamSegmented
          value={which}
          onChange={setWhich}
          options={[
            { value: 'toneCurveRGB' as CurveKey, label: '复合' },
            { value: 'toneCurveR' as CurveKey, label: 'R' },
            { value: 'toneCurveG' as CurveKey, label: 'G' },
            { value: 'toneCurveB' as CurveKey, label: 'B' },
          ]}
        />
        {which !== 'toneCurveRGB' && (
          <span className="ml-2 align-middle"><PendingDot /></span>
        )}
      </div>
      <div className="px-2">
        <CurveEditor
          points={pts}
          onChange={(next) => void useDevelop.getState().applyPatch({ [which]: next } as Partial<EditParams>, false)}
        />
      </div>
      <div className="text-2xs text-txt3 px-3 pt-1">拖动调整 · 单击添加点 · 右键删除</div>

      <GroupLabel>
        参数曲线 <span className="align-middle"><PendingDot /></span>
      </GroupLabel>
      <Row params={params} path="parametric.highlights" label="高光" min={-100} max={100} pending />
      <Row params={params} path="parametric.lights" label="亮调" min={-100} max={100} pending />
      <Row params={params} path="parametric.darks" label="暗调" min={-100} max={100} pending />
      <Row params={params} path="parametric.shadows" label="阴影" min={-100} max={100} pending />
      <Row params={params} path="parametric.shadowSplit" label="阴影分割" min={0} max={100} defaultValue={25} pending />
      <Row params={params} path="parametric.lightSplit" label="亮调分割" min={0} max={100} defaultValue={50} pending />
      <Row params={params} path="parametric.midSplit" label="中间分割" min={0} max={100} defaultValue={75} pending />
    </PanelCard>
  );
}

// ---------- 色彩（32 叶：HSL 24 + 颜色分级 8） ----------

function ColorGroup({ params }: { params: EditParams }) {
  const [band, setBand] = useState<HslBand>('red');
  const hsl = params.hsl[band];
  return (
    <PanelCard title="色彩" defaultOpen={false} hint="32 项">
      <GroupLabel>混色器 · 8 色带 × 色相/饱和/明度</GroupLabel>
      <div className="px-2 pb-1 flex flex-wrap gap-1">
        {HSL_BANDS.map((b) => {
          const active = band === b.value;
          return (
            <button
              key={b.value}
              onClick={() => setBand(b.value)}
              className="h-[22px] px-2 rounded-pill text-2xs transition-colors"
              style={{
                background: active ? 'var(--Lumina-Tokens-accentSoft)' : 'var(--Lumina-Tokens-card)',
                color: active ? 'var(--Lumina-Tokens-accent)' : 'var(--Lumina-Tokens-textSecondary)',
                boxShadow: active ? 'inset 0 0 0 1px color-mix(in srgb, var(--Lumina-Tokens-accent) 40%, transparent)' : 'inset 0 0 0 1px var(--Lumina-Tokens-border)',
                transitionTimingFunction: 'cubic-bezier(.2,.8,.2,1)',
              }}
            >
              {b.label}
            </button>
          );
        })}
      </div>
      {(['hue', 'sat', 'lum'] as const).map((k) => (
        <ParamSlider
          key={k}
          label={{ hue: '色相', sat: '饱和度', lum: '明亮度' }[k]}
          value={hsl[k]}
          min={-100}
          max={100}
          defaultValue={0}
          onChange={(v) => void useDevelop.getState().setHsl(band, k, v, false)}
          onCommit={(v) => void useDevelop.getState().setHsl(band, k, v, true)}
        />
      ))}

      <GroupLabel>
        颜色分级 <span className="align-middle"><PendingDot /></span>
      </GroupLabel>
      <Row params={params} path="colorGrade.shadowHue" label="阴影 色相" min={-100} max={100} pending />
      <Row params={params} path="colorGrade.shadowSat" label="阴影 饱和" min={-100} max={100} pending />
      <Row params={params} path="colorGrade.midtoneHue" label="中间调 色相" min={-100} max={100} pending />
      <Row params={params} path="colorGrade.midtoneSat" label="中间调 饱和" min={-100} max={100} pending />
      <Row params={params} path="colorGrade.highlightHue" label="高光 色相" min={-100} max={100} pending />
      <Row params={params} path="colorGrade.highlightSat" label="高光 饱和" min={-100} max={100} pending />
      <Row params={params} path="colorGrade.blend" label="混合" min={0} max={100} defaultValue={50} pending />
      <Row params={params} path="colorGrade.balance" label="平衡" min={-100} max={100} pending />
    </PanelCard>
  );
}

// ---------- 细节（7 叶） ----------

function DetailGroup({ params }: { params: EditParams }) {
  return (
    <PanelCard title="细节" defaultOpen={false} hint="7 项 · 未渲染">
      <GroupLabel>锐化</GroupLabel>
      <Row params={params} path="detail.sharpness" label="数量" min={0} max={150} defaultValue={25} pending />
      <Row params={params} path="detail.sharpenRadius" label="半径" min={0.5} max={3} step={0.1} defaultValue={1} format={(v) => v.toFixed(1)} pending />
      <Row params={params} path="detail.sharpenDetail" label="细节" min={0} max={100} defaultValue={25} pending />
      <Row params={params} path="detail.sharpenMasking" label="蒙版" min={0} max={100} pending />
      <GroupLabel>降噪</GroupLabel>
      <Row params={params} path="detail.luminanceSmoothing" label="明亮度" min={0} max={100} pending />
      <Row params={params} path="detail.colorNR" label="颜色" min={0} max={100} defaultValue={25} pending />
      <Row params={params} path="detail.colorNRDetail" label="颜色细节" min={0} max={100} defaultValue={50} pending />
    </PanelCard>
  );
}

// ---------- 镜头（2 叶） ----------

function LensGroup({ params }: { params: EditParams }) {
  return (
    <PanelCard title="镜头" defaultOpen={false} hint="2 项 · 未渲染">
      <ParamToggle
        label="启用镜头配置文件"
        checked={params.optics.lensProfileEnable}
        onChange={(v) => void useDevelop.getState().setPath('optics.lensProfileEnable', v, true, v ? '启用镜头配置文件' : '停用镜头配置文件')}
        pending
      />
      <Row params={params} path="optics.manualDistortion" label="扭曲校正" min={-100} max={100} pending />
      <div className="text-2xs text-txt3 px-3 pt-1 leading-4">配置文件按 EXIF 镜头型号匹配；扭曲校正在配置文件基础上叠加。</div>
    </PanelCard>
  );
}

// ---------- 几何（13 叶：裁剪 6 + 透视 7） ----------

function GeometryGroup({ params }: { params: EditParams }) {
  const g = params.geometry;
  return (
    <PanelCard title="几何" defaultOpen={false} hint="13 项 · 未渲染">
      <GroupLabel>裁剪</GroupLabel>
      <ParamToggle label="启用裁剪框" checked={g.hasCrop} onChange={(v) => void useDevelop.getState().setPath('geometry.hasCrop', v, true, v ? '开启裁剪' : '关闭裁剪')} pending />
      <Row params={params} path="geometry.cropTop" label="上边距" min={0} max={50} pending />
      <Row params={params} path="geometry.cropBottom" label="下边距" min={0} max={50} pending />
      <Row params={params} path="geometry.cropLeft" label="左边距" min={0} max={50} pending />
      <Row params={params} path="geometry.cropRight" label="右边距" min={0} max={50} pending />
      <Row params={params} path="geometry.cropAngle" label="拉直角度" min={-10} max={10} step={0.1} format={(v) => `${v.toFixed(1)}°`} pending />
      <GroupLabel>透视</GroupLabel>
      <Row params={params} path="geometry.perspective.vertical" label="垂直校正" min={-100} max={100} pending />
      <Row params={params} path="geometry.perspective.horizontal" label="水平校正" min={-100} max={100} pending />
      <Row params={params} path="geometry.perspective.rotate" label="旋转" min={-45} max={45} step={0.5} pending />
      <Row params={params} path="geometry.perspective.scale" label="缩放" min={0.5} max={2} step={0.01} defaultValue={1} format={(v) => `${v.toFixed(2)}×`} pending />
      <Row params={params} path="geometry.perspective.aspect" label="宽高比" min={-100} max={100} pending />
      <Row params={params} path="geometry.perspective.x" label="X 偏移" min={-100} max={100} pending />
      <Row params={params} path="geometry.perspective.y" label="Y 偏移" min={-100} max={100} pending />
      <div className="text-2xs text-txt3 px-3 pt-1 leading-4">宽高比与参考线快捷操作在画面下方「变换」工具条。</div>
    </PanelCard>
  );
}

// ---------- 效果（6 叶：暗角 3 + 颗粒 3） ----------

function EffectsGroup({ params }: { params: EditParams }) {
  return (
    <PanelCard title="效果" defaultOpen={false} hint="6 项">
      <GroupLabel>暗角</GroupLabel>
      <Row params={params} path="optics.vignetteAmount" label="数量" min={-100} max={100} />
      <Row params={params} path="optics.vignetteMidpoint" label="中点" min={0} max={100} defaultValue={50} />
      <Row params={params} path="optics.vignetteFeather" label="羽化" min={0} max={100} defaultValue={50} />
      <GroupLabel>颗粒</GroupLabel>
      <Row params={params} path="optics.grainAmount" label="数量" min={0} max={100} />
      <Row params={params} path="optics.grainSize" label="大小" min={0} max={100} defaultValue={25} />
      <Row params={params} path="optics.grainFrequency" label="频率" min={0} max={100} defaultValue={50} />
    </PanelCard>
  );
}

// ---------- 校准（7 叶） ----------

function CalibrationGroup({ params }: { params: EditParams }) {
  return (
    <PanelCard title="校准" defaultOpen={false} hint="7 项 · 未渲染">
      <Row params={params} path="calibration.shadowTint" label="阴影色调" min={-100} max={100} pending />
      <GroupLabel>原色</GroupLabel>
      <Row params={params} path="calibration.redHue" label="红 色相" min={-100} max={100} pending />
      <Row params={params} path="calibration.redSat" label="红 饱和" min={-100} max={100} pending />
      <Row params={params} path="calibration.greenHue" label="绿 色相" min={-100} max={100} pending />
      <Row params={params} path="calibration.greenSat" label="绿 饱和" min={-100} max={100} pending />
      <Row params={params} path="calibration.blueHue" label="蓝 色相" min={-100} max={100} pending />
      <Row params={params} path="calibration.blueSat" label="蓝 饱和" min={-100} max={100} pending />
    </PanelCard>
  );
}
