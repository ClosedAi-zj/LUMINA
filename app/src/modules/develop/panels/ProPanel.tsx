/**
 * 修图工作区右栏（v1.2.0 阶段 3 皮肤；M2b-2 G5 dock 化）：8 参数组 dock 标签组 + 蒙版/直方图常驻卡片。
 *
 * 叶数归属（与架构文档 §4 的 93 叶表一致，逐组标注）：
 *   基础 14 ｜ 曲线 11 ｜ 色彩 32 ｜ 细节 7 ｜ 镜头 2 ｜ 几何 13 ｜ 效果 6 ｜ 校准 7 ｜ 蒙版 1（容器）
 * 合计 14+11+32+7+2+13+6+7+1 = 93。
 *
 * v1.2.0 M1 起：引擎 GPU 管线重建为 ops 多 pass，93/93 叶全部参与渲染
 * （CPU/GPU parity 全绿），此前的「未参与渲染」提示与 PendingDot 占位已整体移除。
 */

import { useEffect, useRef, useState } from 'react';
import { useDevelop, sliderLabel, temperatureToKelvin } from '../../../store/developStore';
import CurveEditor from '../CurveEditor';
import MaskPanel from './MaskPanel';
import HistogramPanel, { type Histogram } from './HistogramPanel';
import { PanelCard, GroupLabel, ParamSlider, ParamToggle, ParamSegmented } from './panelKit';
import { LuIcon } from '../../../components/icons';
import type { CurvePoint, EditParams, HslBand } from '../../../api/protocol';

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

/** 参数行 → store.setPath（拖动不提交历史，抬起提交一次）。
 *  temperature 单位契约：store 内为归一化值，滑杆值经 temperatureToKelvin 还原为开尔文显示，
 *  滑杆输出的开尔文大值由 store.applyPatch 归一化后落盘。 */
function Row(props: { params: EditParams; path: string; label?: string; min: number; max: number; step?: number; defaultValue?: number; format?: (v: number) => string }) {
  const { params, path, label, min, max, step = 1, defaultValue = 0, format } = props;
  const isKelvin = path === 'temperature';
  const raw = getIn(params, path);
  return (
    <ParamSlider
      label={label ?? sliderLabel(path.split('.').pop() as keyof EditParams)}
      value={isKelvin ? temperatureToKelvin(raw) : raw}
      min={min}
      max={max}
      step={step}
      defaultValue={defaultValue}
      format={format}
      onChange={(v) => void useDevelop.getState().setPath(path, v, false)}
      onCommit={(v) => void useDevelop.getState().setPath(path, v, true)}
    />
  );
}

/**
 * G5 dock 标签组（R6 v2-B5，PS 右侧 dock 同组切换形态）：
 * 8 个参数组归入一个 dock，标签切换；**全部组保持挂载**（display:none 隐藏），
 * 切 tab 不丢面板状态（展开态 / 组内选择态 / 未提交输入）；
 * 列滚动位置在切换前后保存恢复（验收断言点）。
 * 蒙版 / 直方图不进 dock：02-B3 设计约定「已建蒙版必须常驻可见」，维持常驻卡片。
 */
/** v1.3.0-m3 图标化：dock 标签 = 图标 + 短标题（PS dock 形态；图标经 icons.tsx 统一映射） */
const DOCK_TABS: { id: string; label: string; icon: string }[] = [
  { id: 'basic', label: '基础', icon: 'basic' },
  { id: 'curve', label: '曲线', icon: 'curve' },
  { id: 'color', label: '色彩', icon: 'color' },
  { id: 'detail', label: '细节', icon: 'detail' },
  { id: 'lens', label: '镜头', icon: 'lens' },
  { id: 'geometry', label: '几何', icon: 'geometry' },
  { id: 'effects', label: '效果', icon: 'effects' },
  { id: 'calibration', label: '校准', icon: 'calibration' },
];

