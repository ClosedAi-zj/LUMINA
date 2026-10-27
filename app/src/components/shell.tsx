/** 对话框 / 空状态 / Toast 宿主 / 键帽条 / 图标集 */

import type { ReactNode } from 'react';
import { useJobStore } from '../store/jobStore';
import { useTheme } from '../theme/ThemeProvider';

// ---------- Dialog ----------

export function Dialog({ open, title, children, onClose, width = 420 }: { open: boolean; title: string; children: ReactNode; onClose?: () => void; width?: number }) {
  if (!open) return null;
  return (
    <div className="fixed inset-0 z-50 flex items-center justify-center" style={{ background: 'rgba(0,0,0,0.55)' }} onClick={onClose}>
      {/* R7 M2：模态浮层统一玻璃材质 + 浮入动效（浅色主题玻璃底 = --glass 令牌，文字对比 ≥4.5:1） */}
      <div
        className="glass-m2 rounded-lgC border border-line animate-float-in"
        style={{ width }}
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
          className="pointer-events-auto flex items-center gap-3 px-4 py-2 rounded-mdC border border-line glass-m2 animate-float-in text-xs"
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
// batch 1 全局规范：1.5px 线性、单色 currentColor，16/20px 双档由调用方 size 决定。

export function Icon({ name, size = 16, className = '' }: { name: string; size?: number; className?: string }) {
  const paths: Record<string, ReactNode> = {
    search: <circle cx="10" cy="10" r="6" fill="none" stroke="currentColor" strokeWidth="1.5" />,
    sun: (
      <>
        <circle cx="12" cy="12" r="4" fill="none" stroke="currentColor" strokeWidth="1.5" />
        <path d="M12 2v3M12 19v3M2 12h3M19 12h3M4.9 4.9l2.1 2.1M17 17l2.1 2.1M19.1 4.9L17 7M7 17l-2.1 2.1" stroke="currentColor" strokeWidth="1.5" />
      </>
    ),
    moon: <path d="M20 13A8 8 0 1 1 11 4a6.5 6.5 0 0 0 9 9z" fill="none" stroke="currentColor" strokeWidth="1.5" />,
    folder: <path d="M3 5h6l2 2h10v11H3z" fill="none" stroke="currentColor" strokeWidth="1.5" />,
    star: <path d="M12 3l2.7 5.6 6.1.8-4.5 4.3 1.1 6-5.4-2.9-5.4 2.9 1.1-6L3.2 9.4l6.1-.8z" fill="currentColor" opacity="0.9" />,
    pin: (
      <>
        <path d="M12 21s-6-5.7-6-10a6 6 0 1 1 12 0c0 4.3-6 10-6 10z" fill="none" stroke="currentColor" strokeWidth="1.5" />
        <circle cx="12" cy="11" r="2" fill="currentColor" />
      </>
    ),
    edit: <circle cx="12" cy="12" r="3" fill="currentColor" />,
    clock: (
      <>
        <circle cx="12" cy="12" r="8" fill="none" stroke="currentColor" strokeWidth="1.5" />
        <path d="M12 7v5l3 2" fill="none" stroke="currentColor" strokeWidth="1.5" />
      </>
    ),
    heart: <path d="M12 20s-7-4.6-7-9.5A4 4 0 0 1 12 7a4 4 0 0 1 7 3.5C19 15.4 12 20 12 20z" fill="currentColor" opacity="0.9" />,
    warning: <path d="M12 4l9 15H3zM12 10v4M12 16.5v.5" fill="none" stroke="currentColor" strokeWidth="1.5" />,
    empty: (
      <>
        <rect x="4" y="4" width="16" height="16" rx="2" fill="none" stroke="currentColor" strokeWidth="1.5" strokeDasharray="3 2.4" />
        <path d="M9 12h6" stroke="currentColor" strokeWidth="1.5" />
      </>
    ),
    play: <path d="M8 5l11 7-11 7z" fill="currentColor" />,
    pause: <path d="M7 5h4v14H7zM13 5h4v14h-4z" fill="currentColor" />,
    back: <path d="M14 6l-6 6 6 6" fill="none" stroke="currentColor" strokeWidth="1.5" />,
    forward: <path d="M10 6l6 6-6 6" fill="none" stroke="currentColor" strokeWidth="1.5" />,
    plus: <path d="M12 5v14M5 12h14" fill="none" stroke="currentColor" strokeWidth="1.5" />,
    download: <path d="M12 4v10M7 10l5 5 5-5M5 19h14" fill="none" stroke="currentColor" strokeWidth="1.5" />,
    x: <path d="M6 6l12 12M18 6L6 18" fill="none" stroke="currentColor" strokeWidth="1.5" />,
    settings: (
      <>
        <circle cx="12" cy="12" r="3" fill="none" stroke="currentColor" strokeWidth="1.5" />
        <path d="M12 2v3M12 19v3M2 12h3M19 12h3" stroke="currentColor" strokeWidth="1.5" />
      </>
    ),
    // —— LR 式专项图标（1.5px 线性 / currentColor，与 batch 1 规范一致）——
    crop: (
      <>
        <path d="M8 2v13a1 1 0 0 0 1 1h13" fill="none" stroke="currentColor" strokeWidth="1.5" strokeLinecap="round" strokeLinejoin="round" />
        <path d="M2 8h13a1 1 0 0 1 1 1v13" fill="none" stroke="currentColor" strokeWidth="1.5" strokeLinecap="round" strokeLinejoin="round" />
      </>
    ),
    brush: (
      <>
        <path d="M4 20c1.5-1 2.5-2.5 4-4M8 16l6-6" fill="none" stroke="currentColor" strokeWidth="1.5" strokeLinecap="round" strokeLinejoin="round" />
        <path d="M13 5l6 6-3 3-6-6z" fill="none" stroke="currentColor" strokeWidth="1.5" strokeLinejoin="round" />
      </>
    ),
    gradient: (
      <>
        <rect x="5" y="4" width="14" height="16" rx="1" fill="none" stroke="currentColor" strokeWidth="1.5" />
        <path d="M5 9h14M5 12h14M5 15h14" stroke="currentColor" strokeWidth="1.5" opacity="0.45" />
      </>
    ),
    mask: (
      <>
        <circle cx="12" cy="12" r="7" fill="none" stroke="currentColor" strokeWidth="1.5" strokeDasharray="3 2.2" />
        <circle cx="12" cy="12" r="2.5" fill="none" stroke="currentColor" strokeWidth="1.5" />
      </>
    ),
    histogram: <path d="M4 20v-6M9 20v-11M14 20v-4M19 20v-9" fill="none" stroke="currentColor" strokeWidth="1.5" strokeLinecap="round" />,
    sliders: (
      <>
        <path d="M4 8h7M15 8h5M4 16h5M13 16h7" fill="none" stroke="currentColor" strokeWidth="1.5" strokeLinecap="round" />
        <circle cx="13" cy="8" r="1.8" fill="currentColor" />
        <circle cx="11" cy="16" r="1.8" fill="currentColor" />
      </>
    ),
    compare: (
      <>
        <rect x="3" y="5" width="7" height="14" rx="1" fill="none" stroke="currentColor" strokeWidth="1.5" />
        <rect x="14" y="5" width="7" height="14" rx="1" fill="none" stroke="currentColor" strokeWidth="1.5" />
      </>
    ),
    spot: (
      <>
        <circle cx="12" cy="12" r="6" fill="none" stroke="currentColor" strokeWidth="1.5" />
        <path d="M12 8.5v7M8.5 12h7" stroke="currentColor" strokeWidth="1.5" strokeLinecap="round" />
      </>
    ),
    sync: (
      <>
        <path d="M4 12a8 8 0 0 1 13.5-5.5L20 9" fill="none" stroke="currentColor" strokeWidth="1.5" strokeLinecap="round" strokeLinejoin="round" />
        <path d="M20 4v5h-5" fill="none" stroke="currentColor" strokeWidth="1.5" strokeLinecap="round" strokeLinejoin="round" />
        <path d="M20 12a8 8 0 0 1-13.5 5.5L4 15" fill="none" stroke="currentColor" strokeWidth="1.5" strokeLinecap="round" strokeLinejoin="round" />
        <path d="M4 20v-5h5" fill="none" stroke="currentColor" strokeWidth="1.5" strokeLinecap="round" strokeLinejoin="round" />
      </>
    ),
    reset: (
      <>
        <path d="M4 10a8 8 0 1 1 1.5 5" fill="none" stroke="currentColor" strokeWidth="1.5" strokeLinecap="round" />
        <path d="M4 5v5h5" fill="none" stroke="currentColor" strokeWidth="1.5" strokeLinecap="round" strokeLinejoin="round" />
      </>
    ),
    auto: <path d="M12 4l1.6 4.4L18 10l-4.4 1.6L12 16l-1.6-4.4L6 10l4.4-1.6z" fill="currentColor" opacity="0.9" />,
    book: <path d="M4 5a2 2 0 0 1 2-2h13v15H6a2 2 0 0 0-2 2V5z" fill="none" stroke="currentColor" strokeWidth="1.5" strokeLinejoin="round" />,
    print: (
      <>
        <path d="M7 9V4h10v5" fill="none" stroke="currentColor" strokeWidth="1.5" strokeLinejoin="round" />
        <path d="M5 9h14a2 2 0 0 1 2 2v5h-4v4H7v-4H3v-5a2 2 0 0 1 2-2z" fill="none" stroke="currentColor" strokeWidth="1.5" strokeLinejoin="round" />
      </>
    ),
    web: (
      <>
        <circle cx="12" cy="12" r="8" fill="none" stroke="currentColor" strokeWidth="1.5" />
        <path d="M4 12h16M12 4c2.4 2.4 2.4 13.6 0 16M12 4c-2.4 2.4-2.4 13.6 0 16" fill="none" stroke="currentColor" strokeWidth="1.5" />
      </>
    ),
    curve: <path d="M4 16c4 0 4-8 8-8s4 8 8 8" fill="none" stroke="currentColor" strokeWidth="1.5" strokeLinecap="round" />,
    droplet: <path d="M12 3s5 5.5 5 9a5 5 0 0 1-10 0c0-3.5 5-9 5-9z" fill="none" stroke="currentColor" strokeWidth="1.5" strokeLinejoin="round" />,
    lens: (
      <>
        <circle cx="12" cy="12" r="7" fill="none" stroke="currentColor" strokeWidth="1.5" />
        <circle cx="12" cy="12" r="3" fill="none" stroke="currentColor" strokeWidth="1.5" />
      </>
    ),
    target: (
      <>
        <circle cx="12" cy="12" r="7" fill="none" stroke="currentColor" strokeWidth="1.5" />
        <path d="M12 2v3M12 19v3M2 12h3M19 12h3" stroke="currentColor" strokeWidth="1.5" strokeLinecap="round" />
      </>
    ),
    transform: (
      <path d="M4 9h11l-3-3M20 15H9l3 3" fill="none" stroke="currentColor" strokeWidth="1.5" strokeLinecap="round" strokeLinejoin="round" />
    ),
    grid: <path d="M4 4h7v7H4zM13 4h7v7h-7zM4 13h7v7H4zM13 13h7v7h-7z" fill="none" stroke="currentColor" strokeWidth="1.5" strokeLinejoin="round" />,
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
