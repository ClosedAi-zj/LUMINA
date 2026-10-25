/**
 * 快捷模式右栏（v1.2.0 阶段 3 换皮）：结构保持 1.1 的肌肉记忆不变
 * （基本 6 滑块 → 白平衡 → 色调曲线 → 局部蒙版 → 直方图 → 重置/保存预设），
 * 只把视觉换成与专业模式同一套卡片 / 参数行 / 滑块。
 */

import { useEffect, useState } from 'react';
import { useDevelop, sliderLabel, temperatureToKelvin } from '../../../store/developStore';
import { Button } from '../../../components/primitives';
import CurveEditor from '../CurveEditor';
import MaskPanel from './MaskPanel';
import HistogramPanel, { type Histogram } from './HistogramPanel';
import { PanelCard, ParamSlider, ParamSegmented } from './panelKit';
import type { EditParams, CurvePoint } from '../../../api/protocol';

const BASIC_SLIDERS: { key: keyof EditParams; min: number; max: number; step?: number }[] = [
  { key: 'exposure', min: -5, max: 5, step: 0.01 },
  { key: 'contrast', min: -100, max: 100 },
  { key: 'highlights', min: -100, max: 100 },
  { key: 'shadows', min: -100, max: 100 },
  { key: 'dehaze', min: -100, max: 100 },
  { key: 'vibrance', min: -100, max: 100 },
];

export default function FastPanel({ histogram }: { histogram?: Histogram | null }) {
  const params = useDevelop((s) => s.params);
  const setNumeric = useDevelop((s) => s.setNumeric);
  const applyPatch = useDevelop((s) => s.applyPatch);
  const reset = useDevelop((s) => s.reset);
  const [curvePoints, setCurvePoints] = useState<CurvePoint[]>([]);

  useEffect(() => {
    if (params) setCurvePoints(params.toneCurveRGB);
  }, [params?.processVersion]); // eslint-disable-line react-hooks/exhaustive-deps

  if (!params) return null;

  return (
    <div className="h-full overflow-y-auto p-2 flex flex-col gap-1.5" style={{ background: 'var(--Lumina-Tokens-canvas)' }}>
      <PanelCard title="基本调整" defaultOpen hint="6 项">
        {BASIC_SLIDERS.map((s) => (
          <ParamSlider
            key={s.key as string}
            label={sliderLabel(s.key)}
            value={(params[s.key] as number) ?? 0}
            min={s.min}
            max={s.max}
            step={s.step ?? 1}
            defaultValue={0}
            onChange={(v) => void setNumeric(s.key, v, false)}
            onCommit={(v) => void setNumeric(s.key, v, true)}
            format={(v) => (s.key === 'exposure' ? `${v > 0 ? '+' : ''}${Math.round(v * 100) / 100}` : String(Math.round(v)))}
          />
        ))}
      </PanelCard>

      {/* temperature 单位契约：store 内为归一化值，UI 显示/输入口径为开尔文 */}
      <PanelCard title="白平衡" defaultOpen hint={params.whiteBalance === 'custom' ? `${Math.round(temperatureToKelvin(params.temperature ?? 0))}K` : 'As Shot'}>
        <div className="px-2 pb-1">
          <ParamSegmented
            value={params.whiteBalance}
            onChange={(v) => void applyPatch({ whiteBalance: v }, true, `白平衡 · ${v === 'asShot' ? '拍摄时' : '自定义'}`)}
            options={[
              { value: 'asShot' as const, label: '拍摄时' },
              { value: 'custom' as const, label: '自定义' },
            ]}
          />
        </div>
        {params.whiteBalance === 'custom' && (
          <>
            <ParamSlider
              label="色温"
              value={temperatureToKelvin(params.temperature ?? 0)}
              min={2000}
              max={12000}
              defaultValue={5500}
              format={(v) => `${Math.round(v)}K`}
              onChange={(v) => void applyPatch({ temperature: v }, false)}
              onCommit={(v) => void applyPatch({ temperature: v }, true, `色温 ${Math.round(v)}K`)}
            />
            <ParamSlider
              label="色调"
              value={params.tint ?? 0}
              min={-100}
              max={100}
              defaultValue={0}
              onChange={(v) => void applyPatch({ tint: v }, false)}
              onCommit={(v) => void applyPatch({ tint: v }, true)}
            />
          </>
        )}
      </PanelCard>

      <PanelCard title="色调曲线" defaultOpen hint="复合曲线">
        <div className="px-2">
          <CurveEditor
            points={curvePoints.length >= 2 ? curvePoints : [{ x: 0, y: 0 }, { x: 255, y: 255 }]}
            onChange={(pts) => {
              setCurvePoints(pts);
              void applyPatch({ toneCurveRGB: pts }, false);
            }}
          />
        </div>
        <div className="text-2xs text-txt3 px-3 pt-1">拖动调整 · 单击添加点 · 右键删除（松手写入 XMP）</div>
      </PanelCard>

      <PanelCard title="局部蒙版" defaultOpen hint="L1 · 已建列表">
        <MaskPanel />
      </PanelCard>

      <PanelCard title="直方图" defaultOpen hint="RGB 叠加 + 亮度">
        <HistogramPanel histogram={histogram ?? null} bare />
      </PanelCard>

      <div className="flex items-center gap-2 px-0.5">
        <Button className="flex-1" onClick={() => void reset()}>
          重置
        </Button>
        <Button
          className="flex-1"
          variant="primary"
          onClick={() => {
            const p = useDevelop.getState().params;
            if (p) void useDevelop.getState().saveSnapshot(`预设 ${new Date().toLocaleTimeString('zh-Hans-CN', { hour: '2-digit', minute: '2-digit' })}`);
          }}
        >
          保存预设
        </Button>
      </div>
    </div>
  );
}
