import type { Config } from 'tailwindcss';

/**
 * Tailwind 主题映射：所有颜色只允许引用 design-tokens.css 的 --Lumina-Tokens-* 变量，
 * 禁止在本文件或任何组件内硬编码色值（PRD 红线 R5 / §6.2）。
 */
export default {
  content: ['./index.html', './src/**/*.{ts,tsx}'],
  darkMode: ['class', '[data-theme="Dark"]'],
  theme: {
    extend: {
      colors: {
        canvas: 'var(--Lumina-Tokens-canvas)',
        panel: 'var(--Lumina-Tokens-panel)',
        card: 'var(--Lumina-Tokens-card)',
        hover: 'var(--Lumina-Tokens-hover)',
        viewer: 'var(--Lumina-Tokens-viewer)',
        browserbar: 'var(--Lumina-Tokens-browserBar)',
        line: 'var(--Lumina-Tokens-border)',
        txt: 'var(--Lumina-Tokens-textPrimary)',
        txt2: 'var(--Lumina-Tokens-textSecondary)',
        txt3: 'var(--Lumina-Tokens-textMuted)',
        accent: 'var(--Lumina-Tokens-accent)',
        onaccent: 'var(--Lumina-Tokens-onAccent)',
        success: 'var(--Lumina-Tokens-success)',
        danger: 'var(--Lumina-Tokens-danger)',
        reject: 'var(--Lumina-Tokens-rejectOverlay)',
      },
      borderRadius: {
        sm: 'var(--Lumina-Tokens-radiusSm)',
        md: 'var(--Lumina-Tokens-radiusMd)',
        lg: 'var(--Lumina-Tokens-radiusLg)',
        xl: 'var(--Lumina-Tokens-radiusXl)',
      },
      fontFamily: {
        sans: ['var(--Lumina-Tokens-fontSans)', 'var(--Lumina-Tokens-fontCjk)', 'system-ui', 'sans-serif'],
        mono: ['var(--Lumina-Tokens-fontMono)', 'ui-monospace', 'monospace'],
      },
      fontSize: {
        '2xs': ['0.6875rem', { lineHeight: '1rem' }],
      },
    },
  },
  plugins: [],
} satisfies Config;
