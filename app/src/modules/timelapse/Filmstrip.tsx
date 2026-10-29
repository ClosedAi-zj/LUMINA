/**
 * 胶片条（Phase A）：基于当前帧做窗口化懒加载，仅请求当前帧前后各 N 帧的缩略图
 * （复用 store 的 framePreviewsRequested 去重机制，超出窗口不请求）。
 * 关键帧高亮（gold）；点击缩略跳帧；当前帧自动居中。
 */

import { useEffect, useRef } from 'react';
import { useTimelapse } from './store/timelapseStore';
import { fileUrl } from '../../api/rpc-client';

const WINDOW = 20;

export default function Filmstrip() {
  const selectedSeqId = useTimelapse((s) => s.selectedSeqId);
  const frameNo = useTimelapse((s) => s.frameNo);
  const detail = useTimelapse((s) => s.detail);
  const framePreviews = useTimelapse((s) => s.framePreviews);
  const ramp = useTimelapse((s) => s.ramp);
  const scrollRef = useRef<HTMLDivElement>(null);
  const currentRef = useRef<HTMLButtonElement>(null);

  const total = detail?.frameCount ?? 0;

  // 窗口化懒加载：仅请求 [lo,hi] 内帧的 320 缩略（store 内部去重，重复调用不会重复请求）
  useEffect(() => {
    if (selectedSeqId === null || total === 0) return;
    const lo = Math.max(1, frameNo - WINDOW);
    const hi = Math.min(total, frameNo + WINDOW);
    for (let n = lo; n <= hi; n++) {
      void useTimelapse.getState().ensureFramePreview(selectedSeqId, n, 320);
    }
  }, [selectedSeqId, frameNo, total]);

  // 当前帧自动居中（只滚动胶片条容器自身，不扰动页面）
  useEffect(() => {
    const el = currentRef.current;
    const box = scrollRef.current;
    if (el && box) {
      const target = el.offsetLeft - box.clientWidth / 2 + el.clientWidth / 2;
      box.scrollTo({ left: target, behavior: 'smooth' });
    }
  }, [frameNo]);

  if (selectedSeqId === null || total === 0) return null;

  const lo = Math.max(1, frameNo - WINDOW);
  const hi = Math.min(total, frameNo + WINDOW);
  const keyframeSet = new Set(ramp.map((k) => k.frameNo));

  return (
    <div className="shrink-0 border-t border-line" style={{ background: 'var(--Lumina-Tokens-panel)' }}>
      <div ref={scrollRef} className="flex items-center gap-1 px-2 py-1.5 overflow-x-auto">
        {Array.from({ length: hi - lo + 1 }, (_, i) => lo + i).map((n) => {
          const path = framePreviews[`${selectedSeqId}_${n}_320`];
          const isKey = keyframeSet.has(n);
          const isCurrent = n === frameNo;
          return (
            <button
              key={n}
              ref={isCurrent ? currentRef : undefined}
              type="button"
              onClick={() => useTimelapse.getState().setFrameNo(n)}
              aria-label={`跳到第 ${n} 帧`}
              aria-current={isCurrent}
              className="relative shrink-0 rounded-sm overflow-hidden transition-transform"
              style={{
                width: 56,
                height: 36,
                border: isKey ? '1.5px solid var(--Lumina-Tokens-gold)' : '1px solid var(--Lumina-Tokens-border)',
                outline: isCurrent ? '2px solid var(--Lumina-Tokens-gold)' : 'none',
                outlineOffset: isCurrent ? '1px' : '0',
              }}
            >
              {path ? (
                <img src={fileUrl(path)} alt="" draggable={false} className="w-full h-full object-cover" />
              ) : (
                <div className="w-full h-full flex items-center justify-center" style={{ background: 'var(--Lumina-Tokens-card)' }}>
                  <span className="num text-2xs text-txt3">{n}</span>
                </div>
              )}
            </button>
          );
        })}
      </div>
    </div>
  );
}
