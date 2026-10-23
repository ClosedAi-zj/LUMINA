/**
 * 直方图面板：R/G/B 三通道叠加 + 亮度（灰线，由 ViewerCanvas 采样回调驱动，
 * 150ms 防抖）+ 引擎曝光溢出提示。专业模式与快速模式共用。
 */

import { useEffect, useRef } from 'react';
import { useDevelop } from '../../../store/developStore';
import { api } from '../../../api/engine';
import { ViewerSurface } from '../../../theme/ThemeProvider';

export type Histogram = { r: number[]; g: number[]; b: number[] };

export default function HistogramPanel({ histogram }: { histogram: Histogram | null }) {
  const canvasRef = useRef<HTMLCanvasElement>(null);
  const photoId = useDevelop((s) => s.photoId);

  useEffect(() => {
    if (histogram) drawHist(canvasRef.current, histogram);
  }, [histogram]);

  // 曝光溢出提示（引擎 analysis.getExposedRange）
  useEffect(() => {
    if (photoId === null) return;
    let cancelled = false;
    api.analysis
      .getExposedRange(photoId)
      .then((res) => {
        if (cancelled) return;
        const el = document.getElementById('exposed-range');
        if (el) {
          el.textContent =
            res.underexposedPct > 1 || res.overexposedPct > 1
              ? `阴影溢出 ${res.underexposedPct}% · 高光滑出 ${res.overexposedPct}%`
              : '';
        }
      })
      .catch(() => undefined);
    return () => {
      cancelled = true;
    };
  }, [photoId]);

  return (
    <ViewerSurface className="px-3 py-2 shrink-0" style={{ borderBottom: '1px solid var(--Lumina-Tokens-border)' }}>
      <div className="flex items-center gap-2 mb-1">
        <span className="text-2xs text-txt3">直方图 RGB</span>
        <span className="flex items-center gap-1 text-2xs text-txt3">
          <span className="w-1.5 h-1.5 rounded-full" style={{ background: 'var(--Lumina-Tokens-danger)' }} />R
          <span className="w-1.5 h-1.5 rounded-full" style={{ background: 'var(--Lumina-Tokens-success)' }} />G
          <span className="w-1.5 h-1.5 rounded-full" style={{ background: 'var(--Lumina-Tokens-chartBlue)' }} />B
          <span className="w-1.5 h-1.5 rounded-full" style={{ background: 'var(--Lumina-Tokens-textSecondary)' }} />亮度
        </span>
      </div>
      <canvas ref={canvasRef} width={224} height={64} className="w-full rounded-sm" style={{ height: 56, background: 'var(--Lumina-Tokens-viewer)' }} />
      <div id="exposed-range" className="text-2xs mt-1" style={{ color: 'var(--Lumina-Tokens-danger)', minHeight: 14 }} />
    </ViewerSurface>
  );
}

function drawHist(canvas: HTMLCanvasElement | null, h: Histogram): void {
  if (!canvas) return;
  const ctx = canvas.getContext('2d');
  if (!ctx) return;
  // 通道色读设计令牌（浅色下降饱和，与图例一致）
  const css = getComputedStyle(canvas);
  const tok = (name: string, fallback: string): string => css.getPropertyValue(name).trim() || fallback;
  const w = canvas.width;
  const ht = canvas.height;
  ctx.clearRect(0, 0, w, ht);
  const lum = h.r.map((_, i) => (h.r[i] + h.g[i] + h.b[i]) / 3);
  const maxVal = Math.max(1, ...h.r, ...h.g, ...h.b);
  const draw = (arr: number[], color: string) => {
    ctx.fillStyle = color;
    ctx.globalCompositeOperation = 'lighter';
    ctx.beginPath();
    ctx.moveTo(0, ht);
    for (let i = 0; i < 256; i++) {
      const x = (i / 255) * w;
      const y = ht - (arr[i] / maxVal) * ht * 0.95;
      ctx.lineTo(x, y);
    }
    ctx.lineTo(w, ht);
    ctx.closePath();
    ctx.fill();
  };
  draw(h.r, tok('--Lumina-Tokens-chartRed', 'rgba(217,96,79,0.55)'));
  draw(h.g, tok('--Lumina-Tokens-chartGreen', 'rgba(125,217,166,0.5)'));
  draw(h.b, tok('--Lumina-Tokens-chartBlue', 'rgba(91,141,217,0.55)'));
  // 亮度通道：灰色描线叠加
  ctx.globalCompositeOperation = 'source-over';
  ctx.strokeStyle = tok('--Lumina-Tokens-chartLuma', 'rgba(168,173,181,0.8)');
  ctx.lineWidth = 1;
  ctx.beginPath();
  for (let i = 0; i < 256; i++) {
    const x = (i / 255) * w;
    const y = ht - (lum[i] / maxVal) * ht * 0.95;
    if (i === 0) ctx.moveTo(x, y);
    else ctx.lineTo(x, y);
  }
  ctx.stroke();
}
