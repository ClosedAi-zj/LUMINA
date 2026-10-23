/**
 * 统一三态组件（WP-B3）：空态 / 加载中 / 失败重试。
 *
 * 红线（1.0 修图台黑屏事件的教训）：任何 catch 都必须有 UI 呈现，不允许静默。
 * 所有颜色只走设计令牌，禁止硬编码色值（#08090B 观察区除外）。
 */

import type { ReactNode } from 'react';
import { Spinner } from './primitives';
import { Icon } from './shell';

/** 空态：无数据但有恢复入口 */
export function StateEmpty({
  title,
  desc,
  action,
  icon = 'empty',
  compact = false,
}: {
  title: string;
  desc?: string;
  action?: ReactNode;
  icon?: string;
  compact?: boolean;
}) {
  return (
    <div className={`flex flex-col items-center justify-center gap-2.5 ${compact ? 'py-8' : 'h-full py-16'}`}>
      <Icon name={icon} size={compact ? 18 : 26} className="text-txt3" />
      <div className="text-xs text-txt2">{title}</div>
      {desc && <div className="text-2xs text-txt3 max-w-sm text-center leading-5">{desc}</div>}
      {action}
    </div>
  );
}

/** 加载中：阶段化文案，避免"转圈但不知道在等什么" */
export function StateLoading({ label = '加载中…', desc, compact = false }: { label?: string; desc?: string; compact?: boolean }) {
  return (
    <div className={`flex flex-col items-center justify-center gap-2.5 ${compact ? 'py-8' : 'h-full py-16'}`} role="status" aria-live="polite">
      <Spinner size={compact ? 14 : 18} />
      <div className="text-xs text-txt2">{label}</div>
      {desc && <div className="text-2xs text-txt3 max-w-sm text-center leading-5">{desc}</div>}
    </div>
  );
}

/** 失败态：必须给错误原文 + 重试入口（重试可为空时只显示原文） */
export function StateError({
  title = '加载失败',
  error,
  onRetry,
  retryLabel = '重试',
  compact = false,
  extra,
}: {
  title?: string;
  error: string;
  onRetry?: () => void;
  retryLabel?: string;
  compact?: boolean;
  extra?: ReactNode;
}) {
  return (
    <div className={`flex flex-col items-center justify-center gap-2.5 ${compact ? 'py-8' : 'h-full py-16'}`} role="alert">
      <Icon name="warning" size={compact ? 18 : 26} className="text-danger" />
      <div className="text-xs text-txt">{title}</div>
      <div className="text-2xs text-txt3 max-w-md text-center leading-5 break-all">{error}</div>
      {extra}
      {onRetry && (
        <button
          className="h-7 px-3 rounded-md border border-line text-xs text-txt2 hover:text-txt hover:bg-hover transition-colors"
          onClick={onRetry}
        >
          {retryLabel}
        </button>
      )}
    </div>
  );
}

/** 内联失败条：局部区块出错但页面其余部分可用的场景 */
export function InlineError({ error, onRetry, text = '该区块加载失败' }: { error: string; onRetry?: () => void; text?: string }) {
  return (
    <div
      className="flex items-start gap-2 px-2.5 py-1.5 rounded-md border text-2xs leading-4"
      style={{ background: 'var(--Lumina-Tokens-card)', borderColor: 'var(--Lumina-Tokens-danger)' }}
      role="alert"
    >
      <Icon name="warning" size={12} className="text-danger shrink-0 mt-0.5" />
      <div className="flex-1 min-w-0">
        <div className="text-txt2">{text}</div>
        <div className="text-txt3 break-all">{error}</div>
      </div>
      {onRetry && (
        <button className="text-accent hover:opacity-80 shrink-0" onClick={onRetry}>
          重试
        </button>
      )}
    </div>
  );
}
