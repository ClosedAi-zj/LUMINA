/**
 * 延时工作台主页（对照 LapseOne 参考重排）：
 * 顶部工作流 Tab（调色与修复 / 导出视频；序列管理 = 左栏常驻）+ 左序列栏 +
 * 中帧播放器或导出表格 + 右面板。画面区恒 #08090B。
 */

import { useEffect, useState } from 'react';
import { useTimelapse } from './store/timelapseStore';
import { Button } from '../../components/primitives';
import { EmptyState } from '../../components/shell';
import SequenceList from './SequenceList';
import FramePlayer from './FramePlayer';
import RenderPanel from './RenderPanel';
import ExportView from './ExportView';

type WorkTab = 'grade' | 'export';

const TABS: { key: WorkTab; label: string }[] = [
  { key: 'grade', label: '调色与修复' },
  { key: 'export', label: '导出视频' },
];

export default function TimelapsePage() {
  const loadError = useTimelapse((s) => s.loadError);
  const sequences = useTimelapse((s) => s.sequences);
  const [tab, setTab] = useState<WorkTab>('grade');

  useEffect(() => {
    void useTimelapse.getState().load();
  }, []);

  if (loadError && sequences.length === 0) {
    return (
      <div className="h-full flex items-center justify-center">
        <EmptyState
          icon={<span className="text-danger">⚠</span>}
          title="延时序列加载失败"
          desc={loadError}
          action={<Button onClick={() => void useTimelapse.getState().load()}>重试</Button>}
        />
      </div>
    );
  }

  return (
    <div className="h-full flex flex-col">
      {/* 顶部工作流 Tab（对照 LapseOne：蓝色高亮活动页） */}
      <div className="shrink-0 h-9 px-2 flex items-center gap-1 border-b border-line" style={{ background: 'var(--Lumina-Tokens-panel)' }}>
        {TABS.map((t) => {
          const active = tab === t.key;
          return (
            <button
              key={t.key}
              type="button"
              onClick={() => setTab(t.key)}
              aria-pressed={active}
              className="h-7 px-3 rounded-md text-2xs transition-colors"
              style={
                active
                  ? { background: 'var(--Lumina-Tokens-accent)', color: 'var(--Lumina-Tokens-onAccent)', fontWeight: 600 }
                  : { background: 'var(--Lumina-Tokens-card)', color: 'var(--Lumina-Tokens-textSecondary)' }
              }
            >
              {t.label}
            </button>
          );
        })}
      </div>

      <div className="flex-1 min-h-0 flex">
        <SequenceList />
        {tab === 'grade' ? (
          <>
            <FramePlayer />
            <RenderPanel />
          </>
        ) : (
          <ExportView />
        )}
      </div>
    </div>
  );
}
