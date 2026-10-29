/**
 * 客户端 RGB 直方图（Phase A）：离屏 canvas 加载当前帧预览图，计算 R/G/B + 亮度
 * 256 桶直方图并叠加在观察区一角（半透明 LR 风格）。
 *
 * 容错：若图像跨域 / taint 导致 getImageData 抛错（lumina-file:// 预览在真实环境下
 * 通常无法读取像素），catch 后隐藏直方图并显示「直方图暂不可用」小角标。
 * 不修改引擎；引擎侧直方图接入为 Phase B TODO。
 */

import { useEffect, useRef, useState } from 'react';

const W = 132;
const H = 64;
const BARS = 256;

export default function Histogram({ src }: { src: string | null }) {
  const canvasRef = useRef<HTMLCanvasElement>(null);
  const [unavailable, setUnavailable] = useState(false);

  useEffect(() => {
    setUnavailable(false);
    const canvas = canvasRef.current;
    if (!src || !canvas) return;
    const ctx = canvas.getContext('2d');
    if (!ctx) {
      setUnavailable(true);
      return;
    }
    const img = new Image();
    img.crossOrigin = 'anonymous';
    img.onload = () => {
      try {
        const cw = img.naturalWidth || 256;
        const ch = img.naturalHeight || 256;
        const scale = Math.min(1, 256 / Math.max(cw, ch));
        const dw = Math.max(1, Math.round(cw * scale));
        const dh = Math.max(1, Math.round(ch * scale));
        const off = document.createElement('canvas');
        off.width = dw;
        off.height = dh;
        const octx = off.getContext('2d');
        if (!octx) throw new Error('offscreen context unavailable');
        octx.drawImage(img, 0, 0, dw, dh);
        const data = octx.getImageData(0, 0, dw, dh).data;
        const r = new Array<number>(BARS).fill(0);
        const g = new Array<number>(BARS).fill(0);
        const b = new Array<number>(BARS).fill(0);
        const l = new Array<number>(BARS).fill(0);
        for (let i = 0; i < data.length; i += 4) {
          const R = data[i];
          const G = data[i + 1];
          const B = data[i + 2];
          r[R] += 1;
          g[G] += 1;
          b[B] += 1;
          l[(R + G + B) / 3 | 0] += 1;
        }
        const max = Math.max(1, ...r, ...g, ...b);

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
        drawChannel(l, 'rgba(167,173,183,0.28)');
        drawChannel(r, 'rgba(255,69,58,0.45)');
        drawChannel(g, 'rgba(48,209,88,0.42)');
        drawChannel(b, 'rgba(10,132,255,0.45)');
        setUnavailable(false);
      } catch {
        // 跨域 / taint：getImageData 抛错 → 显示不可用角标（Phase B 接引擎直方图）
        setUnavailable(true);
      }
    };
    img.onerror = () => setUnavailable(true);
    img.src = src;
  }, [src]);

  if (!src) return null;

  return (
    <div
      className="absolute bottom-2 left-2 rounded-md border border-line overflow-hidden pointer-events-none"
      style={{ background: 'var(--Lumina-Tokens-viewerOverlay)' }}
      role="img"
      aria-label="RGB 直方图"
    >
      {unavailable ? (
        <div className="px-2 py-1 text-2xs whitespace-nowrap" style={{ color: 'var(--Lumina-Tokens-textMuted)' }}>
          直方图暂不可用
        </div>
      ) : (
        <canvas ref={canvasRef} style={{ width: W, height: H, display: 'block' }} />
      )}
    </div>
  );
}
