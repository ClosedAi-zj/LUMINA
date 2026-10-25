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
        // v1.2.0：顶栏玻璃底（模糊在组件侧 backdrop-blur 提供）
        glass: 'var(--Lumina-Tokens-glass)',
      },
      borderRadius: {
        sm: 'var(--Lumina-Tokens-radiusSm)',
        md: 'var(--Lumina-Tokens-radiusMd)',
        lg: 'var(--Lumina-Tokens-radiusLg)',
        xl: 'var(--Lumina-Tokens-radiusXl)',
        // v1.2.0 控件级圆角（与上面既有四档并存，既有类名不受影响）
        xs: 'var(--Lumina-Tokens-radiusXs)',
        smC: 'var(--Lumina-Tokens-radiusSmC)',
        mdC: 'var(--Lumina-Tokens-radiusMdC)',
        lgC: 'var(--Lumina-Tokens-radiusLgC)',
        xlC: 'var(--Lumina-Tokens-radiusXlC)',
        pill: 'var(--Lumina-Tokens-radiusPill)',
      },
      boxShadow: {
        sh1: 'var(--Lumina-Tokens-shadow1)',
        sh2: 'var(--Lumina-Tokens-shadow2)',
      },
      fontFamily: {
        sans: ['var(--Lumina-Tokens-fontSans)', 'var(--Lumina-Tokens-fontCjk)', 'system-ui', 'sans-serif'],
        mono: ['var(--Lumina-Tokens-fontMono)', 'ui-monospace', 'monospace'],
      },
      fontSize: {
        '2xs': ['0.6875rem', { lineHeight: '1rem' }],
      },
      // v1.2.0 R7 §3-6 动效：浮层出现 = 200ms ease-out 淡入 + 4px 上移；
      // 面板展开 = 240ms cubic-bezier(0.2,0.8,0.2,1)。只用于浮层/面板展开，不加到照片/网格。
      keyframes: {
        'float-in': {
          '0%': { opacity: '0', transform: 'translateY(4px)' },
          '100%': { opacity: '1', transform: 'translateY(0)' },
        },
        'panel-in': {
          '0%': { opacity: '0', transform: 'translateY(-2px)' },
          '100%': { opacity: '1', transform: 'translateY(0)' },
        },
      },
      animation: {
        'float-in': 'float-in 200ms ease-out both',
        'panel-in': 'panel-in 240ms cubic-bezier(0.2, 0.8, 0.2, 1) both',
      },
    },
  },
  plugins: [],
} satisfies Config;
