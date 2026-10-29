/**
 * 导出视频视图（对照 LapseOne「导出视频」参考）：中央批量任务表格 +
 * 右栏导出参数。数据全部来自引擎真实渲染队列（JobInfo），
 * 不做假按钮：引擎暂无 停止/移除/清空 API，故不渲染对应控件（诚实红线）。
 */

import { useState } from 'react';
import { useTimelapse, type Codec, type Fps, type Resolution } from './store/timelapseStore';
import { Button } from '../../components/primitives';
import { formatEta, formatCount } from '../../utils/format';

const STATE_META: Record<string, { label: string; color: string }> = {
  running: { label: '渲染中', color: 'var(--Lumina-Tokens-accent)' },
  queued: { label: '排队中', color: 'var(--Lumina-Tokens-textMuted)' },
  done: { label: '完成', color: 'var(--Lumina-Tokens-success)' },
  failed: { label: '失败', color: 'var(--Lumina-Tokens-danger)' },
  cancelled: { label: '已取消', color: 'var(--Lumina-Tokens-textMuted)' },
};

/** 目标码率估算（仅展示参考，未透传引擎）：按分辨率档位粗估 */
const BITRATE_EST: Record<Resolution, string> = { '1080p': '16 Mbps', '4K': '52 Mbps', '6K': '85 Mbps' };

