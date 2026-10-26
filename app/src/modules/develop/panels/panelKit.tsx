/**
 * 修图台 v1.2.0 面板套件（阶段 3 皮肤）：卡片 / 参数行 / 滑块 / 开关 / 提示。
 *
 * 视觉规格（team-lead 指定，严格照做）：
 *  - 面板 = 卡片：rounded-lgC(12px) + shadow1 + 卡片间 6px
 *  - 面板标题小型大写（.panel-section-title 10px/加字距，UI 改版 batch 1 对齐 LR）、右侧 16px 折叠箭头（1.5px 线性）
 *  - 分组标题行高 28px、rounded-mdC(8px)、hover bg-hover，左标签 text-txt2、右数值 num 右对齐
 *  - 滑块：轨道 4px 圆头；球 12px（2px 面板色描边 + shadow1），hover 球 14px；激活段 accent
 *  - 过渡 120ms cubic-bezier(.2,.8,.2,1)
 * 颜色一律走令牌（除 #08090B 观察区外无硬编码色值）。
 */

import { useCallback, useRef, useState, type ReactNode } from 'react';

const EASE = 'cubic-bezier(.2,.8,.2,1)';

// ---------- 卡片 ----------

export function PanelCard({
  title,
  hint,
  defaultOpen = false,
  children,
  right,
}: {
  title: string;
  hint?: ReactNode;
  defaultOpen?: boolean;
  children: ReactNode;
  right?: ReactNode;
}) {
  const [open, setOpen] = useState(defaultOpen);
  return (
    <section
      // shrink-0：卡片在 flex 纵列里不许被压缩（否则内容超出一屏时全部挤成 13px 的"空卡"）
      // R7 §3-2：卡片不再整块描边，靠背景色差 + 间距分层；内部用 hairline 分隔标题与内容
      className="shrink-0 rounded-lgC bg-panel overflow-hidden transition-transform hover:-translate-y-px"
      style={{ boxShadow: 'var(--Lumina-Tokens-shadow1)', transitionTimingFunction: EASE, transitionDuration: '120ms' }}
    >
      <button
        onClick={() => setOpen((v) => !v)}
        className="w-full h-7 px-3 flex items-center gap-2 text-left hover:bg-hover transition-colors"
        style={{ transitionTimingFunction: EASE }}
        aria-expanded={open}
      >
        {/* batch 1 规范：分组标题小型大写（.panel-section-title，10px/加字距）+ 28px 紧凑行高 */}
        <span className="panel-section-title">{title}</span>
        {hint && <span className="text-2xs text-txt3 font-normal truncate">{hint}</span>}
        <span className="flex-1" />
        {right}
        <svg
          width="16"
          height="16"
          viewBox="0 0 24 24"
          className="text-txt3 shrink-0"
          style={{ transform: open ? 'rotate(0deg)' : 'rotate(-90deg)', transition: `transform 120ms ${EASE}` }}
          aria-hidden
        >
          <path d="M7 10l5 5 5-5" fill="none" stroke="currentColor" strokeWidth="1.5" />
        </svg>
      </button>
      {open && (
        // R7 §3-6：面板展开 240ms cubic-bezier(0.2,0.8,0.2,1)（keyframes panel-in）
        <div className="pb-2 border-t border-line animate-panel-in">
          {children}
        </div>
      )}
    </section>
  );
}

/**
 * 组内小标题（如「暗角」「颗粒」），保持 8 组归属清晰。
 * （v1.2.0 M1：93/93 叶全部参与渲染，PendingDot「未参与渲染」占位已移除。）
 */
export function GroupLabel({ children }: { children: ReactNode }) {
  return <div className="text-2xs text-txt3 px-3 pt-2 pb-1">{children}</div>;
}

// ---------- 参数行 + 滑块 ----------

