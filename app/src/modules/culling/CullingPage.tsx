/**
 * 精选模式主页（阶段5 B1 重排）：默认 = 网格批量形态（网格 + 右栏审阅 + 计数条）；
 * Tab 切换到单张审阅子模式（原大图流水线：工具条 + 候选条 + 大观察区 + 胶片条）。
 * 键盘流：J/K 翻张、P 晋级、X 拒绝、U 撤销标记、1-5 星、←→/Z/Space（审阅模式）、Esc 清除。
 */

import { useEffect, useState } from 'react';
import { useCulling } from './store/cullingStore';
import { Button, Spinner } from '../../components/primitives';
import { EmptyState, KeycapBar } from '../../components/shell';
import CullingToolbar from './CullingToolbar';
import CullingViewer from './CullingViewer';
import CullingFilmstrip from './CullingFilmstrip';
import CullingGrid from './CullingGrid';
import CullingBatch, { CullingStatusBar } from './CullingBatch';

export default function CullingPage() {
  const loading = useCulling((s) => s.loading);
  const loadError = useCulling((s) => s.loadError);
  const total = useCulling((s) => s.total);
  const [gridCollapsed, setGridCollapsed] = useState(false);
  /** B1：grid = 网格批量（默认）；review = 单张审阅子模式（原流水线） */
  const [mode, setMode] = useState<'grid' | 'review'>('grid');

  // 进入页面即加载候选队列（store 内幂等）
  useEffect(() => {
    void useCulling.getState().load();
  }, []);

  // 键盘流（本模块核心卖点）
  useEffect(() => {
    const onKey = (e: KeyboardEvent) => {
      const tag = (e.target as HTMLElement)?.tagName;
      if (tag === 'INPUT' || tag === 'TEXTAREA' || (e.target as HTMLElement)?.isContentEditable) return;
      const st = useCulling.getState();
      const key = e.key.toLowerCase();
      if (e.key === 'Tab') {
        e.preventDefault();
        setMode((m) => (m === 'grid' ? 'review' : 'grid'));
        return;
      }
      if (e.key >= '1' && e.key <= '5') {
        e.preventDefault();
        st.rate(Number(e.key) as 1 | 2 | 3 | 4 | 5);
      } else if (e.key === '0') {
        e.preventDefault();
        st.rate(0);
      } else if (key === 'p') {
        e.preventDefault();
        st.togglePick();
      } else if (key === 'x') {
        e.preventDefault();
        st.toggleReject();
      } else if (key === 'j') {
        // B1：网格批量翻张（下一张）
        e.preventDefault();
        st.step(1);
      } else if (key === 'k') {
        // B1：网格批量翻张（上一张）
        e.preventDefault();
        st.step(-1);
      } else if (key === 'u') {
        // B1：撤销当前张标记（评分与旗标一并还原为未判）
        e.preventDefault();
        st.clearDecision();
      } else if (e.key === 'ArrowRight') {
        e.preventDefault();
        st.step(1);
      } else if (e.key === 'ArrowLeft') {
        e.preventDefault();
        st.step(-1);
      } else if (key === 'z') {
        e.preventDefault();
        st.toggleZoom();
      } else if (e.key === ' ') {
        e.preventDefault();
        st.toggleCompare();
      } else if (e.key === 'Escape') {
        st.clearDecision();
      }
    };
    window.addEventListener('keydown', onKey);
    return () => window.removeEventListener('keydown', onKey);
  }, []);

  if (loading) {
    return (
      <div className="h-full flex items-center justify-center">
        <Spinner size={22} />
      </div>
    );
  }

  if (loadError) {
    return (
      <div className="h-full flex items-center justify-center">
        <EmptyState
          icon={<span className="text-danger">⚠</span>}
          title="候选队列加载失败"
          desc={loadError}
          action={
            <Button onClick={() => void useCulling.getState().reload()}>重试</Button>
          }
        />
      </div>
    );
  }

  if (total === 0) {
    return (
      <div className="h-full flex items-center justify-center">
        <EmptyState title="图库为空" desc="先导入照片，再进入精选模式开始决策。" />
      </div>
    );
  }

  if (mode === 'grid') {
    return (
      <div className="h-full flex flex-col">
        <CullingToolbar />
        <CullingBatch />
        <CullingStatusBar />
        <KeycapBar
          items={[
            { keys: ['J', 'K'], label: '翻张' },
            { keys: ['P'], label: '晋级' },
            { keys: ['X'], label: '拒绝' },
            { keys: ['U'], label: '撤销标记' },
            { keys: ['1', '–', '5'], label: '打星' },
            { keys: ['Esc'], label: '清除决策' },
            { keys: ['Tab'], label: '单张审阅' },
          ]}
        />
      </div>
    );
  }

  return (
    <div className="h-full flex flex-col">
      <CullingToolbar />
      <div className="flex-1 min-h-0 flex">
        <CullingGrid collapsed={gridCollapsed} onToggle={() => setGridCollapsed((c) => !c)} />
        <div className="flex-1 min-w-0 flex flex-col">
          <CullingViewer />
          <CullingFilmstrip />
        </div>
      </div>
      <KeycapBar
        items={[
          { keys: ['1', '–', '5'], label: '打星' },
          { keys: ['P'], label: '精选' },
          { keys: ['X'], label: '淘汰' },
          { keys: ['←', '→'], label: '切换' },
          { keys: ['Z'], label: '1:1 放大' },
          { keys: ['Space'], label: '对比上一张' },
          { keys: ['Esc'], label: '清除决策' },
          { keys: ['0'], label: '取消评分' },
          { keys: ['Tab'], label: '网格批量' },
        ]}
      />
    </div>
  );
}

