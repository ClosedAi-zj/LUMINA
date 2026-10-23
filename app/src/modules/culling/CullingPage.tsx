/**
 * 精选模式主页：顶部决策工具条 + 左候选网格（可折叠）+ 中央大图观察区 +
 * 底部胶片条 + 键帽提示条。键盘流：1-5 星 / P 精选 / X 淘汰 / ←→ 切换 /
 * Z 放大 / Space 对比 / Esc 清除决策。
 */

import { useEffect, useState } from 'react';
import { useCulling } from './store/cullingStore';
import { Button, Spinner } from '../../components/primitives';
import { EmptyState, KeycapBar } from '../../components/shell';
import CullingToolbar from './CullingToolbar';
import CullingViewer from './CullingViewer';
import CullingFilmstrip from './CullingFilmstrip';
import CullingGrid from './CullingGrid';

export default function CullingPage() {
  const loading = useCulling((s) => s.loading);
  const loadError = useCulling((s) => s.loadError);
  const total = useCulling((s) => s.total);
  const [gridCollapsed, setGridCollapsed] = useState(false);

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
        ]}
      />
    </div>
  );
}

