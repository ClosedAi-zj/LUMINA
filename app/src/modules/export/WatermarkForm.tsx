/**
 * 水印表单（WP-B8）：开关 + 文字 / 引擎锚点 / 不透明度 + 所见即所得预览。
 *
 * 与 v1.0 的差异（都是实打实修的错位）：
 *  - 位置值改为引擎原生的 5 个锚点（bottom-right 等）。旧版发的是 camelCase
 *    （bottomRight 等 9 宫格），引擎 WatermarkSpec::fromJson 不认识 → 全部静默
 *    落到 bottom-right，用户选哪都一样；
 *  - 缩放滑杆改用引擎语义（相对「图宽 12%」基准的倍率，0.05..8）。旧版发 2..30
 *    的整数，2 = 24% 图宽、8 = 96% 图宽，都不是用户在 UI 上以为的意思；
 *  - 文字水印的字号由引擎按「图高 5%」固定，scale 对文字**不生效**（引擎现状，
 *    已列入协议缺口清单），预览如实呈现这一点。
 */

import { useEffect, useRef } from 'react';
import { useExport } from './store/exportStore';
import { Slider, Toggle } from '../../components/primitives';
import { renderTextWatermark, countUnsupportedChars, hasNonAscii } from './watermarkPreview';

/** 与引擎 WatermarkSpec::fromJson kPositions 一致（多选会静默落到 bottom-right） */
const POSITIONS: { key: string; label: string }[] = [
  { key: 'top-left', label: '↖' },
  { key: 'top-right', label: '↗' },
  { key: 'center', label: '·' },
  { key: 'bottom-left', label: '↙' },
  { key: 'bottom-right', label: '↘' },
];

const PREVIEW_W = 384;
const PREVIEW_H = 216; // 16:9

function drawPreview(canvas: HTMLCanvasElement, wm: { enabled: boolean; position: string; scale: number; opacity: number; text?: string }): void {
  const ctx = canvas.getContext('2d');
  if (!ctx) return;
  // 底图：渐变 + 山形剪影的示意样张（只影响观感，不影响水印几何）
  const grad = ctx.createLinearGradient(0, 0, 0, PREVIEW_H);
  grad.addColorStop(0, '#2c3e55');
  grad.addColorStop(0.55, '#5d7b98');
  grad.addColorStop(1, '#181a1f');
  ctx.fillStyle = grad;
  ctx.fillRect(0, 0, PREVIEW_W, PREVIEW_H);
  ctx.fillStyle = 'rgba(0,0,0,0.45)';
  ctx.beginPath();
  ctx.moveTo(0, PREVIEW_H);
  ctx.lineTo(PREVIEW_W * 0.22, PREVIEW_H * 0.52);
  ctx.lineTo(PREVIEW_W * 0.42, PREVIEW_H * 0.78);
  ctx.lineTo(PREVIEW_W * 0.62, PREVIEW_H * 0.44);
  ctx.lineTo(PREVIEW_W * 0.82, PREVIEW_H * 0.72);
  ctx.lineTo(PREVIEW_W, PREVIEW_H * 0.58);
  ctx.lineTo(PREVIEW_W, PREVIEW_H);
  ctx.closePath();
  ctx.fill();
  // 水印本体（引擎同参复刻）
  renderTextWatermark(ctx, PREVIEW_W, PREVIEW_H, wm);
}

export default function WatermarkForm() {
  const watermark = useExport((s) => s.spec.watermark);
  const canvasRef = useRef<HTMLCanvasElement>(null);
  const text = watermark.text ?? '';
  // 非 ASCII（中文等）已由引擎系统字体渲染；纯 ASCII 未收录字符（@ # 等）仍会显示为 '?'
  const nonAscii = hasNonAscii(text);
  const unsupported = nonAscii ? 0 : countUnsupportedChars(text);

  useEffect(() => {
    if (watermark.enabled && canvasRef.current) {
      drawPreview(canvasRef.current, watermark);
    }
  }, [watermark]);

  return (
    <div className="rounded-lg border border-line p-3" style={{ background: 'var(--Lumina-Tokens-panel)' }}>
      <div className="flex items-center justify-between mb-2">
        <span className="text-xs font-medium">水印</span>
        <Toggle checked={watermark.enabled} onChange={(v) => useExport.getState().patchWatermark({ enabled: v })} label="启用水印" />
      </div>
      {watermark.enabled && (
        <>
          {/* 所见即所得预览：非 ASCII 走系统字体、纯 ASCII 走引擎 3x5 点阵，均与导出同参 */}
          <canvas
            ref={canvasRef}
            width={PREVIEW_W}
            height={PREVIEW_H}
            className="w-full rounded-md border border-line mb-2.5"
            style={{ background: 'var(--Lumina-Tokens-card)' }}
          />
          {nonAscii && (
            <div className="text-2xs rounded-md px-2 py-1 mb-2 text-txt3" style={{ background: 'var(--Lumina-Tokens-card)' }}>
              非 ASCII 字符（中文等）由系统字体渲染，观感随系统字体而定；ASCII 字符走引擎内置点阵。
            </div>
          )}
          {unsupported > 0 && (
            <div
              className="text-2xs rounded-md px-2 py-1 mb-2"
              style={{ background: 'rgba(185,138,46,0.14)', color: 'var(--Lumina-Tokens-accent)' }}
            >
              引擎点阵仅收录 ASCII 字母数字与常用符号：{unsupported} 个字符导出时将显示为「?」
            </div>
          )}
          <div className="mb-2.5">
            <div className="text-2xs text-txt2 mb-1">水印文字</div>
            <input
              value={watermark.text ?? ''}
              onChange={(e) => useExport.getState().patchWatermark({ text: e.target.value })}
              placeholder="如：LUMINA STUDIO 或 版权所有 · 齐活林 2025"
              className="w-full h-7 px-2 rounded-md border border-line bg-card text-xs focus:outline-none focus:border-accent"
            />
          </div>
          <div className="mb-2.5">
            <div className="text-2xs text-txt2 mb-1">位置</div>
            <div className="grid grid-cols-5 gap-1 w-40">
              {POSITIONS.map((p) => (
                <button
                  key={p.key}
                  onClick={() => useExport.getState().patchWatermark({ position: p.key })}
                  title={p.key}
                  className={`h-6 rounded-sm border text-2xs transition-colors ${
                    watermark.position === p.key ? 'border-accent text-accent' : 'border-line text-txt3 hover:text-txt2 hover:bg-hover'
                  }`}
                  style={{ background: watermark.position === p.key ? 'var(--Lumina-Tokens-accentSoft)' : 'var(--Lumina-Tokens-card)' }}
                >
                  {p.label}
                </button>
              ))}
            </div>
          </div>
          <Slider
            label="不透明度"
            value={watermark.opacity}
            min={10}
            max={100}
            defaultValue={60}
            onChange={(v) => useExport.getState().patchWatermark({ opacity: v })}
            format={(v) => `${v}%`}
          />
          {/* 引擎现状：scale 只作用于图片水印（markW = 图宽 12% × scale），文字字号固定。
              UI 无图片水印入口，该滑杆暂不放开，避免给用户一个没有效果的旋钮。 */}
          <div className="text-2xs text-txt3 mt-1.5">
            缩放：引擎侧仅对图片水印生效（基准 = 图宽 12%），文字水印字号由引擎按图高 5% 固定——
            图片水印入口与「文字可缩放」已在协议缺口清单中提报。
          </div>
        </>
      )}
    </div>
  );
}
