/**
 * 左侧序列列表（Phase A）：名称 + 帧数 · 状态；下方 Holy Grail 关键帧表与去闪状态
 * 改为 LR 风格可折叠面板（LrPanel）。关键数值（帧号 / EV / 增益%）用克制金点缀。
 */

import { useMemo } from 'react';
import { useTimelapse } from './store/timelapseStore';
import { Button, Spinner } from '../../components/primitives';
import { LrPanel } from '../../components/LrPanel';
import { formatCount } from '../../utils/format';

const STATE_LABEL: Record<string, string> = { idle: '空闲', analyzing: '分析中', rendering: '渲染中' };

export default function SequenceList() {
  const sequences = useTimelapse((s) => s.sequences);
  const loading = useTimelapse((s) => s.loading);
  const loadError = useTimelapse((s) => s.loadError);
  const selectedSeqId = useTimelapse((s) => s.selectedSeqId);
  const detail = useTimelapse((s) => s.detail);
  const ramp = useTimelapse((s) => s.ramp);
  const rampDirty = useTimelapse((s) => s.rampDirty);

  const deflickerText = useMemo(() => {
    if (!detail) return '';
    if (detail.deflicker.analyzed && detail.deflicker.brightnessGainPct !== undefined) {
      return `去闪分析完成 · 亮度增益 ${detail.deflicker.brightnessGainPct.toFixed(1)}%`;
    }
    return '尚未去闪分析 · 建议先分析';
  }, [detail]);

  return (
    <aside className="shrink-0 w-[276px] border-r border-line flex flex-col overflow-hidden" style={{ background: 'var(--Lumina-Tokens-panel)' }}>
      <div className="flex items-center h-9 px-3 border-b border-line shrink-0">
        <span className="text-2xs text-txt2 font-medium">序列</span>
        <div className="flex-1" />
        <span className="num text-2xs text-txt3">{sequences.length}</span>
      </div>

      {loading && (
        <div className="flex items-center justify-center py-8">
          <Spinner size={16} />
        </div>
      )}
      {!loading && loadError && (
        <div className="mx-2 mt-2 rounded-md border p-2.5 text-2xs leading-5" style={{ borderColor: 'var(--Lumina-Tokens-danger)' }}>
          <span className="font-medium" style={{ color: 'var(--Lumina-Tokens-danger)' }}>序列接口不可用</span>
          <div className="text-txt3 mt-0.5">{loadError}</div>
          <button className="mt-1 text-accent hover:opacity-80" onClick={() => void useTimelapse.getState().load()}>重试</button>
        </div>
      )}
      {!loading && !loadError && sequences.length === 0 && (
        <div className="py-8 px-3 text-center text-2xs text-txt3 leading-5">无延时序列 · 序列由导入时自动识别（同机位连拍聚合）</div>
      )}

      <div className="flex-1 min-h-0 overflow-y-auto p-2 flex flex-col gap-1.5">
        {sequences.map((seq) => {
          const active = seq.seqId === selectedSeqId;
          return (
            <button
              key={seq.seqId}
              onClick={() => void useTimelapse.getState().select(seq.seqId)}
              className={`text-left rounded-md border p-2 transition-colors ${active ? 'border-accent' : 'border-line hover:border-txt3'}`}
              style={{ background: active ? 'var(--Lumina-Tokens-accentSoft)' : 'var(--Lumina-Tokens-card)' }}
            >
              <div className={`text-xs truncate ${active ? 'text-txt font-medium' : 'text-txt2'}`}>{seq.name}</div>
              <div className="num text-2xs text-txt3 mt-0.5">
                {formatCount(seq.frameCount)} 帧 · {STATE_LABEL[seq.state] ?? seq.state}
                {seq.state === 'rendering' && <span style={{ color: 'var(--Lumina-Tokens-gold)' }}> {Math.round(seq.renderProgress)}%</span>}
              </div>
            </button>
          );
        })}
      </div>

      {/* Holy Grail 关键帧表（LR 风格可折叠面板） */}
      {detail && (
        <LrPanel title="关键帧 · Holy Grail">
          <div className="px-3 flex flex-col gap-1.5">
            {rampDirty && <span className="text-2xs" style={{ color: 'var(--Lumina-Tokens-gold)' }}>（未应用）</span>}
            {ramp.length === 0 && <div className="text-2xs text-txt3">无关键帧</div>}
            <div className="flex flex-col gap-0.5">
              {ramp.map((k, i) => (
                <div key={`${k.frameNo}_${i}`} className="num text-2xs text-txt3 flex items-center gap-2">
                  <span className="text-txt2 w-7">K{i + 1}</span>
                  <span className="w-10" style={{ color: 'var(--Lumina-Tokens-gold)' }}>{k.frameNo}</span>
                  <span className="w-14" style={{ color: 'var(--Lumina-Tokens-gold)' }}>海拔 {k.ev > 0 ? '+' : ''}{k.ev.toFixed(1)}</span>
                  <span>{k.temperatureK}K</span>
                </div>
              ))}
            </div>
            <div className="text-2xs mt-1" style={{ color: detail.deflicker.analyzed ? 'var(--Lumina-Tokens-success)' : 'var(--Lumina-Tokens-textMuted)' }}>
              {deflickerText}
            </div>
            {rampDirty && (
              <div className="flex gap-1.5 mt-1">
                <Button size="sm" variant="primary" onClick={() => void useTimelapse.getState().commitRamp()}>应用坡度</Button>
                <Button size="sm" onClick={() => useTimelapse.getState().resetRamp()}>还原</Button>
              </div>
            )}
          </div>
        </LrPanel>
      )}
    </aside>
  );
}
