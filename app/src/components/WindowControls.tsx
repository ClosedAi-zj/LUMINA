/**
 * 自定义标题栏窗口控制三键（最小化 / 最大化·还原 / 关闭）。
 *
 * PS/LR 式无边框窗口的 Windows 惯例：
 *  - 三键 46×40 满高热区，hover 中性色阶；
 *  - close hover 固定 #e81123 系（Windows 系统色），图标翻白，双主题一致；
 *  - 最大化态切换为「还原」双框图标（状态由主进程推送 + 初始查询兜底）。
 * 图标 10px 线性、strokeWidth 1（窗口控制属于 chrome 图形，不走内容区 1.5px 图标规范）。
 */

import { useEffect, useState } from 'react';

export function WindowControls() {
  const [maximized, setMaximized] = useState(false);

  useEffect(() => {
    let alive = true;
    // 初始状态兜底（推送事件只覆盖变化时刻）
    void window.lumina.win.isMaximized().then((v) => {
      if (alive) setMaximized(v);
    });
    const off = window.lumina.win.onMaximizeChange((v) => setMaximized(v));
    return () => {
      alive = false;
      off();
    };
  }, []);

  const btnBase =
    'h-full w-11 flex items-center justify-center text-txt2 transition-colors app-no-drag';

  return (
    <div className="h-full flex items-stretch shrink-0">
      <button
        className={`${btnBase} hover:bg-[rgba(255,255,255,0.09)]`}
        title="最小化"
        aria-label="最小化"
        onClick={() => void window.lumina.win.minimize()}
      >
        <svg width="10" height="10" viewBox="0 0 10 10" aria-hidden>
          <path d="M0 5h10" stroke="currentColor" strokeWidth="1" />
        </svg>
      </button>
      <button
        className={`${btnBase} hover:bg-[rgba(255,255,255,0.09)]`}
        title={maximized ? '向下还原' : '最大化'}
        aria-label={maximized ? '向下还原' : '最大化'}
        onClick={() => void window.lumina.win.toggleMaximize()}
      >
        {maximized ? (
          // 还原：前后两个错位方框
          <svg width="10" height="10" viewBox="0 0 10 10" aria-hidden>
            <path
              d="M2.5 2.5V0.5h7v7h-2M0.5 2.5h7v7h-7z"
              fill="none"
              stroke="currentColor"
              strokeWidth="1"
            />
          </svg>
        ) : (
          <svg width="10" height="10" viewBox="0 0 10 10" aria-hidden>
            <rect x="0.5" y="0.5" width="9" height="9" fill="none" stroke="currentColor" strokeWidth="1" />
          </svg>
        )}
      </button>
      <button
        className={`${btnBase} hover:bg-[#e81123] hover:text-white`}
        title="关闭"
        aria-label="关闭"
        onClick={() => void window.lumina.win.close()}
      >
        <svg width="10" height="10" viewBox="0 0 10 10" aria-hidden>
          <path d="M0 0l10 10M10 0L0 10" stroke="currentColor" strokeWidth="1" />
        </svg>
      </button>
    </div>
  );
}
