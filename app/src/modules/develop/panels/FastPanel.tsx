/**
 * 快速模式右栏（帧 02）：预设列 + 基本 6 滑块 + 色调曲线 + 白平衡 + 局部蒙版 L1。
 * 专业模式的完整面板见 ProPanel（7 标签）/ LayersPanel（图层栈），两模式共享同一 params。
 */

import { useEffect, useState } from 'react';
import { useDevelop, sliderLabel } from '../../../store/developStore';
import { api } from '../../../api/engine';
import { Button, Slider, Segmented } from '../../../components/primitives';
import CurveEditor from '../CurveEditor';
import MaskPanel from './MaskPanel';
import type { EditParams, CurvePoint, PresetInfo } from '../../../api/protocol';

const BASIC_SLIDERS: { key: keyof EditParams; min: number; max: number; step?: number }[] = [
  { key: 'exposure', min: -5, max: 5, step: 0.01 },
  { key: 'contrast', min: -100, max: 100 },
  { key: 'highlights', min: -100, max: 100 },
  { key: 'shadows', min: -100, max: 100 },
  { key: 'dehaze', min: -100, max: 100 },
  { key: 'vibrance', min: -100, max: 100 },
];

const SECTION_TITLE = 'text-2xs text-txt3 mb-2 tracking-wide';

export default function FastPanel() {
  const params = useDevelop((s) => s.params);
  const setNumeric = useDevelop((s) => s.setNumeric);
  const applyPatch = useDevelop((s) => s.applyPatch);
  const reset = useDevelop((s) => s.reset);
  const applyPreset = useDevelop((s) => s.applyPreset);
  const [presets, setPresets] = useState<PresetInfo[]>([]);
  const [curvePoints, setCurvePoints] = useState<CurvePoint[]>([]);

  useEffect(() => {
    api.presets
      .list()
      .then(setPresets)
      .catch(() => undefined);
  }, []);

  useEffect(() => {
    if (params) setCurvePoints(params.toneCurveRGB);
  }, [params?.processVersion]); // eslint-disable-line react-hooks/exhaustive-deps

  if (!params) return null;

  return (
    <div className="p-3 overflow-y-auto text-xs">
      {/* 预设（余晖斜刺 / 苍穹·山岚 / 深夜深色 / 建筑·透视 / 日出金顶 / 风光·基础） */}
      <div className={SECTION_TITLE}>预设 · 风光专项</div>
      <div className="flex flex-col gap-1.5 mb-4">
        {presets.map((p) => (
          <button
            key={p.id}
            onClick={() => void applyPreset(p.params, p.name)}
            className="flex items-center gap-2 h-9 px-2.5 rounded-md border border-line bg-card hover:bg-hover hover:border-accent text-left transition-colors"
          >
            <span
              className="w-9 h-6 rounded-sm shrink-0"
              style={{
                background: `linear-gradient(135deg, hsl(${28 + p.params.exposure * 20}, 45%, ${45 + p.params.exposure * 12}%), hsl(${210 - p.params.dehaze}, 30%, ${22 + p.params.shadows / 10}%))`,
              }}
            />
            <span className="flex-1">
              <span className="block text-2xs text-txt">{p.name}</span>
              <span className="block text-2xs text-txt3 num">
                曝光 {fmt(p.params.exposure)} · 对比 {p.params.contrast} · 高光 {p.params.highlights}
              </span>
            </span>
          </button>
        ))}
      </div>

      {/* 基本 6 滑块 */}
      <div className={SECTION_TITLE}>基本调整</div>
      {BASIC_SLIDERS.map((s) => (
        <Slider
          key={s.key as string}
          label={sliderLabel(s.key)}
          value={params[s.key] as number}
          min={s.min}
          max={s.max}
          step={s.step ?? 1}
          defaultValue={0}
          onChange={(v) => void setNumeric(s.key, v, false)}
          onCommit={(v) => void setNumeric(s.key, v, true)}
          format={(v) => (s.key === 'exposure' ? `${v > 0 ? '+' : ''}${Math.round(v * 100) / 100}` : String(Math.round(v)))}
        />
      ))}

      {/* 白平衡 */}
      <div className={`${SECTION_TITLE} mt-3`}>白平衡</div>
      <div className="flex items-center gap-2 mb-2">
        <Segmented
          value={params.whiteBalance}
          onChange={(v) => void applyPatch({ whiteBalance: v }, true, `白平衡 · ${v === 'asShot' ? '拍摄时' : '自定义'}`)}
          options={[
            { value: 'asShot' as const, label: '拍摄时' },
            { value: 'custom' as const, label: '自定义' },
          ]}
        />
        <span className="num text-2xs text-txt3">{params.whiteBalance === 'custom' ? `${params.temperature ?? 5500}K · ${params.tint ?? 0}` : 'As Shot'}</span>
      </div>
      {params.whiteBalance === 'custom' && (
        <>
          <Slider
            label="色温"
            value={params.temperature ?? 5500}
            min={2000}
            max={12000}
            defaultValue={5500}
            onChange={(v) => void applyPatch({ temperature: v }, false)}
            onCommit={(v) => void applyPatch({ temperature: v }, true, `色温 ${v}K`)}
            format={(v) => `${v}K`}
          />
          <Slider label="色调" value={params.tint ?? 0} min={-100} max={100} defaultValue={0} onChange={(v) => void applyPatch({ tint: v }, false)} onCommit={(v) => void applyPatch({ tint: v }, true)} />
        </>
      )}

      {/* 色调曲线 */}
      <div className={`${SECTION_TITLE} mt-3`}>色调曲线</div>
      <CurveEditor
        points={curvePoints}
        onChange={(pts) => {
          setCurvePoints(pts);
          void applyPatch({ toneCurveRGB: pts }, false);
        }}
      />
      <div className="flex items-center justify-between mt-1 mb-3">
        <span className="text-2xs text-txt3">拖动调整 · 单击添加点 · 右键删除（松手写入 XMP）</span>
      </div>

      {/* 局部蒙版三件套 */}
      <div className={`${SECTION_TITLE} mt-2`}>局部蒙版 · L1</div>
      <MaskPanel />

      {/* 底部操作（重置 / 保存预设） */}
      <div className="flex items-center gap-2 mt-4">
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

function fmt(v: number): string {
  return `${v > 0 ? '+' : ''}${Math.round(v * 100) / 100}`;
}
