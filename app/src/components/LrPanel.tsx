/**
 * LR 风格可折叠面板（Phase A）：标题栏 + 折叠态 + 发丝线分隔，密度更高、视觉更考究。
 * 用 --Lumina-Tokens-panel/card/line 描边与分层；关键数值由消费方用 --Lumina-Tokens-gold 着色。
 *
 * Props：
 *  - title：面板标题（小型大写，复用全局 .panel-section-title）
 *  - collapsed：初始折叠态（仅作初始值，用户可随时展开/收起）
 *  - right：标题栏右侧插槽（如状态/计数）
 *  - icon：标题栏左侧图标（ReactNode）
 *  - children：面板内容
 */

import { useState, type ReactNode } from 'react';

const EASE = 'cubic-bezier(.2,.8,.2,1)';

export function LrPanel({
  title,
  collapsed,
  right,
  icon,
  children,
}: {
  title: string;
  collapsed?: boolean;
  right?: ReactNode;
  icon?: ReactNode;
  children: ReactNode;
}) {
  const [open, setOpen] = useState(collapsed ?? false);
  return (
    <section className="shrink-0 overflow-hidden border-b border-line">
      <button
        type="button"
        onClick={() => setOpen((v) => !v)}
        aria-expanded={open}
        className="section-head w-full h-[30px] px-2.5 flex items-center gap-1.5 text-left transition-colors"
        style={{ transitionTimingFunction: EASE }}
      >
        {icon && <span className="shrink-0 text-txt3">{icon}</span>}
        <span className="panel-section-title truncate">{title}</span>
        <span className="flex-1" />
        {right}
        <svg
          width="14"
          height="14"
          viewBox="0 0 24 24"
          className="text-txt3 shrink-0"
          style={{ transform: open ? 'rotate(0deg)' : 'rotate(-90deg)', transition: `transform 120ms ${EASE}` }}
          aria-hidden
        >
          <path d="M7 10l5 5 5-5" fill="none" stroke="currentColor" strokeWidth="1.5" />
        </svg>
      </button>
      {open && <div className="pb-2 animate-panel-in">{children}</div>}
    </section>
  );
}