export default function ProPanel({ histogram }: { histogram?: Histogram | null }) {
  const params = useDevelop((s) => s.params);
  const [activeTab, setActiveTab] = useState('basic');
  const colRef = useRef<HTMLDivElement>(null);
  const scrollTopRef = useRef(0);
  const switchTab = (id: string) => {
    scrollTopRef.current = colRef.current?.scrollTop ?? 0;
    setActiveTab(id);
  };
  // 切 tab 恢复列滚动位置：面板状态不许因切换被甩走
  useEffect(() => {
    if (colRef.current) colRef.current.scrollTop = scrollTopRef.current;
  }, [activeTab]);

  if (!params) return null;

  return (
    <div ref={colRef} className="h-full overflow-y-auto flex flex-col" style={{ background: 'transparent' }}>
      {/* G5 dock：8 参数组标签组（图标化 dock 标签；组保持挂载，切 tab 不丢状态）
          v1.4.1：dock 与下方参数组同属一列，段与段之间只用一根发丝线，不再各自套框 */}
      <div className="shrink-0 border-b border-line">
        {/* v1.4.1：4 列网格代替 flex-wrap —— 318px 右栏里 8 个 tab 会折成 5+3 的错位两行，
            网格化后固定 4+4，行宽均分、视觉基线一致 */}
        <div className="grid grid-cols-4 gap-0.5 px-1 py-1" role="tablist" aria-label="调整面板 dock">
          {DOCK_TABS.map((t) => {
            const active = activeTab === t.id;
            return (
              <button
                key={t.id}
                role="tab"
                aria-selected={active}
                onClick={() => switchTab(t.id)}
                title={t.label}
                className={`inline-flex items-center justify-center gap-1 h-7 px-1 rounded-smC text-2xs transition-colors ${
                  active ? 'text-txt bg-hover font-medium' : 'text-txt2 hover:text-txt hover:bg-hover'
                }`}
                style={active ? { boxShadow: 'inset 0 -2px 0 var(--Lumina-Tokens-accent)' } : undefined}
              >
                <LuIcon name={t.icon} size={13} className="shrink-0" />
                <span className="truncate">{t.label}</span>
              </button>
            );
          })}
        </div>
        <div className="flex flex-col">
          {/* inactive 组 display:none 但保持挂载：展开态/选择态/输入不丢 */}
          <div style={{ display: activeTab === 'basic' ? undefined : 'none' }}><BasicGroup params={params} /></div>
          <div style={{ display: activeTab === 'curve' ? undefined : 'none' }}><CurveGroup params={params} /></div>
          <div style={{ display: activeTab === 'color' ? undefined : 'none' }}><ColorGroup params={params} /></div>
          <div style={{ display: activeTab === 'detail' ? undefined : 'none' }}><DetailGroup params={params} /></div>
          <div style={{ display: activeTab === 'lens' ? undefined : 'none' }}><LensGroup params={params} /></div>
          <div style={{ display: activeTab === 'geometry' ? undefined : 'none' }}><GeometryGroup params={params} /></div>
          <div style={{ display: activeTab === 'effects' ? undefined : 'none' }}><EffectsGroup params={params} /></div>
          <div style={{ display: activeTab === 'calibration' ? undefined : 'none' }}><CalibrationGroup params={params} /></div>
        </div>
      </div>

      {/* 02-B3：已建蒙版必须常驻可见（设计依据 LR 蒙版面板常驻列出），默认展开 */}
      <PanelCard title="蒙版" icon="mask" defaultOpen hint="局部调整 · 已建列表常驻">
        <MaskPanel />
      </PanelCard>

      {/* 02-5：直方图移到右栏下部，RGB 三色叠（chartRed/Green/Blue 令牌） */}
      <PanelCard title="直方图" icon="histogram" defaultOpen hint="RGB 叠加 + 亮度">
        <HistogramPanel histogram={histogram ?? null} bare />
      </PanelCard>
    </div>
  );
}

// ---------- 基础（14 叶） ----------