export interface ParamSliderProps {
  label: string;
  value: number;
  min: number;
  max: number;
  step?: number;
  defaultValue?: number;
  format?: (v: number) => string;
  /** 拖动中（不提交历史） */
  onChange: (v: number) => void;
  /** 抬起（提交一次历史） */
  onCommit: (v: number) => void;
}

export function ParamSlider({ label, value, min, max, step = 1, defaultValue = 0, format, onChange, onCommit }: ParamSliderProps) {
  const trackRef = useRef<HTMLDivElement>(null);
  const [dragging, setDragging] = useState(false);
  const [hover, setHover] = useState(false);

  const quantize = useCallback(
    (raw: number): number => {
      const clamped = Math.max(min, Math.min(max, raw));
      if (step <= 0) return clamped;
      const snapped = Math.round(clamped / step) * step;
      // 消除浮点尾差（0.01 步长的 0.30000000000000004）
      return Number(snapped.toFixed(6));
    },
    [min, max, step],
  );

  const valueFromClientX = useCallback(
    (clientX: number): number => {
      const el = trackRef.current;
      if (!el) return value;
      const rect = el.getBoundingClientRect();
      if (rect.width <= 0) return value;
      const t = Math.max(0, Math.min(1, (clientX - rect.left) / rect.width));
      return quantize(min + t * (max - min));
    },
    [min, max, quantize, value],
  );

  const pct = max === min ? 0 : ((value - min) / (max - min)) * 100;
  const isDefault = Math.abs(value - defaultValue) < 1e-9;
  const text = format ? format(value) : String(Math.round(value * 100) / 100);

  return (
    <div
      className="h-6 mx-1 px-2 rounded-mdC hover:bg-hover transition-colors select-none"
      style={{ transitionTimingFunction: EASE }}
      onMouseEnter={() => setHover(true)}
      onMouseLeave={() => setHover(false)}
      onDoubleClick={() => {
        onChange(defaultValue);
        onCommit(defaultValue);
      }}
      title="双击复位"
    >
      <div className="flex items-center h-[15px] pt-1.5">
        <span className={`text-[11.5px] truncate ${isDefault ? 'text-txt2' : 'text-txt'}`}>{label}</span>
        <span className="flex-1" />
        <span className="num text-[11px] text-txt2 shrink-0">{text}</span>
      </div>
      <div
        ref={trackRef}
        className="relative h-1.5 rounded-pill cursor-ew-resize"
        style={{ background: 'var(--Lumina-Tokens-hover)' }}
        onPointerDown={(e) => {
          e.preventDefault();
          (e.target as HTMLElement).setPointerCapture(e.pointerId);
          setDragging(true);
          onChange(valueFromClientX(e.clientX));
        }}
        onPointerMove={(e) => {
          if (!dragging) return;
          onChange(valueFromClientX(e.clientX));
        }}
        onPointerUp={(e) => {
          if (!dragging) return;
          setDragging(false);
          onCommit(valueFromClientX(e.clientX));
        }}
        onPointerCancel={() => {
          if (!dragging) return;
          setDragging(false);
          onCommit(value);
        }}
      >
        {/* macOS 式：轨道 6px 圆头，已完成段 accent */}
        <div
          className="absolute left-0 top-0 h-1.5 rounded-pill"
          style={{ width: `${pct}%`, background: 'var(--Lumina-Tokens-accent)', transition: dragging ? 'none' : `width 120ms ${EASE}` }}
        />
        {/* 白球 + 0.5px hairline 描边 + shadow1（R7 §3-1） */}
        <div
          className="absolute rounded-full"
          style={{
            width: hover || dragging ? 14 : 12,
            height: hover || dragging ? 14 : 12,
            top: hover || dragging ? -4 : -3,
            left: `calc(${pct}% )`,
            marginLeft: hover || dragging ? -7 : -6,
            background: 'var(--Lumina-Tokens-textPrimary)',
            border: '0.5px solid var(--Lumina-Tokens-border)',
            boxShadow: 'var(--Lumina-Tokens-shadow1)',
            transition: `width 120ms ${EASE}, height 120ms ${EASE}, top 120ms ${EASE}, margin-left 120ms ${EASE}`,
          }}
        />
      </div>
    </div>
  );
}

