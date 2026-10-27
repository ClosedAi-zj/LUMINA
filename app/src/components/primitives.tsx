/** 基础 UI 组件集（全部消费设计令牌，禁止硬编码色值） */

import { useState, type ButtonHTMLAttributes, type ReactNode } from 'react';

// ---------- Button ----------

export function Button({
  children,
  variant = 'default',
  size = 'md',
  className = '',
  ...rest
}: ButtonHTMLAttributes<HTMLButtonElement> & { variant?: 'default' | 'primary' | 'danger' | 'ghost'; size?: 'sm' | 'md' | 'lg' }) {
  const base =
    'inline-flex items-center justify-center gap-1.5 rounded-md border transition-colors duration-100 disabled:opacity-40 disabled:cursor-not-allowed select-none';
  const sizeCls = size === 'sm' ? 'h-6 px-2 text-2xs' : size === 'lg' ? 'h-9 px-4 text-sm' : 'h-7 px-3 text-xs';
  let variantCls = '';
  if (variant === 'primary') {
    variantCls = 'border-transparent bg-accent text-onaccent hover:opacity-90 font-medium';
  } else if (variant === 'danger') {
    variantCls = 'border-transparent bg-danger/90 text-white hover:opacity-90';
  } else if (variant === 'ghost') {
    variantCls = 'border-transparent bg-transparent text-txt2 hover:text-txt hover:bg-hover';
  } else {
    variantCls = 'border-line bg-card text-txt2 hover:text-txt hover:bg-hover';
  }
  return (
    <button className={`${base} ${sizeCls} ${variantCls} ${className}`} {...rest}>
      {children}
    </button>
  );
}

// ---------- Chip ----------

export function Chip({ label, active, onClick, onRemove, count }: { label: ReactNode; active?: boolean; onClick?: () => void; onRemove?: () => void; count?: number }) {
  return (
    <span
      onClick={onClick}
      className={`inline-flex items-center gap-1 h-6 px-2 rounded-full border text-2xs cursor-pointer transition-colors ${
        active ? 'border-accent bg-[var(--Lumina-Tokens-accentSoft)] text-accent' : 'border-line bg-card text-txt2 hover:text-txt hover:bg-hover'
      }`}
    >
      {label}
      {count !== undefined && <span className="num opacity-60">{count}</span>}
      {onRemove && (
        <span
          role="button"
          aria-label={`移除筛选 ${typeof label === 'string' ? label : ''}`}
          onClick={(e) => {
            e.stopPropagation();
            onRemove();
          }}
          className="ml-0.5 opacity-60 hover:opacity-100"
        >
          ×
        </span>
      )}
    </span>
  );
}

// ---------- Slider ----------

export function Slider({
  label,
  value,
  min,
  max,
  step = 1,
  defaultValue,
  onChange,
  onCommit,
  format,
}: {
  label: string;
  value: number;
  min: number;
  max: number;
  step?: number;
  defaultValue?: number;
  onChange: (v: number) => void;
  onCommit?: (v: number) => void;
  format?: (v: number) => string;
}) {
  const changed = defaultValue !== undefined && Math.abs(value - defaultValue) > 1e-9;
  const pct = max === min ? 0 : ((value - min) / (max - min)) * 100;
  return (
    <div className="mb-2.5">
      <div className="flex items-center justify-between mb-1">
        <span className="text-2xs text-txt2">{label}</span>
        <div className="flex items-center gap-1">
          {changed && defaultValue !== undefined && (
            <button
              className="text-2xs w-4 h-4 rounded-sm bg-hover text-txt3 hover:text-txt"
              title="双击滑块名重置"
              onClick={() => {
                onChange(defaultValue);
                onCommit?.(defaultValue);
              }}
            >
              ↺
            </button>
          )}
          <span className={`text-2xs num ${changed ? 'text-accent' : 'text-txt2'}`}>{format ? format(value) : value > 0 ? `+${value}` : value}</span>
        </div>
      </div>
      {/* v1.4.1：自绘轨道 + 白球，原生 input 仅作透明事件层。
          此前把渐变背景写进 input 再叠 accent-color，Chromium 会额外绘制原生进度色块，
          于是同一条轨道出现「厚蓝条 + 细灰条」两种厚度；改自绘后与 ParamSlider 完全同款。
          透明 input 仍保留键盘方向键与无障碍语义（不牺牲可达性）。 */}
      <div className="relative h-4">
        <div
          className="absolute left-0 right-0 top-1/2 -translate-y-1/2 h-1 rounded-pill"
          style={{ background: 'var(--Lumina-Tokens-hover)' }}
        >
          <div
            className="h-1 rounded-pill"
            style={{ width: `${pct}%`, background: 'var(--Lumina-Tokens-accent)' }}
          />
        </div>
        <div
          className="absolute rounded-full pointer-events-none"
          style={{
            width: 13,
            height: 13,
            left: `${pct}%`,
            top: '50%',
            transform: 'translate(-50%, -50%)',
            background: '#ffffff',
            border: '0.5px solid rgba(0, 0, 0, 0.18)',
            boxShadow: '0 1px 3px rgba(0, 0, 0, 0.45)',
          }}
        />
        <AppearanceNoneRange
          min={min}
          max={max}
          step={step}
          value={value}
          onChange={onChange}
          onCommit={() => onCommit?.(value)}
          onReset={() => {
            if (defaultValue !== undefined) {
              onChange(defaultValue);
              onCommit?.(defaultValue);
            }
          }}
        />
      </div>
    </div>
  );
}

