/** 对话框 / 空状态 / Toast 宿主 / 键帽条 / 图标集 */

import type { ReactNode } from 'react';
import { useJobStore } from '../store/jobStore';
import { useTheme } from '../theme/ThemeProvider';

// ---------- Dialog ----------

export function Dialog({ open, title, children, onClose, width = 420 }: { open: boolean; title: string; children: ReactNode; onClose?: () => void; width?: number }) {
  if (!open) return null;
  return (
    <div className="fixed inset-0 z-50 flex items-center justify-center" style={{ background: 'rgba(0,0,0,0.55)' }} onClick={onClose}>
      <div
        className="rounded-lgC border border-line bg-panel"
        style={{ width, boxShadow: 'var(--Lumina-Tokens-shadow2)' }}
        onClick={(e) => e.stopPropagation()}
      >
        <div className="flex items-center justify-between px-4 h-11 border-b border-line">
          <span className="text-sm font-medium">{title}</span>
          {onClose && (
            <button className="text-txt3 hover:text-txt" onClick={onClose} aria-label="关闭">
              ✕
            </button>
          )}
        </div>
        <div className="p-4">{children}</div>
      </div>
    </div>
  );
}

// ---------- EmptyState ----------

export function EmptyState({ icon, title, desc, action }: { icon?: ReactNode; title: string; desc?: string; action?: ReactNode }) {
  return (
    <div className="flex flex-col items-center justify-center h-full gap-3 py-16">
      <div className="text-txt3 text-4xl">{icon ?? '◎'}</div>
      <div className="text-sm text-txt2">{title}</div>
      {desc && <div className="text-2xs text-txt3 max-w-sm text-center leading-5">{desc}</div>}
      {action}
    </div>
  );
}

// ---------- ToastHost（含 5s 撤销） ----------

export function ToastHost() {
  const toasts = useJobStore((s) => s.toasts);
  const dismiss = useJobStore((s) => s.dismissToast);
  return (
    <div className="fixed bottom-4 left-1/2 -translate-x-1/2 z-[60] flex flex-col items-center gap-2 pointer-events-none">
      {toasts.map((t) => (
        <div
          key={t.id}
          className="pointer-events-auto flex items-center gap-3 px-4 py-2 rounded-mdC border border-line bg-panel text-xs"
          style={{ boxShadow: 'var(--Lumina-Tokens-shadow2)' }}
        >
          <span
            className="w-1.5 h-1.5 rounded-full"
            style={{ background: t.kind === 'error' ? 'var(--Lumina-Tokens-danger)' : t.kind === 'success' ? 'var(--Lumina-Tokens-success)' : t.kind === 'warn' ? 'var(--Lumina-Tokens-accent)' : 'var(--Lumina-Tokens-textMuted)' }}
          />
          <span className="text-txt">{t.text}</span>
          {t.undoFn && (
            <button
              className="text-accent hover:opacity-80 font-medium"
              onClick={() => {
                void t.undoFn?.();
                dismiss(t.id);
              }}
            >
              撤销
            </button>
          )}
          <button className="text-txt3 hover:text-txt" onClick={() => dismiss(t.id)} aria-label="关闭提示">
            ✕
          </button>
        </div>
      ))}
    </div>
  );
}

// ---------- KeycapBar ----------

export function KeycapBar({ items }: { items: { keys: string[]; label: string }[] }) {
  return (
    <div className="flex items-center gap-4 text-2xs text-txt3 px-3 h-8 border-t border-line bg-panel overflow-x-auto whitespace-nowrap">
      {items.map((it, i) => (
        <span key={i} className="inline-flex items-center gap-1.5">
          {it.keys.map((k) => (
            <span key={k} className="keycap">
              {k}
            </span>
          ))}
          <span>{it.label}</span>
        </span>
      ))}
    </div>
  );
}

// ---------- 图标集（内联 SVG，currentColor） ----------

