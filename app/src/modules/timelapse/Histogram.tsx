/**
 * 客户端 RGB 直方图（Phase B）：改用引擎 RPC `timelapse.frameHistogram`
 * 取当前帧 R/G/B + 亮度 256 桶直方图，叠加在观察区一角（半透明 LR 风格）。
 * 引擎已接入，不再走 canvas 读像素。
 *
 * 容错：RPC 失败 / 返回长度异常 → 显示「直方图暂不可用」小角标（不抛错、不阻断播放器）。
 */

import { useEffect, useRef, useState } from 'react';
import { api } from '../../api/engine';
import type { TimelapseFrameHistogram } from '../../api/protocol';

const BARS = 256;

/** overlay = 观察区角标小图（旧位）；panel = 右栏顶部大图（对照 LapseOne 参考布局） */
const SIZES = {
  overlay: { W: 132, H: 64 },
  panel: { W: 268, H: 108 },
} as const;

// 模块级缓存：key = `${seqId}_${frameNo}`，避免同一帧重复 RPC
const histogramCache = new Map<string, TimelapseFrameHistogram>();

export default function Histogram({ seqId, frameNo, variant = 'overlay' }: { seqId: number | null; frameNo: number; variant?: 'overlay' | 'panel' }) {
  const { W, H } = SIZES[variant];
  const canvasRef = useRef<HTMLCanvasElement>(null);
  const [data, setData] = useState<TimelapseFrameHistogram | null>(null);
  const [unavailable, setUnavailable] = useState(false);

  // 数据获取（依赖 seqId / frameNo）
  useEffect(() => {
    let cancelled = false;
    setUnavailable(false);
    if (seqId === null) {
      setData(null);
      return;
    }
    const key = `${seqId}_${frameNo}`;
    const cached = histogramCache.get(key);
    if (cached) {
      setData(cached);
      setUnavailable(false);
      return;
    }
    (async () => {
      try {
        const h = await api.timelapse.frameHistogram(seqId, frameNo, 256);
        if (cancelled) return;
        // 长度异常（非 256 桶）视为不可用
        if (!h || h.r.length !== BARS || h.g.length !== BARS || h.b.length !== BARS || h.luma.length !== BARS) {
          setUnavailable(true);
          return;
        }
        histogramCache.set(key, h);
        setData(h);
        setUnavailable(false);
      } catch {
        if (!cancelled) setUnavailable(true);
      }
    })();
    return () => {
      cancelled = true;
    };
  }, [seqId, frameNo]);

  // 绘制（数据来自 RPC；4 通道叠加：luma 作底层淡色，R/G/B 彩色线）
  useEffect(() => {
    const canvas = canvasRef.current;
    if (!data || !canvas) return;
    const ctx = canvas.getContext('2d');
    if (!ctx) {
      setUnavailable(true);
      return;
    }
    const { r, g, b, luma } = data;
    const max = Math.max(1, ...r, ...g, ...b, ...luma);

    const dpr = window.devicePixelRatio || 1;
    canvas.width = W * dpr;
    canvas.height = H * dpr;
    ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
    ctx.clearRect(0, 0, W, H);
    ctx.fillStyle = 'rgba(8,9,11,0.55)';
    ctx.fillRect(0, 0, W, H);

    const drawChannel = (arr: number[], color: string) => {
      ctx.beginPath();
      for (let i = 0; i < BARS; i++) {
        const x = (i / (BARS - 1)) * W;
        const y = H - (arr[i] / max) * (H - 2) - 1;
        if (i === 0) ctx.moveTo(x, y);
        else ctx.lineTo(x, y);
      }
      ctx.lineTo(W, H);
      ctx.lineTo(0, H);
      ctx.closePath();
      ctx.fillStyle = color;
      ctx.fill();
    };
    drawChannel(luma, 'rgba(167,173,183,0.28)');
    drawChannel(r, 'rgba(255,69,58,0.45)');
    drawChannel(g, 'rgba(48,209,88,0.42)');
    drawChannel(b, 'rgba(10,132,255,0.45)');
  }, [data]);

  if (seqId === null) return null;

  // panel 变体：右栏顶部大图 + 通道图例（对照 LapseOne 参考的 RGB 直方图位）
  if (variant === 'panel') {
    return (
      <div className="rounded-lgC border border-line overflow-hidden" style={{ background: 'var(--Lumina-Tokens-card)' }} role="img" aria-label="RGB 直方图">
        <div className="flex items-center gap-2.5 px-2.5 h-6 border-b border-line">
          {unavailable || !data ? (
            <span className="text-2xs" style={{ color: 'var(--Lumina-Tokens-textMuted)' }}>直方图暂不可用</span>
          ) : (
            <>
              <LegendDot color="rgba(255,69,58,0.9)" label="R" />
              <LegendDot color="rgba(48,209,88,0.9)" label="G" />
              <LegendDot color="rgba(10,132,255,0.9)" label="B" />
              <LegendDot color="rgba(167,173,183,0.8)" label="亮度" />
            </>
          )}
          <div className="flex-1" />
          <span className="num text-2xs text-txt3">帧 {frameNo}</span>
        </div>
        {unavailable || !data ? (
          <div className="flex items-center justify-center" style={{ height: H }}>
            <span className="text-2xs" style={{ color: 'var(--Lumina-Tokens-textMuted)' }}>引擎直方图接口不可用（不影响其他功能）</span>
          </div>
        ) : (
          <canvas ref={canvasRef} style={{ width: W, height: H, display: 'block' }} />
        )}
      </div>
    );
  }

  return (
    <div
      className="absolute bottom-2 left-2 rounded-md border border-line overflow-hidden pointer-events-none"
      style={{ background: 'var(--Lumina-Tokens-viewerOverlay)' }}
      role="img"
      aria-label="RGB 直方图"
    >
      {unavailable || !data ? (
        <div className="px-2 py-1 text-2xs whitespace-nowrap" style={{ color: 'var(--Lumina-Tokens-textMuted)' }}>
          直方图暂不可用
        </div>
      ) : (
        <canvas ref={canvasRef} style={{ width: W, height: H, display: 'block' }} />
      )}
    </div>
  );
}

function LegendDot({ color, label }: { color: string; label: string }) {
  return (
    <span className="inline-flex items-center gap-1">
      <span className="w-1.5 h-1.5 rounded-full" style={{ background: color }} />
      <span className="text-2xs text-txt3">{label}</span>
    </span>
  );
}
