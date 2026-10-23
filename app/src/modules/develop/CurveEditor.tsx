/**
 * 色调曲线编辑器（帧 02）：控制点可拖动，双击添加点，右键删除点。
 * 曲线值域 0..255（crs 域），输出 toneCurveRGB。
 */

import { useEffect, useRef, useState } from 'react';
import type { CurvePoint } from '../../api/protocol';

export default function CurveEditor({ points, onChange }: { points: CurvePoint[]; onChange: (pts: CurvePoint[]) => void }) {
  const canvasRef = useRef<HTMLCanvasElement>(null);
  const [dragIdx, setDragIdx] = useState<number | null>(null);
  const size = 200;

  useEffect(() => {
    const canvas = canvasRef.current;
    if (!canvas) return;
    const ctx = canvas.getContext('2d');
    if (!ctx) return;
    const dpr = window.devicePixelRatio || 1;
    canvas.width = size * dpr;
    canvas.height = size * dpr;
    ctx.scale(dpr, dpr);

    // 网格 / 曲线颜色一律读设计令牌（浅色主题随 data-theme 翻转）
    const css = getComputedStyle(canvas);
    ctx.fillStyle = css.getPropertyValue('--Lumina-Tokens-card').trim() || 'transparent';
    ctx.fillRect(0, 0, size, size);
    const accent = css.getPropertyValue('--Lumina-Tokens-accent').trim() || 'currentColor';
    ctx.strokeStyle = 'rgba(128,128,128,0.18)';
    ctx.lineWidth = 1;
    for (let i = 1; i < 4; i++) {
      const p = (size / 4) * i;
      ctx.beginPath();
      ctx.moveTo(p, 0);
      ctx.lineTo(p, size);
      ctx.moveTo(0, p);
      ctx.lineTo(size, p);
      ctx.stroke();
    }

    // 曲线（单调分段线性插值）
    const pts = [...points].sort((a, b) => a.x - b.x);
    ctx.lineWidth = 2;
    ctx.strokeStyle = accent;
    ctx.beginPath();
    for (let x = 0; x <= 255; x += 2) {
      let lo = 0;
      while (lo < pts.length && pts[lo].x <= x) lo++;
      let y: number;
      if (lo === 0) y = pts[0]?.y ?? x;
      else if (lo >= pts.length) y = pts[pts.length - 1]?.y ?? x;
      else {
        const a = pts[lo - 1];
        const b = pts[lo];
        const t = (x - a.x) / Math.max(1, b.x - a.x);
        y = a.y + (b.y - a.y) * t;
      }
      const px = (x / 255) * size;
      const py = size - (y / 255) * size;
      if (x === 0) ctx.moveTo(px, py);
      else ctx.lineTo(px, py);
    }
    ctx.stroke();

    // 控制点
    for (const p of pts) {
      ctx.fillStyle = accent;
      ctx.beginPath();
      ctx.arc((p.x / 255) * size, size - (p.y / 255) * size, 4, 0, Math.PI * 2);
      ctx.fill();
    }
  }, [points]);

  const toCurve = (e: React.PointerEvent | React.MouseEvent): { x: number; y: number } => {
    const rect = canvasRef.current!.getBoundingClientRect();
    const x = Math.max(0, Math.min(255, Math.round(((e.clientX - rect.left) / rect.width) * 255)));
    const y = Math.max(0, Math.min(255, Math.round((1 - (e.clientY - rect.top) / rect.height) * 255)));
    return { x, y };
  };

  return (
    <canvas
      ref={canvasRef}
      className="w-full rounded-md border border-line cursor-crosshair"
      style={{ height: size, background: 'var(--Lumina-Tokens-card)', touchAction: 'none' }}
      onPointerDown={(e) => {
        const { x, y } = toCurve(e);
        const idx = points.findIndex((p) => Math.abs(p.x - x) < 14);
        if (idx >= 0) {
          setDragIdx(idx);
          (e.target as HTMLElement).setPointerCapture(e.pointerId);
        } else {
          // 双击语义：单击空白添加点
          const next = [...points, { x, y }].sort((a, b) => a.x - b.x);
          onChange(next);
        }
      }}
      onPointerMove={(e) => {
        if (dragIdx === null) return;
        const { x, y } = toCurve(e);
        const next = points.map((p, i) => (i === dragIdx ? { x: i === 0 ? 0 : i === points.length - 1 ? 255 : x, y } : p));
        onChange(next);
      }}
      onPointerUp={() => setDragIdx(null)}
      onContextMenu={(e) => {
        e.preventDefault();
        const { x } = toCurve(e);
        const idx = points.findIndex((p) => Math.abs(p.x - x) < 14);
        if (idx > 0 && idx < points.length - 1) {
          onChange(points.filter((_, i) => i !== idx));
        }
      }}
    />
  );
}