// ---------- 开关行 ----------

export function ParamToggle({ label, checked, onChange }: { label: string; checked: boolean; onChange: (v: boolean) => void }) {
  return (
    <div className="h-6 mx-1 px-2 rounded-mdC hover:bg-hover transition-colors flex items-center gap-2" style={{ transitionTimingFunction: EASE }}>
      <span className="text-[11.5px] text-txt2 flex-1 truncate">{label}</span>
      <button
        onClick={() => onChange(!checked)}
        className="relative rounded-pill shrink-0 transition-colors"
        style={{ width: 40, height: 22, background: checked ? 'var(--Lumina-Tokens-accent)' : 'var(--Lumina-Tokens-hover)', transitionTimingFunction: EASE }}
        aria-pressed={checked}
        aria-label={label}
      >
        <span
          className="absolute rounded-full"
          style={{
            width: 18,
            height: 18,
            top: 2,
            left: checked ? 20 : 2,
            background: 'var(--Lumina-Tokens-textPrimary)',
            boxShadow: 'var(--Lumina-Tokens-shadow1)',
            transition: `left 120ms ${EASE}`,
          }}
        />
      </button>
    </div>
  );
}

// ---------- 复选框（macOS 式：16px / 5px 圆角，选中 accent 填充 + 白勾） ----------

export function ParamCheckbox({ label, checked, onChange }: { label: string; checked: boolean; onChange: (v: boolean) => void }) {
  return (
    <div className="h-6 mx-1 px-2 rounded-mdC hover:bg-hover transition-colors flex items-center gap-2" style={{ transitionTimingFunction: EASE }}>
      <button
        onClick={() => onChange(!checked)}
        className="shrink-0 flex items-center justify-center transition-colors"
        style={{
          width: 16,
          height: 16,
          borderRadius: 5,
          background: checked ? 'var(--Lumina-Tokens-accent)' : 'transparent',
          border: checked ? '1px solid var(--Lumina-Tokens-accent)' : '1px solid var(--Lumina-Tokens-border)',
          transitionTimingFunction: EASE,
        }}
        role="checkbox"
        aria-checked={checked}
        aria-label={label}
      >
        {checked && (
          <svg width="10" height="10" viewBox="0 0 24 24" aria-hidden>
            <path d="M5 12.5l4.5 4.5L19 7.5" fill="none" stroke="var(--Lumina-Tokens-onAccent)" strokeWidth="3" strokeLinecap="round" strokeLinejoin="round" />
          </svg>
        )}
      </button>
      <span className="text-[11.5px] text-txt2 flex-1 truncate">{label}</span>
    </div>
  );
}

// ---------- 分段选择（圆角与选中态按 v1.2 规格） ----------

export function ParamSegmented<T extends string>({
  value,
  options,
  onChange,
}: {
  value: T;
  options: { value: T; label: string }[];
  onChange: (v: T) => void;
}) {
  return (
    <div className="inline-flex items-center gap-0.5 p-0.5 rounded-mdC border border-line" style={{ background: 'var(--Lumina-Tokens-card)' }}>
      {options.map((o) => {
        const active = o.value === value;
        return (
          <button
            key={o.value}
            onClick={() => onChange(o.value)}
            className="h-[22px] px-2 rounded-smC text-2xs transition-colors"
            style={{
              background: active ? 'var(--Lumina-Tokens-accentSoft)' : 'transparent',
              color: active ? 'var(--Lumina-Tokens-accent)' : 'var(--Lumina-Tokens-textSecondary)',
              boxShadow: active ? 'inset 0 0 0 1px color-mix(in srgb, var(--Lumina-Tokens-accent) 40%, transparent)' : 'none',
              transitionTimingFunction: EASE,
            }}
          >
            {o.label}
          </button>
        );
      })}
    </div>
  );
}