function BasicGroup({ params }: { params: EditParams }) {
  const applyPatch = useDevelop((s) => s.applyPatch);
  return (
    <PanelCard title="基础" icon="basic" defaultOpen hint="14 项">
      <div className="px-2 pb-1 flex items-center gap-2">
        <ParamSegmented
          value={params.whiteBalance}
          onChange={(v) => void applyPatch({ whiteBalance: v }, true, `白平衡 · ${v === 'asShot' ? '拍摄时' : '自定义'}`)}
          options={[
            { value: 'asShot' as const, label: '拍摄时' },
            { value: 'custom' as const, label: '自定义' },
          ]}
        />
        {/* v1.4.1：去掉恒显的「As Shot」英文读数——分段控件选中「拍摄时」时它只是同义重复；
            仅自定义时显示开尔文值，读数才有信息量 */}
        {params.whiteBalance === 'custom' && (
          <span className="num text-2xs text-txt3">{Math.round(temperatureToKelvin(params.temperature ?? 0))}K</span>
        )}
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
      <Row params={params} path="texture" label="纹理" min={-100} max={100} />
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
    <PanelCard title="曲线" icon="curve" defaultOpen hint="11 项">
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
          <span className="ml-2 align-middle text-2xs text-txt3">通道曲线</span>
        )}
      </div>
      <div className="px-2">
        <CurveEditor
          points={pts}
          onChange={(next) => void useDevelop.getState().applyPatch({ [which]: next } as Partial<EditParams>, false)}
        />
      </div>
      <div className="text-2xs text-txt3 px-3 pt-1">拖动调整 · 单击添加点 · 右键删除</div>

      <GroupLabel>参数曲线</GroupLabel>
      <Row params={params} path="parametric.highlights" label="高光" min={-100} max={100} />
      <Row params={params} path="parametric.lights" label="亮调" min={-100} max={100} />
      <Row params={params} path="parametric.darks" label="暗调" min={-100} max={100} />
      <Row params={params} path="parametric.shadows" label="阴影" min={-100} max={100} />
      <Row params={params} path="parametric.shadowSplit" label="阴影分割" min={0} max={100} defaultValue={25} />
      <Row params={params} path="parametric.lightSplit" label="亮调分割" min={0} max={100} defaultValue={50} />
      <Row params={params} path="parametric.midSplit" label="中间分割" min={0} max={100} defaultValue={75} />
    </PanelCard>
  );
}

// ---------- 色彩（32 叶：HSL 24 + 颜色分级 8） ----------

function ColorGroup({ params }: { params: EditParams }) {
  const [band, setBand] = useState<HslBand>('red');
  const hsl = params.hsl[band];
  return (
    <PanelCard title="色彩" icon="droplet" defaultOpen={false} hint="32 项">
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
        颜色分级
      </GroupLabel>
      <Row params={params} path="colorGrade.shadowHue" label="阴影 色相" min={-100} max={100} />
      <Row params={params} path="colorGrade.shadowSat" label="阴影 饱和" min={-100} max={100} />
      <Row params={params} path="colorGrade.midtoneHue" label="中间调 色相" min={-100} max={100} />
      <Row params={params} path="colorGrade.midtoneSat" label="中间调 饱和" min={-100} max={100} />
      <Row params={params} path="colorGrade.highlightHue" label="高光 色相" min={-100} max={100} />
      <Row params={params} path="colorGrade.highlightSat" label="高光 饱和" min={-100} max={100} />
      <Row params={params} path="colorGrade.blend" label="混合" min={0} max={100} defaultValue={50} />
      <Row params={params} path="colorGrade.balance" label="平衡" min={-100} max={100} />
    </PanelCard>
  );
}

// ---------- 细节（7 叶） ----------

function DetailGroup({ params }: { params: EditParams }) {
  return (
    <PanelCard title="细节" icon="detail" defaultOpen={false} hint="7 项">
      <GroupLabel>锐化</GroupLabel>
      <Row params={params} path="detail.sharpness" label="数量" min={0} max={150} defaultValue={25} />
      <Row params={params} path="detail.sharpenRadius" label="半径" min={0.5} max={3} step={0.1} defaultValue={1} format={(v) => v.toFixed(1)} />
      <Row params={params} path="detail.sharpenDetail" label="细节" min={0} max={100} defaultValue={25} />
      <Row params={params} path="detail.sharpenMasking" label="蒙版" min={0} max={100} />
      <GroupLabel>降噪</GroupLabel>
      <Row params={params} path="detail.luminanceSmoothing" label="明亮度" min={0} max={100} />
      <Row params={params} path="detail.colorNR" label="颜色" min={0} max={100} defaultValue={25} />
      <Row params={params} path="detail.colorNRDetail" label="颜色细节" min={0} max={100} defaultValue={50} />
    </PanelCard>
  );
}

// ---------- 镜头（3 叶：配置文件 + 畸变 + CA 强度叶 #94，T2.5/Q3） ----------

