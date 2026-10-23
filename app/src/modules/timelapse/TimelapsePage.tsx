/**
 * 延时工作台主页（对照 08 号设计稿）：左序列/关键帧 + 中帧播放器/曲线 +
 * 右渲染输出。画面区恒 #08090B。
 */

import { useEffect } from 'react';
import { useTimelapse } from './store/timelapseStore';
import { Button } from '../../components/primitives';
import { EmptyState } from '../../components/shell';
import SequenceList from './SequenceList';
import FramePlayer from './FramePlayer';
import RenderPanel from './RenderPanel';

export default function TimelapsePage() {
  const loadError = useTimelapse((s) => s.loadError);
  const sequences = useTimelapse((s) => s.sequences);

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
    <div className="h-full flex">
      <SequenceList />
      <FramePlayer />
      <RenderPanel />
    </div>
  );
}