export default function ExportView() {
  const detail = useTimelapse((s) => s.detail);
  const resolution = useTimelapse((s) => s.resolution);
  const fps = useTimelapse((s) => s.fps);
  const codec = useTimelapse((s) => s.codec);
  const renderJobs = useTimelapse((s) => s.renderJobs);
  const [exportDir, setExportDir] = useState('');

  const runningCount = renderJobs.filter((j) => j.state === 'running' || j.state === 'queued').length;
  const doneCount = renderJobs.filter((j) => j.state === 'done').length;

  return (
    <>
      {/* 中央批量表格 */}
      <div className="flex-1 min-w-0 flex flex-col overflow-hidden" style={{ background: 'var(--Lumina-Tokens-panel)' }}>
        {/* 工具条（对照 LapseOne：左标题 + 蓝色主按钮） */}
        <div className="shrink-0 h-11 px-3 flex items-center gap-2 border-b border-line">
          <span className="text-xs font-medium">导出队列</span>
          <div className="flex-1" />
          <span className="num text-2xs text-txt3 mr-1">
            进行中 {runningCount} · 完成 {doneCount}
          </span>
          <Button
            variant="primary"
            size="sm"
            disabled={!detail || detail.frameCount === 0}
            onClick={() => void useTimelapse.getState().render()}
            title="将当前选中序列按右侧参数加入渲染队列"
          >
            渲染当前序列
          </Button>
          <button className="text-2xs text-txt3 hover:text-txt px-1.5" onClick={() => void useTimelapse.getState().refreshJobs()}>
            刷新
          </button>
        </div>

        {/* 表格 */}
        <div className="flex-1 min-h-0 overflow-y-auto">
          {renderJobs.length === 0 ? (
            <div className="h-full flex flex-col items-center justify-center gap-1.5 text-center px-6">
              <span className="text-2xs text-txt3 leading-5">暂无渲染任务</span>
              <span className="text-2xs text-txt3 leading-5">在「调色与修复」页设置输出参数后，点击「渲染当前序列」加入队列</span>
            </div>
          ) : (
            <table className="w-full text-left" style={{ borderCollapse: 'collapse' }}>
              <thead>
                <tr className="border-b border-line">
                  {['状态', '任务', '输出设置', '范围', '进度', '预计剩余', '详情'].map((h) => (
                    <th key={h} className="text-2xs text-txt3 font-medium px-3 py-2 whitespace-nowrap">
                      {h}
                    </th>
                  ))}
                </tr>
              </thead>
              <tbody>
                {renderJobs.map((j, i) => {
                  const meta = STATE_META[j.state] ?? STATE_META.queued;
                  const active = j.state === 'running' || j.state === 'queued';
                  return (
                    <tr key={j.jobId} className="border-b border-line" style={{ background: i % 2 === 1 ? 'var(--Lumina-Tokens-card)' : 'transparent' }}>
                      <td className="px-3 py-2 text-2xs whitespace-nowrap" style={{ color: meta.color }}>
                        {meta.label}
                      </td>
                      <td className="px-3 py-2 num text-2xs text-txt whitespace-nowrap">任务 {j.jobId}</td>
                      {/* 输出设置：引擎未在 JobInfo 回传逐任务参数，展示当前面板参数口径（表头口径说明） */}
                      <td className="px-3 py-2 text-2xs text-txt2 whitespace-nowrap">
                        {resolution} · {fps}fps · {codec === 'prores' ? 'ProRes' : codec.toUpperCase()}
                      </td>
                      <td className="px-3 py-2 text-2xs text-txt3 whitespace-nowrap" title="渲染区间暂为全序列（入出点区间渲染随 Phase B 接入）">
                        全部{detail ? ` · ${formatCount(detail.frameCount)} 帧` : ''}
                      </td>
                      <td className="px-3 py-2" style={{ minWidth: 140 }}>
                        <div className="flex items-center gap-2">
                          <div className="flex-1 h-1.5 rounded-full overflow-hidden" style={{ background: 'var(--Lumina-Tokens-hover)' }}>
                            <div
                              className="h-full rounded-full transition-all"
                              style={{
                                width: `${Math.max(2, j.progress)}%`,
                                background: j.state === 'done' ? 'var(--Lumina-Tokens-success)' : j.state === 'failed' ? 'var(--Lumina-Tokens-danger)' : 'var(--Lumina-Tokens-accent)',
                              }}
                            />
                          </div>
                          <span className="num text-2xs text-txt3 w-9 text-right">{Math.round(j.progress)}%</span>
                        </div>
                      </td>
                      <td className="px-3 py-2 num text-2xs text-txt3 whitespace-nowrap">
                        {active && j.etaMs !== null ? formatEta(j.etaMs) : '—'}
                      </td>
                      <td className="px-3 py-2 text-2xs text-txt3 max-w-[200px] truncate" title={j.detail}>
                        {j.detail || '—'}
                      </td>
                    </tr>
                  );
                })}
              </tbody>
            </table>
          )}
        </div>
      </div>

      {/* 右栏导出参数（对照 LapseOne：目录 / 编码 / 尺寸 / FPS / 码率） */}
      <aside className="shrink-0 w-[280px] border-l border-line flex flex-col overflow-y-auto" style={{ background: 'var(--Lumina-Tokens-panel)' }}>
        <div className="p-3 flex flex-col gap-3">
          <div className="flex items-center gap-2">
            <span className="text-xs font-medium">导出视频</span>
            {renderJobs.length > 0 && (
              <span className="num text-2xs px-1.5 rounded-full border border-line text-txt3">{renderJobs.length}</span>
            )}
          </div>

          {/* 导出目录：当前为预置（引擎落盘路径以任务详情为准，Phase B 接入自定义目录） */}
          <div>
            <div className="text-2xs text-txt2 mb-1">导出目录（预置）</div>
            <div className="flex gap-1.5">
              <input
                value={exportDir}
                onChange={(e) => setExportDir(e.target.value)}
                placeholder="渲染落盘路径以任务详情为准"
                className="flex-1 min-w-0 h-7 px-2 rounded-md border border-line text-2xs bg-card text-txt placeholder:text-txt3"
                readOnly
              />
              <button
                type="button"
                className="shrink-0 h-7 px-2 rounded-md border border-line text-2xs text-txt2 bg-card hover:bg-hover"
                onClick={async () => {
                  const picked = await window.lumina.pickDirectory({ title: '选择导出目录（预置，Phase B 生效）' });
                  if (picked) setExportDir(picked);
                }}
              >
                浏览
              </button>
            </div>
            <div className="text-2xs text-txt3 mt-1">自定义输出目录将随 Phase B 接入渲染管线，当前选择仅作预置记忆。</div>
          </div>

          <ParamSelect
            label="编码"
            value={codec}
            onChange={(v) => useTimelapse.getState().setCodec(v as Codec)}
            options={[
              { value: 'h265', label: 'H.265 (MP4)' },
              { value: 'h264', label: 'H.264 (MP4)' },
              { value: 'prores', label: 'ProRes 422 (MOV)' },
            ]}
          />
          <ParamSelect
            label="尺寸"
            value={resolution}
            onChange={(v) => useTimelapse.getState().setResolution(v as Resolution)}
            options={[
              { value: '1080p', label: '1080p' },
              { value: '4K', label: '4K (UHD)' },
              { value: '6K', label: '6K' },
            ]}
          />
          <ParamSelect
            label="FPS"
            value={String(fps)}
            onChange={(v) => useTimelapse.getState().setFps(Number(v) as Fps)}
            options={[
              { value: '25', label: '25' },
              { value: '30', label: '30' },
              { value: '60', label: '60' },
            ]}
          />

          {/* 码率：展示性估算（未透传引擎，明确标注） */}
          <div>
            <div className="text-2xs text-txt2 mb-1">目标码率（估算）</div>
            <div className="h-7 px-2 flex items-center rounded-md border border-line bg-card num text-2xs text-txt2">
              {BITRATE_EST[resolution]}
            </div>
          </div>

          <div className="flex-1" />
          <Button
            variant="primary"
            disabled={!detail || detail.frameCount === 0}
            onClick={() => void useTimelapse.getState().render()}
          >
            渲染当前序列
          </Button>
          {detail && (
            <span className="num text-2xs text-txt3 text-center">
              {formatCount(detail.frameCount)} 帧 · {resolution} · {fps}fps
            </span>
          )}
        </div>
      </aside>
    </>
  );
}

function ParamSelect({ label, value, onChange, options }: { label: string; value: string; onChange: (v: string) => void; options: { value: string; label: string }[] }) {
  return (
    <div>
      <div className="text-2xs text-txt2 mb-1">{label}</div>
      <select
        value={value}
        onChange={(e) => onChange(e.target.value)}
        className="w-full h-7 px-1.5 rounded-md border border-line text-2xs bg-card text-txt cursor-pointer"
        aria-label={label}
      >
        {options.map((o) => (
          <option key={o.value} value={o.value}>
            {o.label}
          </option>
        ))}
      </select>
    </div>
  );
}
