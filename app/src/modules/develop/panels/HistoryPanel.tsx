/**
 * 历史面板：undo/redo + 步骤列表（UI 侧标签）+ 快照/虚拟副本（协议 §5.5，R2 三层非破坏）。
 */

import { useEffect, useState } from 'react';
import { useDevelop, normalizeParams } from '../../../store/developStore';
import { api } from '../../../api/engine';
import { Button } from '../../../components/primitives';
import { InlineError } from '../../../components/states';
import { errText } from '../../../api/rpc-client';
import type { SnapshotInfo, VCopyInfo } from '../../../api/protocol';

export default function HistoryPanel() {
  const historyLabels = useDevelop((s) => s.historyLabels);
  const historySnapshots = useDevelop((s) => s.historySnapshots);
  const canUndo = useDevelop((s) => s.canUndo);
  const canRedo = useDevelop((s) => s.canRedo);
  const undo = useDevelop((s) => s.undo);
  const redo = useDevelop((s) => s.redo);
  const photoId = useDevelop((s) => s.photoId);
  const [snapshots, setSnapshots] = useState<SnapshotInfo[]>([]);
  const [vcopies, setVcopies] = useState<VCopyInfo[]>([]);
  const [loadError, setLoadError] = useState<string | null>(null);

  useEffect(() => {
    if (photoId === null) return;
    // 零静默：快照/副本列表拿不到要在面板里显示原因（1.0 曾静默吞掉）
    Promise.all([api.edit.getSnapshots(photoId), api.vcopy.list(photoId)])
      .then(([snaps, vcs]) => {
        setSnapshots(snaps);
        setVcopies(vcs);
        setLoadError(null);
      })
      .catch((err) => setLoadError(errText(err)));
  }, [photoId, loadError]);

  return (
    <div className="px-2 pb-1 text-xs">
      <div className="flex items-center gap-1.5 mb-1.5">
        <span className="flex-1" />
        <Button size="sm" disabled={!canUndo} onClick={() => void undo()} title="Ctrl+Z">
          ↶ 撤销
        </Button>
        <Button size="sm" disabled={!canRedo} onClick={() => void redo()} title="Ctrl+Shift+Z">
          ↷ 重做
        </Button>
      </div>
      {/* 步骤 2/2 · 时间 形态：当前步高亮，任意步骤可点击回放参数快照 */}
      {loadError && (
        <div className="mb-2">
          <InlineError text="快照 / 虚拟副本列表加载失败" error={loadError} onRetry={() => setLoadError(null)} />
        </div>
      )}
      <div className="max-h-52 overflow-y-auto flex flex-col gap-px mb-3">
        {historyLabels.length === 0 && <div className="text-2xs text-txt3 py-1">暂无步骤</div>}
        {[...historyLabels]
          .map((label, i) => ({ label, index: i }))
          .reverse()
          .map(({ label, index }) => {
            const isCurrent = index === historyLabels.length - 1;
            const snap = historySnapshots[index];
            const timeText = snap ? `${new Date(snap.time).getHours()}:${String(new Date(snap.time).getMinutes()).padStart(2, '0')}` : null;
            return (
              <button
                key={index}
                onClick={() => {
                  if (!isCurrent) void useDevelop.getState().jumpHistory(index);
                }}
                disabled={isCurrent}
                title={isCurrent ? '当前步骤' : '点击回放该步骤的参数快照'}
                className={`h-6 px-2 rounded-sm flex items-center gap-1.5 text-2xs text-left transition-colors ${
                  isCurrent
                    ? 'bg-[var(--Lumina-Tokens-accentSoft)] text-accent'
                    : 'text-txt3 hover:bg-hover hover:text-txt2'
                }`}
              >
                <span className="num shrink-0">
                  {index + 1}/{historyLabels.length}
                </span>
                <span className="truncate flex-1">{label}</span>
                {timeText && <span className="num shrink-0 opacity-60">{timeText}</span>}
              </button>
            );
          })}
      </div>

      <div className="text-2xs text-txt3 mb-1.5">快照</div>
      <div className="flex flex-col gap-px mb-3">
        {snapshots.length === 0 && <div className="text-2xs text-txt3">暂无快照</div>}
        {groupSnapshots(snapshots).map((g) => (
          <button
            key={g.ids[0]}
            className="h-6 px-2 rounded-mdC text-left text-2xs text-txt2 hover:bg-hover hover:text-txt flex items-center gap-1.5"
            title={g.ids.length > 1 ? `${g.name}（${g.ids.length} 条同名快照，点击恢复最新一条）` : g.name}
            onClick={async () => {
              if (photoId === null) return;
              try {
                const res = await api.edit.restoreSnapshot(photoId, g.ids[0]);
                useDevelop.setState({
                  params: normalizeParams(res.params),
                  revision: useDevelop.getState().revision + 1,
                  historyLabels: [...useDevelop.getState().historyLabels, `快照恢复 · ${g.name}`],
                });
              } catch (err) {
                const { useJobStore } = await import('../../../store/jobStore');
                useJobStore.getState().toast(`快照恢复失败：${errText(err)}`, 'error', 6000);
              }
            }}
          >
            <span className="truncate">◉ {g.name}</span>
            {g.ids.length > 1 && <span className="num text-txt3 shrink-0">×{g.ids.length}</span>}
          </button>
        ))}
        <Button
          size="sm"
          className="mt-1"
          onClick={async () => {
            const name = `快照 ${snapshots.length + 1}`;
            await useDevelop.getState().saveSnapshot(name);
            if (photoId !== null) setSnapshots(await api.edit.getSnapshots(photoId));
          }}
        >
          ＋ 保存快照
        </Button>
      </div>

      <div className="text-2xs text-txt3 mb-1.5">虚拟副本</div>
      <div className="flex flex-col gap-px">
        {vcopies.length === 0 && <div className="text-2xs text-txt3">暂无副本</div>}
        {vcopies.map((v) => (
          <div key={v.vcopyId} className="h-6 px-2 rounded-sm flex items-center text-2xs text-txt2">
            ⧉ {v.name}
          </div>
        ))}
        <Button
          size="sm"
          className="mt-1"
          onClick={async () => {
            if (photoId === null) return;
            await api.vcopy.create(photoId);
            setVcopies(await api.vcopy.list(photoId));
          }}
        >
          ＋ 创建虚拟副本
        </Button>
      </div>
    </div>
  );
}

/** 同名连续快照合并（历次冒烟留下的 64 条「冒烟快照」应显示成一条 ×64，而不是刷满列表） */
function groupSnapshots(list: SnapshotInfo[]): { name: string; ids: number[]; latest: number }[] {
  const out: { name: string; ids: number[]; latest: number }[] = [];
  for (const s of list) {
    const last = out[out.length - 1];
    if (last && last.name === s.name) {
      last.ids.push(s.snapshotId);
      last.latest = Math.max(last.latest, s.createdAt);
    } else {
      out.push({ name: s.name, ids: [s.snapshotId], latest: s.createdAt });
    }
  }
  return out;
}
