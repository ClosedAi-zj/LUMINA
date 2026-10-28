/**
 * 右侧导出任务队列：进度条 + 状态徽标（排队/进行中/完成/失败/已取消）
 * + 取消 / 重试操作 + 接口降级横幅（零静默红线）。
 */

import { useEffect, useMemo, useRef } from 'react';
import { useExport } from './store/exportStore';
import { useJobStore } from '../../store/jobStore';
import { Button } from '../../components/primitives';
import { formatEta } from '../../utils/format';

const STATE_LABEL: Record<string, string> = {
  queued: '排队',
  running: '进行中',
  done: '完成',
  failed: '失败',
  cancelled: '已取消',
};

function stateColor(state: string): string {
  switch (state) {
    case 'running':
      return 'var(--Lumina-Tokens-accent)';
    case 'done':
      return 'var(--Lumina-Tokens-success)';
    case 'failed':
      return 'var(--Lumina-Tokens-danger)';
    default:
      return 'var(--Lumina-Tokens-textMuted)';
  }
}

export default function ExportQueue() {
  const jobs = useExport((s) => s.jobs);
  const jobsError = useExport((s) => s.jobsError);
  // 事件泵的实时进度（jobStore 由 event.job.progress/completed 驱动）。
  // 旧实现只在 refreshJobs 时合并一次 → 队列进度条几乎不动（要手点「刷新」才跳一下）。
  const liveJobs = useJobStore((s) => s.jobs);

  // 只在两个源真正变化时重建行数据（不做每帧全量重建）
  const rows = useMemo(
    () =>
      jobs.map((j) => {
        const live = liveJobs[j.jobId];
        if (live && live.state === 'running' && j.state === 'running') {
          return { ...j, progress: live.progress, detail: live.detail || j.detail, etaMs: live.etaMs };
        }
        return j;
      }),
    [jobs, liveJobs],
  );

  // 事件推进到「完成/失败/取消」时，拉一次队列让状态徽标与最终进度落定
  //（事件只带进度与终态，产出物字段仍在引擎队列里；这不是轮询，只在终态变化时触发一次）
  const settledKey = useMemo(
    () =>
      rows
        .map((j) => `${j.jobId}:${j.state}`)
        .join(','),
    [rows],
  );
  const prevSettledRef = useRef<string>('');
  useEffect(() => {
    const prev = prevSettledRef.current;
    prevSettledRef.current = settledKey;
    if (prev === '' || prev === settledKey) return;
    void useExport.getState().refreshJobs();
  }, [settledKey]);

  const activeCount = rows.filter((j) => j.state === 'running' || j.state === 'queued').length;

  return (
    <aside className="shrink-0 w-72 border-l border-line flex flex-col overflow-hidden" style={{ background: 'var(--Lumina-Tokens-panel)' }}>
      <div className="flex items-center h-9 px-3 border-b border-line shrink-0">
        <span className="text-2xs text-txt2 font-medium">任务队列</span>
        {activeCount > 0 && <span className="num text-2xs text-accent ml-1.5">{activeCount} 个进行中</span>}
        <div className="flex-1" />
        <button className="text-2xs text-txt3 hover:text-txt" onClick={() => void useExport.getState().refreshJobs()}>
          刷新
        </button>
      </div>

      {jobsError && (
        <div className="mx-2 mt-2 rounded-md border p-2.5 text-2xs leading-5 shrink-0" style={{ borderColor: 'var(--Lumina-Tokens-danger)' }}>
          <span className="font-medium" style={{ color: 'var(--Lumina-Tokens-danger)' }}>
            导出接口不可用 · 功能已降级
          </span>
          <div className="text-txt3 mt-0.5">{jobsError}</div>
          <button className="mt-1 text-accent hover:opacity-80" onClick={() => void useExport.getState().refreshJobs()}>
            重试
          </button>
        </div>
      )}

      <div className="flex-1 min-h-0 overflow-y-auto p-2 flex flex-col gap-1.5">
        {!jobsError && rows.length === 0 && <div className="py-8 text-center text-2xs text-txt3">暂无导出任务</div>}
        {rows.map((j) => (
          <div key={j.jobId} className="rounded-md border border-line p-2.5" style={{ background: 'var(--Lumina-Tokens-card)' }}>
            <div className="flex items-center gap-2">
              <span className="w-1.5 h-1.5 rounded-full shrink-0" style={{ background: stateColor(j.state) }} />
              <span className="num text-2xs text-txt2 truncate flex-1">
                任务 {j.jobId} · 导出渲染
              </span>
              <span className="text-2xs shrink-0" style={{ color: stateColor(j.state) }}>
                {STATE_LABEL[j.state] ?? j.state}
              </span>
            </div>
            {(j.state === 'running' || j.state === 'queued') && (
              <div className="mt-1.5 h-1 rounded-full overflow-hidden" style={{ background: 'var(--Lumina-Tokens-hover)' }}>
                <div className="h-full rounded-full bg-accent transition-all" style={{ width: `${Math.max(2, j.progress)}%` }} />
              </div>
            )}
            <div className="flex items-center gap-2 mt-1.5">
              <span className="num text-2xs text-txt3 truncate flex-1">{j.detail || `进度 ${Math.round(j.progress)}%`}</span>
              {j.state === 'running' && j.etaMs !== null && <span className="num text-2xs text-txt3 shrink-0">剩余 {formatEta(j.etaMs)}</span>}
              {(j.state === 'running' || j.state === 'queued') && (
                <Button size="sm" onClick={() => void useExport.getState().cancelJob(j.jobId)}>
                  取消
                </Button>
              )}
              {j.state === 'failed' && (
                <Button size="sm" variant="primary" onClick={() => void useExport.getState().retryJob(j.jobId)}>
                  重试
                </Button>
              )}
            </div>
          </div>
        ))}
      </div>
    </aside>
  );
}