/** 透明原生 range：只负责命中/键盘/无障碍，视觉全部由父级自绘 */
function AppearanceNoneRange({
  min,
  max,
  step,
  value,
  onChange,
  onCommit,
  onReset,
}: {
  min: number;
  max: number;
  step: number;
  value: number;
  onChange: (v: number) => void;
  onCommit: () => void;
  onReset: () => void;
}) {
  const [hover, setHover] = useState(false);
  return (
    <>
      {/* hover 时才出现的放大球（原生 thumb 不可见，这里补一层视觉反馈） */}
      {hover && (
        <div
          className="absolute rounded-full pointer-events-none"
          style={{
            width: 16,
            height: 16,
            left: `${max === min ? 0 : ((value - min) / (max - min)) * 100}%`,
            top: '50%',
            transform: 'translate(-50%, -50%)',
            background: '#ffffff',
            border: '0.5px solid rgba(0, 0, 0, 0.18)',
            boxShadow: '0 1px 4px rgba(0, 0, 0, 0.5)',
          }}
        />
      )}
      <input
        type="range"
        min={min}
        max={max}
        step={step}
        value={value}
        onChange={(e) => onChange(Number(e.target.value))}
        onPointerUp={onCommit}
        onKeyUp={onCommit}
        onDoubleClick={onReset}
        onMouseEnter={() => setHover(true)}
        onMouseLeave={() => setHover(false)}
        aria-label="滑块"
        className="absolute inset-0 w-full h-full opacity-0 cursor-ew-resize"
      />
    </>
  );
}

// ---------- Toggle ----------

export function Toggle({ checked, onChange, label }: { checked: boolean; onChange: (v: boolean) => void; label?: string }) {
  return (
    <button
      onClick={() => onChange(!checked)}
      className="inline-flex items-center gap-2 shrink-0 whitespace-nowrap"
      role="switch"
      aria-checked={checked}
      aria-label={label ?? '开关'}
    >
      <span
        className={`relative inline-block w-8 h-[18px] rounded-full transition-colors ${checked ? 'bg-accent' : 'bg-hover border border-line'}`}
      >
        <span
          className={`absolute top-[2px] w-[14px] h-[14px] rounded-full bg-white transition-transform ${checked ? 'translate-x-[16px]' : 'translate-x-[2px]'}`}
          style={{ background: checked ? 'var(--Lumina-Tokens-onAccent)' : 'var(--Lumina-Tokens-textSecondary)' }}
        />
      </span>
      {label && <span className="text-2xs text-txt2 whitespace-nowrap">{label}</span>}
    </button>
  );
}

// ---------- Segmented ----------

export function Segmented<T extends string>({ options, value, onChange }: { options: { value: T; label: ReactNode }[]; value: T; onChange: (v: T) => void }) {
  return (
    <div className="inline-flex rounded-md border border-line overflow-hidden">
      {options.map((o) => (
        <button
          key={o.value}
          onClick={() => onChange(o.value)}
          className={`px-3 h-7 text-xs transition-colors ${value === o.value ? 'bg-accent text-onaccent font-medium' : 'bg-card text-txt2 hover:text-txt hover:bg-hover'}`}
        >
          {o.label}
        </button>
      ))}
    </div>
  );
}

// ---------- Spinner ----------

export function Spinner({ size = 16 }: { size?: number }) {
  return (
    <svg width={size} height={size} viewBox="0 0 24 24" className="animate-spin">
      <circle cx="12" cy="12" r="9" fill="none" stroke="var(--Lumina-Tokens-border)" strokeWidth="3" />
      <path d="M12 3a9 9 0 0 1 9 9" fill="none" stroke="var(--Lumina-Tokens-accent)" strokeWidth="3" strokeLinecap="round" />
    </svg>
  );
}
