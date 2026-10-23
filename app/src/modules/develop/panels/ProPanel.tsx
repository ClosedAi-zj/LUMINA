/**
 * 专业模式右侧 7 标签调整面板（设计稿 10 右栏）：
 * 光 / 色 / 细节 / 效果 / 校准 / 镜头 / 几何。
 * 参数 = 同一 EditParams（与快速模式共享，点路径经 setPath 落盘）。
 */

import { useState } from 'react';
import { useDevelop, sliderLabel } from '../../../store/developStore';
import { Slider, Segmented, Toggle } from '../../../components/primitives';
import CurveEditor from '../CurveEditor';
import type { CurvePoint, EditParams, HslBand } from '../../../api/protocol';

type ProTab = 'light' | 'color' | 'detail' | 'effects' | 'calibration' | 'lens' | 'geometry';

const TABS: { value: ProTab; label: string }[] = [
  { value: 'light', label: '光' },
  { value: 'color', label: '色' },
  { value: 'detail', label: '细节' },
  { value: 'effects', label: '效果' },
  { value: 'calibration', label: '校准' },
  { value: 'lens', label: '镜头' },
  { value: 'geometry', label: '几何' },
];

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

const SECTION = 'text-2xs text-txt3 mb-1.5 mt-3 tracking-wide first:mt-0';

export default function ProPanel() {
  const params = useDevelop((s) => s.params);
  const [tab, setTab] = useState<ProTab>('light');

  if (!params) return null;

  return (
    <div className="p-3 overflow-y-auto text-xs h-full">
      <Segmented value={tab} onChange={setTab} options={TABS} />

      {tab === 'light' && <LightTab params={params} />}
      {tab === 'color' && <ColorTab params={params} />}
      {tab === 'detail' && <DetailTab params={params} />}
      {tab === 'effects' && <EffectsTab params={params} />}
      {tab === 'calibration' && <CalibrationTab params={params} />}
      {tab === 'lens' && <LensTab params={params} />}
      {tab === 'geometry' && <GeometryTab params={params} />}
    </div>
  );
}

/** 读取点路径当前值 */
function getIn(obj: unknown, path: string): number {
  let cur: unknown = obj;
  for (const k of path.split('.')) {
    if (cur === null || typeof cur !== 'object') return 0;
    cur = (cur as Record<string, unknown>)[k];
  }
  return typeof cur === 'number' ? cur : 0;
}

