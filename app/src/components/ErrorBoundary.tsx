/**
 * 渲染错误边界（WP-B3 零静默红线）。
 *
 * 背景：2026-10-22 修图台出现"整窗全黑且无任何提示"——根因是一条
 * `[...undefined]` 抛错打穿了整棵 React 树，React 18 卸载全部节点后窗口空白。
 * 有了这个边界，任何页面级渲染异常都会被兜成可见卡片 + 恢复入口，顶栏与导航继续可用。
 */

import { Component, type ErrorInfo, type ReactNode } from 'react';

interface Props {
  children: ReactNode;
  /** 出错时的定位标签（例如当前视图名），只用于展示 */
  scope?: string;
  /** 恢复回调（默认整页重载） */
  onReset?: () => void;
}

interface State {
  error: Error | null;
  info: string;
  count: number;
}

export default class ErrorBoundary extends Component<Props, State> {
  state: State = { error: null, info: '', count: 0 };

  static getDerivedStateFromError(error: Error): Partial<State> {
    return { error };
  }

  componentDidCatch(error: Error, info: ErrorInfo): void {
    // 主进程 stdout 留痕（冒烟/日志可用 [renderer][error] 检索）
    console.error(`[error-boundary] ${this.props.scope ?? 'app'}: ${error.message}`, info.componentStack ?? '');
    this.setState({ info: info.componentStack ?? '' });
  }

  private reset = (): void => {
    this.setState((s) => ({ error: null, info: '', count: s.count + 1 }));
    this.props.onReset?.();
  };

  render(): ReactNode {
    const { error, info, count } = this.state;
    if (!error) {
      // key 变化强制重建子树，避免残留坏状态
      return <div key={count} className="h-full">{this.props.children}</div>;
    }
    const scope = this.props.scope ?? '界面';
    return (
      <div className="h-full flex flex-col items-center justify-center gap-3 px-8" role="alert">
        <div className="text-sm text-txt">{scope} 渲染出错，已拦截（应用未退出）</div>
        <div className="text-2xs text-danger max-w-xl text-center leading-5 break-all">{error.message}</div>
        {info && (
          <pre className="text-2xs text-txt3 max-w-2xl max-h-40 overflow-auto whitespace-pre-wrap leading-4 px-3 py-2 rounded-md border border-line" style={{ background: 'var(--Lumina-Tokens-card)' }}>
            {info.trim()}
          </pre>
        )}
        <div className="flex items-center gap-2">
          <button className="h-7 px-3 rounded-md border border-line text-xs text-txt2 hover:text-txt hover:bg-hover" onClick={this.reset}>
            重试该页面
          </button>
          <button className="h-7 px-3 rounded-md text-xs text-txt3 hover:text-txt" onClick={() => window.location.reload()}>
            重新加载应用
          </button>
        </div>
        <div className="text-2xs text-txt3">已记录到日志。若反复出现，请把上面的错误原文反馈给开发者。</div>
      </div>
    );
  }
}
