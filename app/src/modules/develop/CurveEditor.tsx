/**
 * 色调曲线编辑器（帧 02）：控制点可拖动，双击添加点，右键删除点。
 * 曲线值域 0..255（crs 域），输出 toneCurveRGB。
 */

import { useEffect, useRef, useState } from 'react';
import type { CurvePoint } from '../../api/protocol';

/**
 * 色调曲线编辑器（受控组件）：points 完全由调用方（store 里的 params）驱动，
 * 自身不持有曲线副本 —— 否则切图后组件里会残留上一张的曲线，拖动时又写回新图。
 *
 * onChange = 拖动中（commit=false，只刷新预览）；
 * onCommit = 松手/加点/删点（commit=true，落历史 + 写 XMP）。
 */
export default function CurveEditor({
  points,
  onChange,
  onCommit,
}: {
  points: CurvePoint[];
  onChange: (pts: CurvePoint[]) => void;
  onCommit?: (pts: CurvePoint[]) => void;
}) {
  const canvasRef = useRef<HTMLCanvasElement>(null);
  const [dragIdx, setDragIdx] = useState<number | null>(null);
  const size = 200;
  /** 最近一次派发的曲线：拖动中 onChange 是异步落 store，松手时要用它提交而不是旧的 props */
  const latestRef = useRef<CurvePoint[]>(points);
  latestRef.current = points;
  /** 本次按下后是否真的移动过：单击一个已存在的点（没拖）不该提交 —— 否则每次
   *  「点一下看看」都会写一条同值的历史步骤（空步），历史面板被噪声刷满 */
  const movedRef = useRef(false);

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
          movedRef.current = false;
          setDragIdx(idx);
          (e.target as HTMLElement).setPointerCapture(e.pointerId);
        } else {
          // 双击语义：单击空白添加点（离散操作 → 直接 commit）
          const next = [...points, { x, y }].sort((a, b) => a.x - b.x);
          onChange(next);
          onCommit?.(next);
        }
      }}
      onPointerMove={(e) => {
        if (dragIdx === null) return;
        const { x, y } = toCurve(e);
        const next = points.map((p, i) => (i === dragIdx ? { x: i === 0 ? 0 : i === points.length - 1 ? 255 : x, y } : p));
        // 仅坐标真的变了才算「移动过」（避免 1px 抖动也当成一次编辑）
        const cur = points[dragIdx];
        if (!cur || cur.x !== next[dragIdx].x || cur.y !== next[dragIdx].y) movedRef.current = true;
        // 先写 ref 再派发：松手可能早于一次 React 重渲染，提交要以「刚拖出的值」为准
        latestRef.current = next;
        onChange(next);
      }}
      onPointerUp={() => {
        if (dragIdx === null) return;
        // 松手 = 一次编辑结束：以当前受控值为准提交一次（拖动中每帧都不写历史）
        setDragIdx(null);
        if (!movedRef.current) return; // 纯单击（没拖动）不产生历史步骤
        movedRef.current = false;
        onCommit?.([...latestRef.current].sort((a, b) => a.x - b.x));
      }}
      onContextMenu={(e) => {
        e.preventDefault();
        const { x } = toCurve(e);
        const idx = points.findIndex((p) => Math.abs(p.x - x) < 14);
        if (idx > 0 && idx < points.length - 1) {
          const next = points.filter((_, i) => i !== idx);
          onChange(next);
          onCommit?.(next);
        }
      }}
    />
  );
}
