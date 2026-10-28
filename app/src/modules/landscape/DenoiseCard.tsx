/**
 * 降噪卡（对照 12 号设计稿）：前后对比滑块（denoise.preview）+
 * 强度滑块（松开即刷新预览）+ 亮度蒙版开关 + 应用（denoise.apply 任务化）。
 *
 * 文案如实（P25）：引擎实现是**经典 NLM（非局部均值）**，不是 AI 模型，
 * 也不再挂「v1.1」徽标冒充未交付能力 —— 功能当前可用。
 */

import { useRef, useState } from 'react';
import { useLandscape } from './store/landscapeStore';
import { useJobProgress } from '../../api/hooks';
import { Slider, Spinner, Toggle } from '../../components/primitives';
import { fileUrl } from '../../api/rpc-client';

export default function DenoiseCard() {
  const photoId = useLandscape((s) => s.photoId);
  const photoName = useLandscape((s) => s.photoName);
  const origPath = useLandscape((s) => s.origPath);
  const denoisedPath = useLandscape((s) => s.denoisedPath);
  const denoisePreviewLoading = useLandscape((s) => s.denoisePreviewLoading);
  const strength = useLandscape((s) => s.strength);
  const luminanceMask = useLandscape((s) => s.luminanceMask);
  const denoiseJobId = useLandscape((s) => s.denoiseJobId);
  const denoiseJob = useJobProgress(denoiseJobId);
  const [split, setSplit] = useState(50);

  // 强度变化 400ms 防抖后刷新降噪预览
  const previewTimer = useRef<ReturnType<typeof setTimeout> | null>(null);
  const debouncedPreview = () => {
    if (previewTimer.current) clearTimeout(previewTimer.current);
    previewTimer.current = setTimeout(() => void useLandscape.getState().refreshDenoisePreview(), 400);
  };

  return (
    <div className="rounded-lg border border-line p-3 flex flex-col gap-2.5" style={{ background: 'var(--Lumina-Tokens-panel)' }}>
      <div className="flex items-center gap-2">
        <span className="text-xs font-medium">降噪（经典 NLM）</span>
        <span className="text-2xs text-txt3">非局部均值 · 非 AI 模型</span>
        <div className="flex-1" />
      </div>

      {/* 前后对比（拖动分割线；降噪侧金色角标） */}
      <div className="relative h-40 rounded-md overflow-hidden select-none" style={{ background: 'var(--Lumina-Tokens-viewer)' }}>
        {denoisedPath ? (
          <>
            <img src={fileUrl(denoisedPath)} alt="降噪后" draggable={false} className="absolute inset-0 w-full h-full object-cover" />
            {origPath && (
              <div className="absolute inset-0 overflow-hidden" style={{ width: `${split}%` }}>
                <img src={fileUrl(origPath)} alt="原图" draggable={false} className="absolute inset-0 h-full object-cover" style={{ width: `${(100 / Math.max(split, 1)) * 100}%`, maxWidth: 'none' }} />
              </div>
            )}
            <div className="absolute top-0 bottom-0 w-0.5 cursor-ew-resize" style={{ left: `${split}%`, background: 'var(--Lumina-Tokens-accent)' }}>
              <span className="absolute top-1/2 left-1/2 -translate-x-1/2 -translate-y-1/2 w-4 h-4 rounded-full bg-accent border border-onaccent" />
            </div>
            <span className="absolute top-1 left-1 text-2xs px-1 py-0.5 rounded-sm num" style={{ background: 'rgba(12,13,16,0.72)', color: 'var(--Lumina-Tokens-textSecondary)' }}>原图</span>
            <span className="absolute top-1 right-1 text-2xs px-1 py-0.5 rounded-sm font-bold" style={{ background: 'var(--Lumina-Tokens-accent)', color: 'var(--Lumina-Tokens-onAccent)' }}>降噪后</span>
            {/* 拖动热区 */}
            <div
              className="absolute top-0 bottom-0 w-6 cursor-ew-resize"
              style={{ left: `calc(${split}% - 12px)` }}
              onPointerDown={(e) => {
                const box = (e.currentTarget.parentElement as HTMLElement).getBoundingClientRect();
                const move = (ev: PointerEvent) => {
                  const ratio = ((ev.clientX - box.left) / box.width) * 100;
                  setSplit(Math.max(5, Math.min(95, ratio)));
                };
                const up = () => {
                  window.removeEventListener('pointermove', move);
                  window.removeEventListener('pointerup', up);
                };
                window.addEventListener('pointermove', move);
                window.addEventListener('pointerup', up);
              }}
            />
            {denoisePreviewLoading && (
              <div className="absolute bottom-1 right-1">
                <Spinner size={14} />
              </div>
            )}
          </>
        ) : (
          <div className="absolute inset-0 flex items-center justify-center text-2xs text-txt3">
            {photoId === null ? '无可用样张' : denoisePreviewLoading ? <Spinner size={16} /> : '降噪预览不可用'}
          </div>
        )}
      </div>

      {photoId !== null && <div className="num text-2xs text-txt3 truncate">{photoName} · 观察窗为画面中央区域</div>}

      <Slider
        label="降噪强度"
        value={strength}
        min={0}
        max={100}
        defaultValue={50}
        onChange={(v) => {
          useLandscape.getState().setStrength(v);
          debouncedPreview();
        }}
        format={(v) => `${v}`}
      />
      <div className="flex items-center gap-3">
        <Toggle checked={luminanceMask} onChange={(v) => useLandscape.getState().setLuminanceMask(v)} label="亮度蒙版" />
        <div className="flex-1" />
        <button
          className="text-2xs text-accent hover:opacity-80"
          onClick={() => {
            debouncedPreview();
          }}
        >
          刷新预览
        </button>
      </div>
      <div className="flex items-center gap-2">
        <button
          className="h-7 px-3 rounded-md text-xs font-medium disabled:opacity-40 disabled:cursor-not-allowed"
          style={{ background: 'var(--Lumina-Tokens-accent)', color: 'var(--Lumina-Tokens-onAccent)' }}
          disabled={photoId === null || denoiseJob?.state === 'running'}
          onClick={() => void useLandscape.getState().applyDenoise()}
        >
          {denoiseJob?.state === 'running' ? `降噪中 ${Math.round(denoiseJob.progress)}%` : '应用降噪（生成 DNG 衍生件）'}
        </button>
        <span className="text-2xs text-txt3">原片零写入</span>
      </div>
    </div>
  );
}