function LensGroup({ params }: { params: EditParams }) {
  return (
    <PanelCard title="镜头" icon="lens" defaultOpen={false} hint="3 项">
      <ParamToggle
        label="启用镜头配置文件"
        checked={params.optics.lensProfileEnable}
        onChange={(v) => void useDevelop.getState().setPath('optics.lensProfileEnable', v, true, v ? '启用镜头配置文件' : '停用镜头配置文件')}
 />
      <Row params={params} path="optics.manualDistortion" label="扭曲校正" min={-100} max={100} />
      {/* 叶 #94（T2.5/Q3）：色差强度 ↔ crs:ChromaticAberrationScale；渲染实装在引擎 lens 算子侧 */}
      <Row params={params} path="optics.chromaticAberration" label="去色差" min={-100} max={100} />
      <div className="text-2xs text-txt3 px-3 pt-1 leading-4">配置文件按 EXIF 镜头型号匹配；扭曲校正在配置文件基础上叠加；去色差为手动强度入口。</div>
    </PanelCard>
  );
}

// ---------- 几何（13 叶：裁剪 6 + 透视 7） ----------

function GeometryGroup({ params }: { params: EditParams }) {
  const g = params.geometry;
  return (
    <PanelCard title="几何" icon="geometry" defaultOpen={false} hint="13 项">
      <GroupLabel>裁剪</GroupLabel>
      <ParamToggle label="启用裁剪框" checked={g.hasCrop} onChange={(v) => void useDevelop.getState().setPath('geometry.hasCrop', v, true, v ? '开启裁剪' : '关闭裁剪')} />
      <Row params={params} path="geometry.cropTop" label="上边距" min={0} max={50} />
      <Row params={params} path="geometry.cropBottom" label="下边距" min={0} max={50} />
      <Row params={params} path="geometry.cropLeft" label="左边距" min={0} max={50} />
      <Row params={params} path="geometry.cropRight" label="右边距" min={0} max={50} />
      <Row params={params} path="geometry.cropAngle" label="拉直角度" min={-10} max={10} step={0.1} format={(v) => `${v.toFixed(1)}°`} />
      <GroupLabel>透视</GroupLabel>
      <Row params={params} path="geometry.perspective.vertical" label="垂直校正" min={-100} max={100} />
      <Row params={params} path="geometry.perspective.horizontal" label="水平校正" min={-100} max={100} />
      <Row params={params} path="geometry.perspective.rotate" label="旋转" min={-45} max={45} step={0.5} />
      <Row params={params} path="geometry.perspective.scale" label="缩放" min={0.5} max={2} step={0.01} defaultValue={1} format={(v) => `${v.toFixed(2)}×`} />
      <Row params={params} path="geometry.perspective.aspect" label="宽高比" min={-100} max={100} />
      <Row params={params} path="geometry.perspective.x" label="X 偏移" min={-100} max={100} />
      <Row params={params} path="geometry.perspective.y" label="Y 偏移" min={-100} max={100} />
      <div className="text-2xs text-txt3 px-3 pt-1 leading-4">宽高比与参考线快捷操作在画面下方「变换」工具条。</div>
    </PanelCard>
  );
}

// ---------- 效果（6 叶：暗角 3 + 颗粒 3） ----------

function EffectsGroup({ params }: { params: EditParams }) {
  return (
    <PanelCard title="效果" icon="effects" defaultOpen={false} hint="6 项">
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
    <PanelCard title="校准" icon="calibration" defaultOpen={false} hint="7 项">
      <Row params={params} path="calibration.shadowTint" label="阴影色调" min={-100} max={100} />
      <GroupLabel>原色</GroupLabel>
      <Row params={params} path="calibration.redHue" label="红 色相" min={-100} max={100} />
      <Row params={params} path="calibration.redSat" label="红 饱和" min={-100} max={100} />
      <Row params={params} path="calibration.greenHue" label="绿 色相" min={-100} max={100} />
      <Row params={params} path="calibration.greenSat" label="绿 饱和" min={-100} max={100} />
      <Row params={params} path="calibration.blueHue" label="蓝 色相" min={-100} max={100} />
      <Row params={params} path="calibration.blueSat" label="蓝 饱和" min={-100} max={100} />
    </PanelCard>
  );
}
