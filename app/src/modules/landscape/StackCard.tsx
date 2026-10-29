/**
 * 焦点堆栈（景深堆栈）合成卡（v1.3.0，对照 HDR 卡）：组检测列表 +
 * 自动对齐/DNG|TIFF 选项 + 锐度/融合滑块 + 一键合成（任务化，失败显式提示）+
 * 完成产物回执（result.photoId 已入图库，可一键进修图台）。
 */

import { useLandscape } from './store/landscapeStore';
import { useJobProgress } from '../../api/hooks';
import { Button, Segmented, Spinner, Toggle } from '../../components/primitives';

export default function StackCard() {
  const stackGroups = useLandscape((s) => s.stackGroups);
  const stackGroupsLoading = useLandscape((s) => s.stackGroupsLoading);
  const stackGroupsError = useLandscape((s) => s.stackGroupsError);
  const selectedStackGroupId = useLandscape((s) => s.selectedStackGroupId);
  const stackAutoAlign = useLandscape((s) => s.stackAutoAlign);
  const stackOutputFormat = useLandscape((s) => s.stackOutputFormat);
  const stackSmoothness = useLandscape((s) => s.stackSmoothness);
  const stackJobId = useLandscape((s) => s.stackJobId);
  const stackResultPhotoId = useLandscape((s) => s.stackResultPhotoId);
  const stackJob = useJobProgress(stackJobId);

  const totalPhotos = stackGroups.reduce((acc, g) => acc + g.photoIds.length, 0);
  const selected = stackGroups.find((g) => g.groupId === selectedStackGroupId) ?? null;

  return (
    <div className="rounded-lg border border-line p-3 flex flex-col gap-2.5" style={{ background: 'var(--Lumina-Tokens-panel)' }}>
      <div className="flex items-center gap-2">
        <span className="text-xs font-medium">焦点堆栈合成</span>
        <div className="flex-1" />
        <span className="text-2xs px-1.5 py-0.5 rounded-sm" style={{ background: 'var(--Lumina-Tokens-accentSoft)', color: 'var(--Lumina-Tokens-accent)' }}>v1.3.0</span>
      </div>
      <div className="text-2xs text-txt3 leading-4">自动对齐 · 焦点加权融合 · 全清晰<i className="not-italic text-txt3">（同机位不同对焦连拍 ≥2 张）</i></div>

      {stackGroupsLoading && (
        <div className="flex items-center justify-center py-4">
          <Spinner size={16} />
        </div>
      )}
      {!stackGroupsLoading && stackGroupsError && (
        <div className="text-2xs leading-5" style={{ color: 'var(--Lumina-Tokens-danger)' }}>
          堆栈组检测失败：{stackGroupsError}
          <button className="ml-1.5 text-accent hover:opacity-80" onClick={() => void useLandscape.getState().load()}>重试</button>
        </div>
      )}
      {!stackGroupsLoading && !stackGroupsError && stackGroups.length === 0 && (
        <div className="text-2xs text-txt3 py-2 leading-5">
          未识别到堆栈组
          <br />
          <span className="text-txt3">导入同机位不同对焦连拍（≥2 张）后自动识别；当前图库暂无符合条件的连拍</span>
        </div>
      )}

      {stackGroups.length > 0 && (
        <div className="flex items-center gap-2 flex-wrap">
          {stackGroups.slice(0, 8).map((g) => (
            <button
              key={g.groupId}
              onClick={() => useLandscape.getState().selectStackGroup(g.groupId)}
              className={`num text-2xs h-6 px-2 rounded-md border transition-colors ${
                g.groupId === selectedStackGroupId ? 'border-accent text-accent' : 'border-line text-txt3 hover:text-txt2 hover:bg-hover'
              }`}
              style={{ background: g.groupId === selectedStackGroupId ? 'var(--Lumina-Tokens-accentSoft)' : 'var(--Lumina-Tokens-card)' }}
              title={`组 ${g.groupId} · ${g.frameCount} 张 · ${g.photoIds.length} 帧`}
            >
              组 {g.groupId} · {g.frameCount} 张
            </button>
          ))}
          {stackGroups.length > 8 && <span className="num text-2xs text-txt3">+{stackGroups.length - 8}</span>}
        </div>
      )}

      {selected && (
        <>
          <div className="flex items-center gap-4">
            <Toggle checked={stackAutoAlign} onChange={(v) => useLandscape.getState().patchStackComposite({ autoAlign: v })} label="自动对齐" />
          </div>
          <div className="flex items-center gap-2">
            <span className="text-2xs text-txt2 shrink-0">输出</span>
            <Segmented
              value={stackOutputFormat}
              onChange={(v) => useLandscape.getState().patchStackComposite({ outputFormat: v })}
              options={[
                { value: 'dng', label: 'DNG' },
                { value: 'tiff', label: 'TIFF' },
              ]}
            />
          </div>
          <div className="flex items-center gap-2">
            <span className="text-2xs text-txt2 shrink-0">锐度/融合（1 柔 → 12 锐）</span>
            <input
              type="range"
              min={1}
              max={12}
              step={1}
              value={stackSmoothness}
              onChange={(e) => useLandscape.getState().patchStackComposite({ smoothness: Number(e.target.value) })}
              className="flex-1 accent-[var(--Lumina-Tokens-accent)]"
            />
            <span className="num text-2xs text-txt3 w-5 text-right">{stackSmoothness}</span>
          </div>
          <div className="flex items-center gap-2">
            <Button variant="primary" size="sm" disabled={stackJob?.state === 'running'} onClick={() => void useLandscape.getState().runStackComposite()}>
              {stackJob?.state === 'running' ? `合成中 ${Math.round(stackJob.progress)}%` : `合成组 ${selected.groupId}（${selected.frameCount} 张）`}
            </Button>
            <span className="num text-2xs text-txt3">共 {stackGroups.length} 组 · {totalPhotos} 张</span>
          </div>
        </>
      )}

      {/* 合成产物回执：引擎完成事件 result.photoId（已入图库索引） */}
      {stackResultPhotoId !== null && (
        <div
          className="rounded-md border p-2 flex items-center gap-2"
          style={{ background: 'var(--Lumina-Tokens-card)', borderColor: 'var(--Lumina-Tokens-success)' }}
        >
          <span className="text-2xs" style={{ color: 'var(--Lumina-Tokens-success)' }}>✓</span>
          <span className="text-2xs text-txt2 flex-1">
            产物已入图库 · photoId=<span className="num">{stackResultPhotoId}</span>
          </span>
          <Button
            size="sm"
            onClick={() => void useLandscape.getState().openStackResult()}
            title="在修图台打开焦点堆栈合成产物"
          >
            在修图台打开
          </Button>
        </div>
      )}
    </div>
  );
}
