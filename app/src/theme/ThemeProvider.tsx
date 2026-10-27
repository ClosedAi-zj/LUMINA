/**
 * 主题 Provider：深浅双主题（根元素 data-theme），设置经 system.setSettings 持久化。
 * 观察区组件一律用 <ViewerSurface>（恒 --Lumina-Tokens-viewer，红线 R5）。
 */

import { createContext, useCallback, useContext, useEffect, useState, type ReactNode } from 'react';
import { api } from '../api/engine';

export type ThemeName = 'Dark' | 'Light';

interface ThemeCtx {
  theme: ThemeName;
  setTheme: (t: ThemeName) => void;
  toggle: () => void;
}

const Ctx = createContext<ThemeCtx>({ theme: 'Dark', setTheme: () => undefined, toggle: () => undefined });

const STORAGE_KEY = 'lumina.theme';

export function ThemeProvider({ children }: { children: ReactNode }) {
  const [theme, setThemeState] = useState<ThemeName>(() => {
    const saved = localStorage.getItem(STORAGE_KEY);
    return saved === 'Light' ? 'Light' : 'Dark';
  });

  useEffect(() => {
    document.documentElement.setAttribute('data-theme', theme);
  }, [theme]);

  const setTheme = useCallback((t: ThemeName) => {
    setThemeState(t);
    localStorage.setItem(STORAGE_KEY, t);
    // 设置持久化到引擎 settings.json（失败不影响 UI）
    api.system.setSettings({ theme: t === 'Light' ? 'light' : 'dark' }).catch(() => undefined);
  }, []);

  const toggle = useCallback(() => {
    setTheme(theme === 'Dark' ? 'Light' : 'Dark');
  }, [theme, setTheme]);

  // MCP 事件桥：外部（渲染层 MCP 执行器）无法访问 React 上下文，
  // 统一经 lumina:theme-set 事件走同一个 setTheme，避免 DOM attribute 与上下文状态分叉。
  useEffect(() => {
    const onSet = (e: Event) => {
      const t = (e as CustomEvent<{ theme?: ThemeName }>).detail?.theme;
      if (t === 'Dark' || t === 'Light') setTheme(t);
    };
    window.addEventListener('lumina:theme-set', onSet);
    return () => window.removeEventListener('lumina:theme-set', onSet);
  }, [setTheme]);

  return <Ctx.Provider value={{ theme, setTheme, toggle }}>{children}</Ctx.Provider>;
}

export function useTheme(): ThemeCtx {
  return useContext(Ctx);
}

/** 照片观察区恒色封装（R5：查看器/地图/延时画面恒 #08090B，不随主题翻转） */
export function ViewerSurface({ children, className = '', style }: { children?: ReactNode; className?: string; style?: React.CSSProperties }) {
  return (
    <div
      className={className}
      style={{ background: 'var(--Lumina-Tokens-viewer)', ...style }}
      data-viewer-surface="true"
    >
      {children}
    </div>
  );
}