export function Icon({ name, size = 16, className = '' }: { name: string; size?: number; className?: string }) {
  const paths: Record<string, ReactNode> = {
    search: <circle cx="10" cy="10" r="6" fill="none" stroke="currentColor" strokeWidth="1.6" />,
    sun: (
      <>
        <circle cx="12" cy="12" r="4" fill="none" stroke="currentColor" strokeWidth="1.6" />
        <path d="M12 2v3M12 19v3M2 12h3M19 12h3M4.9 4.9l2.1 2.1M17 17l2.1 2.1M19.1 4.9L17 7M7 17l-2.1 2.1" stroke="currentColor" strokeWidth="1.6" />
      </>
    ),
    moon: <path d="M20 13A8 8 0 1 1 11 4a6.5 6.5 0 0 0 9 9z" fill="none" stroke="currentColor" strokeWidth="1.6" />,
    folder: <path d="M3 5h6l2 2h10v11H3z" fill="none" stroke="currentColor" strokeWidth="1.6" />,
    star: <path d="M12 3l2.7 5.6 6.1.8-4.5 4.3 1.1 6-5.4-2.9-5.4 2.9 1.1-6L3.2 9.4l6.1-.8z" fill="currentColor" opacity="0.9" />,
    pin: (
      <>
        <path d="M12 21s-6-5.7-6-10a6 6 0 1 1 12 0c0 4.3-6 10-6 10z" fill="none" stroke="currentColor" strokeWidth="1.6" />
        <circle cx="12" cy="11" r="2" fill="currentColor" />
      </>
    ),
    edit: <circle cx="12" cy="12" r="3" fill="currentColor" />,
    clock: (
      <>
        <circle cx="12" cy="12" r="8" fill="none" stroke="currentColor" strokeWidth="1.6" />
        <path d="M12 7v5l3 2" fill="none" stroke="currentColor" strokeWidth="1.6" />
      </>
    ),
    heart: <path d="M12 20s-7-4.6-7-9.5A4 4 0 0 1 12 7a4 4 0 0 1 7 3.5C19 15.4 12 20 12 20z" fill="currentColor" opacity="0.9" />,
    warning: <path d="M12 4l9 15H3zM12 10v4M12 16.5v.5" fill="none" stroke="currentColor" strokeWidth="1.7" />,
    empty: (
      <>
        <rect x="4" y="4" width="16" height="16" rx="2" fill="none" stroke="currentColor" strokeWidth="1.6" strokeDasharray="3 2.4" />
        <path d="M9 12h6" stroke="currentColor" strokeWidth="1.6" />
      </>
    ),
    play: <path d="M8 5l11 7-11 7z" fill="currentColor" />,
    pause: <path d="M7 5h4v14H7zM13 5h4v14h-4z" fill="currentColor" />,
    back: <path d="M14 6l-6 6 6 6" fill="none" stroke="currentColor" strokeWidth="1.8" />,
    forward: <path d="M10 6l6 6-6 6" fill="none" stroke="currentColor" strokeWidth="1.8" />,
    plus: <path d="M12 5v14M5 12h14" fill="none" stroke="currentColor" strokeWidth="1.8" />,
    download: <path d="M12 4v10M7 10l5 5 5-5M5 19h14" fill="none" stroke="currentColor" strokeWidth="1.7" />,
    x: <path d="M6 6l12 12M18 6L6 18" fill="none" stroke="currentColor" strokeWidth="1.8" />,
    settings: (
      <>
        <circle cx="12" cy="12" r="3" fill="none" stroke="currentColor" strokeWidth="1.6" />
        <path d="M12 2v3M12 19v3M2 12h3M19 12h3" stroke="currentColor" strokeWidth="1.6" />
      </>
    ),
  };
  return (
    <svg width={size} height={size} viewBox="0 0 24 24" className={className} aria-hidden>
      {paths[name] ?? null}
    </svg>
  );
}

// ---------- 主题切换按钮 ----------

export function ThemeToggle() {
  const { theme, toggle } = useTheme();
  return (
    <button
      onClick={toggle}
      className="inline-flex items-center gap-1.5 h-7 px-2.5 rounded-md border border-line bg-card text-txt2 hover:text-txt hover:bg-hover text-2xs"
      title={theme === 'Dark' ? '切换到浅色主题' : '切换到深色主题'}
    >
      <Icon name={theme === 'Dark' ? 'moon' : 'sun'} size={13} />
      {theme === 'Dark' ? '深色' : '浅色'}
    </button>
  );
}
