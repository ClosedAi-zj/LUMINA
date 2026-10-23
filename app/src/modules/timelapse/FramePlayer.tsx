/**
 * 中央帧播放器（对照 08 号设计稿）：帧预览（恒 #08090B 观察区）+
 * 播放/暂停 + 进度刷（含关键帧标记）+ 底部曝光过渡曲线（可拖关键帧纵向微调）。
 */

import { useEffect, useMemo, useRef } from 'react';
import { useTimelapse } from './store/timelapseStore';
import { fileUrl } from '../../api/rpc-client';
import { Icon } from '../../components/shell';
import { Spinner } from '../../components/primitives';

const PLAYER_H = 380;

export default function FramePlayer() {
  const detail = useTimelapse((s) => s.detail);
  const detailLoading = useTimelapse((s) => s.detailLoading);
  const selectedSeqId = useTimelapse((s) => s.selectedSeqId);
  const frameNo = useTimelapse((s) => s.frameNo);
  const playing = useTimelapse((s) => s.playing);
  const framePreviews = useTimelapse((s) => s.framePreviews);
  const ramp = useTimelapse((s) => s.ramp);
  const curveBoxRef = useRef<HTMLDivElement>(null);

  const key = selectedSeqId !== null ? `${selectedSeqId}_${frameNo}` : '';
  const previewPath = framePreviews[key] ?? null;

  // 帧变化 → 预取当前帧大图（1080）
  useEffect(() => {
    if (selectedSeqId !== null) void useTimelapse.getState().ensureFramePreview(selectedSeqId, frameNo);
  }, [selectedSeqId, frameNo]);

  // 播放循环（8fps 演示节奏）
  useEffect(() => {
    if (!playing) return;
    const t = setInterval(() => useTimelapse.getState().stepFrame(1), 125);
    return () => clearInterval(t);
  }, [playing]);

  // 曝光过渡曲线：keyframes (frameNo, ev) → 画布坐标
  const curve = useMemo(() => {
    if (ramp.length === 0) return null;
    const total = Math.max(detail?.frameCount ?? 1, 1);
    const w = 100; // 归一化，容器内缩放
    const h = 100;
    const pts = ramp
      .slice()
      .sort((a, b) => a.frameNo - b.frameNo)
      .map((k) => ({
        x: (k.frameNo / total) * w,
        y: ((5 - k.ev) / 10) * h, // ev -5..+5 → 下→上
        k,
      }));
    const d = pts.map((p, i) => `${i === 0 ? 'M' : 'L'}${p.x.toFixed(2)},${p.y.toFixed(2)}`).join(' ');
    return { pts, d, total };
  }, [ramp, detail]);

  // 曲线上拖关键帧（纵向 = ev 微调）
  const onCurvePointerMove = (e: React.PointerEvent, index: number) => {
    const box = curveBoxRef.current;
    if (!box) return;
    const rect = box.getBoundingClientRect();
    const ratio = 1 - (e.clientY - rect.top) / rect.height;
    const ev = Math.max(-5, Math.min(5, ratio * 10 - 5));
    useTimelapse.getState().moveKeyframe(index, ev);
  };

  if (detailLoading || !detail) {
    return (
      <div className="flex-1 min-w-0 flex items-center justify-center" style={{ background: 'var(--Lumina-Tokens-viewer)' }}>
        <Spinner size={22} />
      </div>
    );
  }

  return (
    <div className="flex-1 min-w-0 flex flex-col" style={{ background: 'var(--Lumina-Tokens-viewer)' }}>
      {/* 画面区 */}
      <div className="relative flex-1 min-h-0 flex items-center justify-center overflow-hidden" style={{ height: PLAYER_H }}>
        {previewPath ? (
          <img src={fileUrl(previewPath)} alt={`帧 ${frameNo}`} draggable={false} className="max-w-full max-h-full object-contain" />
        ) : (
          <Spinner size={22} />
        )}
        {/* 中央播放/暂停按钮 */}
        <button
          onClick={() => useTimelapse.getState().setPlaying(!playing)}
          className="absolute w-14 h-14 rounded-full border-2 border-accent bg-panel/70 flex items-center justify-center text-accent hover:scale-105 transition-transform"
          title={playing ? '暂停' : '播放'}
          aria-label={playing ? '暂停' : '播放'}
        >
          <Icon name={playing ? 'pause' : 'play'} size={22} />
        </button>
        <span className="absolute top-2 right-3 num text-2xs text-txt2 px-1.5 py-0.5 rounded-sm bg-card border border-line">
          {frameNo} / {detail.frameCount}
        </span>
        <span className="absolute top-2 left-3 text-2xs text-txt2 px-1.5 py-0.5 rounded-sm bg-card border border-line">{detail.name}</span>
      </div>

      {/* 进度刷 */}
      <div className="shrink-0 flex items-center gap-3 px-4 h-10 border-t border-line" style={{ background: 'var(--Lumina-Tokens-panel)' }}>
        <span className="num text-2xs text-txt2 w-20">
          {String(Math.floor(frameNo / 480)).padStart(2, '0')}:{String(Math.floor((frameNo % 480) / 8)).padStart(2, '0')}:{String((frameNo % 8) * 7.5).padStart(2, '0')}
        </span>
        <span className="num text-2xs text-txt3 shrink-0">{frameNo} / {detail.frameCount} 帧</span>
        <div className="relative flex-1 flex items-center">
          <input
            type="range"
            min={1}
            max={Math.max(1, detail.frameCount)}
            value={frameNo}
            onChange={(e) => {
              useTimelapse.getState().setPlaying(false);
              useTimelapse.getState().setFrameNo(Number(e.target.value));
            }}
            className="w-full h-1 appearance-none rounded-full cursor-pointer accent-accent"
            style={{
              background: `linear-gradient(to right, var(--Lumina-Tokens-accent) 0%, var(--Lumina-Tokens-accent) ${((frameNo - 1) / Math.max(1, detail.frameCount - 1)) * 100}%, var(--Lumina-Tokens-border) ${((frameNo - 1) / Math.max(1, detail.frameCount - 1)) * 100}%, var(--Lumina-Tokens-border) 100%)`,
            }}
          />
          {/* 关键帧位置标记 */}
          {ramp.map((k, i) => (
            <span
              key={i}
              className="absolute top-1/2 -translate-y-1/2 w-0.5 h-3 rounded-sm pointer-events-none"
              style={{ left: `${(k.frameNo / Math.max(1, detail.frameCount)) * 100}%`, background: 'var(--Lumina-Tokens-accent)' }}
            />
          ))}
        </div>
      </div>

      {/* 曝光过渡曲线（Holy Grail） */}
      <div className="shrink-0 border-t border-line px-4 py-2" style={{ background: 'var(--Lumina-Tokens-panel)' }}>
        <div className="flex items-center mb-1">
          <span className="text-2xs text-txt2 font-medium">Holy Grail · 曝光过渡曲线</span>
          <div className="flex-1" />
          <span className="text-2xs text-txt3">{detail.frameCount} 帧 · 拖动关键帧微调（纵向 = EV）</span>
        </div>
        <div ref={curveBoxRef} className="relative h-16 rounded-md border border-line overflow-hidden" style={{ background: 'var(--Lumina-Tokens-card)' }}>
          {curve ? (
            <svg width="100%" height="100%" viewBox="0 0 100 100" preserveAspectRatio="none" className="absolute inset-0">
              <line x1="0" y1="50" x2="100" y2="50" className="stroke-line" strokeWidth={0.5} vectorEffect="non-scaling-stroke" />
              <path d={curve.d} fill="none" className="stroke-accent" strokeWidth={1.5} vectorEffect="non-scaling-stroke" />
            </svg>
          ) : (
            <div className="absolute inset-0 flex items-center justify-center text-2xs text-txt3">无关键帧数据</div>
          )}
          {curve?.pts.map((p, i) => (
            <span
              key={i}
              role="slider"
              aria-label={`关键帧 ${i + 1} EV ${p.k.ev.toFixed(1)}`}
              aria-valuenow={p.k.ev}
              aria-valuemin={-5}
              aria-valuemax={5}
              tabIndex={0}
              className="absolute w-2.5 h-2.5 rounded-full bg-accent border border-onaccent cursor-ns-resize hover:scale-125 transition-transform"
              style={{ left: `calc(${p.x}% - 5px)`, top: `calc(${p.y}% - 5px)` }}
              onPointerDown={(e) => {
                (e.target as Element).setPointerCapture?.(e.pointerId);
                const move = (ev: React.PointerEvent) => onCurvePointerMove(ev, i);
                const up = () => {
                  window.removeEventListener('pointermove', move as unknown as EventListener);
                  window.removeEventListener('pointerup', up);
                };
                window.addEventListener('pointermove', move as unknown as EventListener);
                window.addEventListener('pointerup', up);
              }}
              title={`帧 ${p.k.frameNo} · ${p.k.ev > 0 ? '+' : ''}${p.k.ev.toFixed(1)}EV · ${p.k.temperatureK}K（拖动纵向微调）`}
            />
          ))}
        </div>
      </div>
    </div>
  );
}