function PSlider({ params, path, label, min, max, step = 1, defaultValue = 0, format }: {
  params: EditParams;
  path: string;
  label?: string;
  min: number;
  max: number;
  step?: number;
  defaultValue?: number;
  format?: (v: number) => string;
}) {
  return (
    <Slider
      label={label ?? sliderLabel(path.split('.').pop() as keyof EditParams)}
      value={getIn(params, path)}
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

function fmtEv(v: number): string {
  return `${v > 0 ? '+' : ''}${Math.round(v * 100) / 100}`;
}

/**
 * 落盘提示条：真引擎 edit.setParams 接受这些分组并回显，但 XMP 写入器
 * 目前只持久化扁平 crs:* 字段 + lumina:Masks + lumina:AdjustmentStack +
 * crs:ToneCurvePV2012，分组字段（colorGrade/detail/optics/calibration/
 * geometry/hsl）重开照片会丢失。这里显式告知，不做静默。
 */
function PersistGap({ what }: { what: string }) {
  return (
    <div
      className="mt-3 px-2 py-1.5 rounded-sm border border-line text-2xs text-txt3 leading-4"
      style={{ background: 'var(--Lumina-Tokens-card)' }}
    >
      落盘提示：{what} 当前只存在于引擎内存会话，重开照片会回默认值（引擎 XMP 写入器尚未持久化该分组）。已上报协议缺口。
    </div>
  );
}

// ---------- 光 ----------

function LightTab({ params }: { params: EditParams }) {
  const [curvePts, setCurvePts] = useState<CurvePoint[]>(params.toneCurveRGB);
  return (
    <div>
      <div className={SECTION}>基本 · 明暗</div>
      <PSlider params={params} path="exposure" min={-5} max={5} step={0.01} format={fmtEv} />
      <PSlider params={params} path="contrast" min={-100} max={100} />
      <PSlider params={params} path="highlights" min={-100} max={100} />
      <PSlider params={params} path="shadows" min={-100} max={100} />
      <PSlider params={params} path="whites" min={-100} max={100} />
      <PSlider params={params} path="blacks" min={-100} max={100} />

      <div className={SECTION}>色调曲线 · 点曲线</div>
      <CurveEditor
        points={curvePts}
        onChange={(pts) => {
          setCurvePts(pts);
          void useDevelop.getState().applyPatch({ toneCurveRGB: pts }, false);
        }}
      />
      <div className="text-2xs text-txt3 mt-1">拖动调整 · 单击添加点 · 右键删除</div>
    </div>
  );
}

// ---------- 色 ----------

function ColorTab({ params }: { params: EditParams }) {
  const [band, setBand] = useState<HslBand>('red');
  const applyPatch = useDevelop((s) => s.applyPatch);
  const hsl = params.hsl[band];

  return (
    <div>
      <div className={SECTION}>白平衡</div>
      <div className="flex items-center gap-2 mb-2">
        <Segmented
          value={params.whiteBalance}
          onChange={(v) => void applyPatch({ whiteBalance: v }, true, `白平衡 · ${v === 'asShot' ? '拍摄时' : '自定义'}`)}
          options={[
            { value: 'asShot' as const, label: '拍摄时' },
            { value: 'custom' as const, label: '自定义' },
          ]}
        />
        <span className="num text-2xs text-txt3">{params.whiteBalance === 'custom' ? `${params.temperature ?? 5500}K` : 'As Shot'}</span>
      </div>
      <PSlider params={params} path="temperature" label="色温" min={2000} max={12000} defaultValue={5500} format={(v) => `${v}K`} />
      <PSlider params={params} path="tint" label="色调" min={-100} max={100} />

      <div className={SECTION}>偏好</div>
      <PSlider params={params} path="vibrance" label="自然饱和度" min={-100} max={100} />
      <PSlider params={params} path="saturation" label="饱和度" min={-100} max={100} />

      <div className={SECTION}>混色器 · HSL</div>
      <div className="flex items-center gap-0.5 flex-wrap mb-2">
        {HSL_BANDS.map((b) => (
          <button
            key={b.value}
            onClick={() => setBand(b.value)}
            className={`h-5 px-1.5 rounded-sm border text-2xs transition-colors ${band === b.value ? 'border-accent text-accent' : 'border-line text-txt3 hover:text-txt2'}`}
            style={{ background: band === b.value ? 'var(--Lumina-Tokens-accentSoft)' : 'var(--Lumina-Tokens-card)' }}
          >
            {b.label}
          </button>
        ))}
      </div>
      {(['hue', 'sat', 'lum'] as const).map((k) => (
        <Slider
          key={k}
          label={`色相 / 饱和 / 明度 · ${{ hue: '色相', sat: '饱和度', lum: '明亮度' }[k]}`}
          value={hsl[k]}
          min={-100}
          max={100}
          defaultValue={0}
          onChange={(v) => void useDevelop.getState().setHsl(band, k, v, false)}
          onCommit={(v) => void useDevelop.getState().setHsl(band, k, v, true)}
        />
      ))}

      <div className={SECTION}>颜色分级 · Color Grade</div>
      <PSlider params={params} path="colorGrade.shadowHue" label="阴影 色相" min={-100} max={100} />
      <PSlider params={params} path="colorGrade.shadowSat" label="阴影 饱和" min={-100} max={100} />
      <PSlider params={params} path="colorGrade.midtoneHue" label="中间调 色相" min={-100} max={100} />
      <PSlider params={params} path="colorGrade.midtoneSat" label="中间调 饱和" min={-100} max={100} />
      <PSlider params={params} path="colorGrade.highlightHue" label="高光 色相" min={-100} max={100} />
      <PSlider params={params} path="colorGrade.highlightSat" label="高光 饱和" min={-100} max={100} />
      <PSlider params={params} path="colorGrade.blend" label="混合" min={0} max={100} defaultValue={50} />
      <PSlider params={params} path="colorGrade.balance" label="平衡" min={-100} max={100} />
      <PersistGap what="混色器 HSL 与颜色分级" />
    </div>
  );
}

// ---------- 细节 ----------

function DetailTab({ params }: { params: EditParams }) {
  return (
    <div>
      <div className={SECTION}>锐化</div>
      <PSlider params={params} path="detail.sharpness" label="数量" min={0} max={150} defaultValue={25} />
      <PSlider params={params} path="detail.sharpenRadius" label="半径" min={0.5} max={3} step={0.1} defaultValue={1} format={(v) => v.toFixed(1)} />
      <PSlider params={params} path="detail.sharpenDetail" label="细节" min={0} max={100} defaultValue={25} />
      <PSlider params={params} path="detail.sharpenMasking" label="蒙版" min={0} max={100} />
      <div className={SECTION}>降噪</div>
      <PSlider params={params} path="detail.luminanceSmoothing" label="明亮度" min={0} max={100} />
      <PSlider params={params} path="detail.colorNR" label="颜色" min={0} max={100} defaultValue={25} />
      <PSlider params={params} path="detail.colorNRDetail" label="颜色细节" min={0} max={100} defaultValue={50} />
      <PersistGap what="锐化与降噪" />
    </div>
  );
}

// ---------- 效果 ----------

function EffectsTab({ params }: { params: EditParams }) {
  return (
    <div>
      <div className={SECTION}>质感</div>
      <PSlider params={params} path="clarity" label="清晰度" min={-100} max={100} />
      <PSlider params={params} path="texture" label="纹理" min={-100} max={100} />
      <PSlider params={params} path="dehaze" label="去雾" min={-100} max={100} />
      <div className={SECTION}>暗角</div>
      <PSlider params={params} path="optics.vignetteAmount" label="数量" min={-100} max={100} />
      <PSlider params={params} path="optics.vignetteMidpoint" label="中点" min={0} max={100} defaultValue={50} />
      <PSlider params={params} path="optics.vignetteFeather" label="羽化" min={0} max={100} defaultValue={50} />
      <div className={SECTION}>颗粒</div>
      <PSlider params={params} path="optics.grainAmount" label="数量" min={0} max={100} />
      <PSlider params={params} path="optics.grainSize" label="大小" min={0} max={100} defaultValue={25} />
      <PSlider params={params} path="optics.grainFrequency" label="频率" min={0} max={100} defaultValue={50} />
      <PersistGap what="暗角与颗粒（清晰度/纹理/去雾为扁平字段，正常落盘）" />
    </div>
  );
}

// ---------- 校准 ----------

function CalibrationTab({ params }: { params: EditParams }) {
  return (
    <div>
      <div className={SECTION}>相机校准</div>
      <PSlider params={params} path="calibration.shadowTint" label="阴影色调" min={-100} max={100} />
      <div className={SECTION}>原色 · 红</div>
      <PSlider params={params} path="calibration.redHue" label="色相" min={-100} max={100} />
      <PSlider params={params} path="calibration.redSat" label="饱和度" min={-100} max={100} />
      <div className={SECTION}>原色 · 绿</div>
      <PSlider params={params} path="calibration.greenHue" label="色相" min={-100} max={100} />
      <PSlider params={params} path="calibration.greenSat" label="饱和度" min={-100} max={100} />
      <div className={SECTION}>原色 · 蓝</div>
      <PSlider params={params} path="calibration.blueHue" label="色相" min={-100} max={100} />
      <PSlider params={params} path="calibration.blueSat" label="饱和度" min={-100} max={100} />
      <PersistGap what="相机校准" />
    </div>
  );
}

// ---------- 镜头 ----------

function LensTab({ params }: { params: EditParams }) {
  return (
    <div>
      <div className={SECTION}>镜头校正</div>
      <div className="mb-2">
        <Toggle
          checked={params.optics.lensProfileEnable}
          onChange={(v) => void useDevelop.getState().setPath('optics.lensProfileEnable', v, true, v ? '启用镜头配置文件' : '停用镜头配置文件')}
          label="启用镜头配置文件"
        />
      </div>
      <PSlider params={params} path="optics.manualDistortion" label="手动扭曲校正" min={-100} max={100} />
      <div className="text-2xs text-txt3 mt-3 leading-4">配置文件按 EXIF 镜头型号自动匹配；扭曲校正滑条在配置文件基础上叠加。</div>
      <PersistGap what="镜头配置文件与扭曲校正" />
    </div>
  );
}

// ---------- 几何（详细版；工具条形态见 TransformBar） ----------

function GeometryTab({ params }: { params: EditParams }) {
  const g = params.geometry;
  return (
    <div>
      <div className={SECTION}>裁剪</div>
      <Toggle checked={g.hasCrop} onChange={(v) => void useDevelop.getState().setPath('geometry.hasCrop', v, true, v ? '开启裁剪' : '关闭裁剪')} label="启用裁剪框" />
      <PSlider params={params} path="geometry.cropAngle" label="拉直角度" min={-10} max={10} step={0.1} format={(v) => `${v.toFixed(1)}°`} />
      <div className={SECTION}>透视</div>
      <PSlider params={params} path="geometry.perspective.vertical" label="垂直校正" min={-100} max={100} />
      <PSlider params={params} path="geometry.perspective.horizontal" label="水平校正" min={-100} max={100} />
      <PSlider params={params} path="geometry.perspective.rotate" label="旋转" min={-45} max={45} step={0.5} />
      <PSlider params={params} path="geometry.perspective.scale" label="缩放" min={0.5} max={2} step={0.01} defaultValue={1} format={(v) => `${v.toFixed(2)}×`} />
      <PSlider params={params} path="geometry.perspective.x" label="X 偏移" min={-100} max={100} />
      <PSlider params={params} path="geometry.perspective.y" label="Y 偏移" min={-100} max={100} />
      <div className="text-2xs text-txt3 mt-3 leading-4">宽高比与参考线的快捷操作在画面下方「变换」工具条。</div>
      <PersistGap what="裁剪与透视校正" />
    </div>
  );
}
