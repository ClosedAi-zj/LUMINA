/**
 * 冷启动三段进度（WP-B3）：引擎握手 → 库加载 → 缩略图就绪。
 *
 * 目的：大库首次启动（5 万张）过去只显示一个转圈，用户不知道卡在哪一段；
 * 现在每段都有明确文案与失败态（失败不吞，给「重试」与「跳过继续」）。
 */

import { Icon } from './shell';
import { Spinner } from './primitives';
import type { BootStage } from '../store/uiStore';

export interface StageInfo {
  key: BootStage;
  label: string;
  desc: string;
}

export const BOOT_STAGES: StageInfo[] = [
  { key: 'handshake', label: '引擎握手', desc: '连接图像引擎并协商协议版本' },
  { key: 'library', label: '库加载', desc: '读取目录树、时间线与统计' },
  { key: 'thumbs', label: '缩略图就绪', desc: '预取首屏缩略图' },
];

const ORDER: BootStage[] = ['handshake', 'library', 'thumbs', 'ready'];

function stageIndex(s: BootStage): number {
  const i = ORDER.indexOf(s);
  return i < 0 ? 0 : i;
}

export default function BootProgress({
  stage,
  error,
  onRetry,
  onSkip,
}: {
  stage: BootStage;
  error?: string | null;
  onRetry?: () => void;
  onSkip?: () => void;
}) {
  const cur = stageIndex(stage);
  return (
    <div
      className="absolute inset-0 z-50 flex flex-col items-center justify-center gap-5"
      style={{ background: 'var(--Lumina-Tokens-canvas)' }}
      role="status"
      aria-live="polite"
    >
      <div className="flex items-center gap-2">
        <span className="w-6 h-6 rounded-md bg-accent flex items-center justify-center text-onaccent text-xs font-bold">L</span>
        <span className="text-sm font-semibold tracking-wide text-txt">LUMINA</span>
        <span className="text-2xs text-txt3">正在启动</span>
      </div>

      <div className="w-80 flex flex-col gap-2.5">
        {BOOT_STAGES.map((s, i) => {
          const done = cur > i || stage === 'ready';
          const active = cur === i && stage !== 'ready';
          const failed = !!error && cur === i;
          return (
            <div key={s.key} className="flex items-start gap-2.5">
              <div className="w-4 h-4 shrink-0 flex items-center justify-center mt-0.5">
                {failed ? (
                  <Icon name="warning" size={12} className="text-danger" />
                ) : done ? (
                  <svg width="12" height="12" viewBox="0 0 24 24" aria-hidden className="text-success">
                    <path d="M4 12l6 6L20 6" fill="none" stroke="currentColor" strokeWidth="2.4" />
                  </svg>
                ) : active ? (
                  <Spinner size={12} />
                ) : (
                  <span className="w-1.5 h-1.5 rounded-full" style={{ background: 'var(--Lumina-Tokens-textMuted)' }} />
                )}
              </div>
              <div className="min-w-0">
                <div className={`text-xs ${failed ? 'text-danger' : done || active ? 'text-txt' : 'text-txt3'}`}>{s.label}</div>
                <div className="text-2xs text-txt3 leading-4">{s.desc}</div>
              </div>
            </div>
          );
        })}
      </div>

      {error ? (
        <div className="w-80 flex flex-col gap-2">
          <div className="text-2xs text-danger break-all leading-4">{error}</div>
          <div className="flex items-center gap-2">
            {onRetry && (
              <button className="h-7 px-3 rounded-md bg-accent text-onaccent text-xs font-medium" onClick={onRetry}>
                重试
              </button>
            )}
            {onSkip && (
              <button className="h-7 px-3 rounded-md border border-line text-xs text-txt2 hover:text-txt hover:bg-hover" onClick={onSkip}>
                跳过，先进入界面
              </button>
            )}
          </div>
        </div>
      ) : (
        <div className="w-80 h-0.5 rounded-full overflow-hidden" style={{ background: 'var(--Lumina-Tokens-border)' }}>
          <div
            className="h-full transition-all duration-300"
            style={{ width: `${Math.round((cur / BOOT_STAGES.length) * 100)}%`, background: 'var(--Lumina-Tokens-accent)' }}
          />
        </div>
      )}
    </div>
  );
}

/** 供冒烟/诊断读取：当前阶段名 */
export function stageLabel(s: BootStage): string {
  return BOOT_STAGES.find((x) => x.key === s)?.label ?? s;
}
