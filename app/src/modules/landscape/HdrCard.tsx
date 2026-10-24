/**
 * HDR 包围曝光合成卡（v1.0.0，对照 12 号设计稿）：组检测列表 +
 * 去鬼影/自动对齐/DNG|TIFF 选项 + 一键合成（任务化，失败显式提示）+
 * 完成产物回执（result.photoId 已入图库，可一键进修图台）。
 */

import { useLandscape } from './store/landscapeStore';
import { useJobProgress } from '../../api/hooks';
import { Button, Segmented, Spinner, Toggle } from '../../components/primitives';

export default function HdrCard() {
  const groups = useLandscape((s) => s.groups);
  const groupsLoading = useLandscape((s) => s.groupsLoading);
  const groupsError = useLandscape((s) => s.groupsError);
  const selectedGroupId = useLandscape((s) => s.selectedGroupId);
  const deghost = useLandscape((s) => s.deghost);
  const autoAlign = useLandscape((s) => s.autoAlign);
  const outputFormat = useLandscape((s) => s.outputFormat);
  const hdrJobId = useLandscape((s) => s.hdrJobId);
  const hdrResultPhotoId = useLandscape((s) => s.hdrResultPhotoId);
  const hdrJob = useJobProgress(hdrJobId);

  const totalPhotos = groups.reduce((acc, g) => acc + g.photoIds.length, 0);
  const selected = groups.find((g) => g.groupId === selectedGroupId) ?? null;

  return (
    <div className="rounded-lg border border-line p-3 flex flex-col gap-2.5" style={{ background: 'var(--Lumina-Tokens-panel)' }}>
      <div className="flex items-center gap-2">
        <span className="text-xs font-medium">HDR 包围曝光合成</span>
        <div className="flex-1" />
        <span className="text-2xs px-1.5 py-0.5 rounded-sm" style={{ background: 'var(--Lumina-Tokens-accentSoft)', color: 'var(--Lumina-Tokens-accent)' }}>v1.1.1</span>
      </div>
      <div className="text-2xs text-txt3 leading-4">自动对齐 · 去鬼影 · 宽影调<i className="not-italic text-txt3">（包围组 ≥3 张）</i></div>

      {groupsLoading && (
        <div className="flex items-center justify-center py-4">
          <Spinner size={16} />
        </div>
      )}
      {!groupsLoading && groupsError && (
        <div className="text-2xs leading-5" style={{ color: 'var(--Lumina-Tokens-danger)' }}>
          包围组检测失败：{groupsError}
          <button className="ml-1.5 text-accent hover:opacity-80" onClick={() => void useLandscape.getState().load()}>重试</button>
        </div>
      )}
      {!groupsLoading && !groupsError && groups.length === 0 && (
        <div className="text-2xs text-txt3 py-2 leading-5">
          未识别到包围组
          <br />
          <span className="text-txt3">导入 RAW 连拍（同机位 ≥3 张不同 EV）后自动识别；当前图库暂无符合条件的连拍</span>
        </div>
      )}

      {groups.length > 0 && (
        <div className="flex items-center gap-2 flex-wrap">
          {groups.slice(0, 8).map((g) => (
            <button
              key={g.groupId}
              onClick={() => useLandscape.getState().selectGroup(g.groupId)}
              className={`num text-2xs h-6 px-2 rounded-md border transition-colors ${
                g.groupId === selectedGroupId ? 'border-accent text-accent' : 'border-line text-txt3 hover:text-txt2 hover:bg-hover'
              }`}
              style={{ background: g.groupId === selectedGroupId ? 'var(--Lumina-Tokens-accentSoft)' : 'var(--Lumina-Tokens-card)' }}
              title={`组 ${g.groupId} · ${g.photoIds.length} 张 · ${g.exposureStops}EV 间隔`}
            >
              组 {g.groupId} · {g.photoIds.length} 张
            </button>
          ))}
          {groups.length > 8 && <span className="num text-2xs text-txt3">+{groups.length - 8}</span>}
        </div>
      )}

      {selected && (
        <>
          <div className="flex items-center gap-4">
            <Toggle checked={deghost} onChange={(v) => useLandscape.getState().patchComposite({ deghost: v })} label="去鬼影" />
            <Toggle checked={autoAlign} onChange={(v) => useLandscape.getState().patchComposite({ autoAlign: v })} label="自动对齐" />
          </div>
          <div className="flex items-center gap-2">
            <span className="text-2xs text-txt2 shrink-0">输出</span>
            <Segmented
              value={outputFormat}
              onChange={(v) => useLandscape.getState().patchComposite({ outputFormat: v })}
              options={[
                { value: 'dng', label: 'DNG' },
                { value: 'tiff', label: 'TIFF' },
              ]}
            />
          </div>
          <div className="flex items-center gap-2">
            <Button variant="primary" size="sm" disabled={hdrJob?.state === 'running'} onClick={() => void useLandscape.getState().runComposite()}>
              {hdrJob?.state === 'running' ? `合成中 ${Math.round(hdrJob.progress)}%` : `合成组 ${selected.groupId}（${selected.photoIds.length} 张）`}
            </Button>
            <span className="num text-2xs text-txt3">共 {groups.length} 组 · {totalPhotos} 张</span>
          </div>
        </>
      )}

      {/* 合成产物回执：引擎完成事件 result.photoId（已入图库索引） */}
      {hdrResultPhotoId !== null && (
        <div
          className="rounded-md border p-2 flex items-center gap-2"
          style={{ background: 'var(--Lumina-Tokens-card)', borderColor: 'var(--Lumina-Tokens-success)' }}
        >
          <span className="text-2xs" style={{ color: 'var(--Lumina-Tokens-success)' }}>✓</span>
          <span className="text-2xs text-txt2 flex-1">
            产物已入图库 · photoId=<span className="num">{hdrResultPhotoId}</span>
          </span>
          <Button
            size="sm"
            onClick={() => void useLandscape.getState().openHdrResult()}
            title="在修图台打开 HDR 合成产物"
          >
            在修图台打开
          </Button>
        </div>
      )}
    </div>
  );
}
