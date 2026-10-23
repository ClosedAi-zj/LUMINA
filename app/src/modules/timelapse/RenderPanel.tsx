/**
 * 右侧渲染输出面板（对照 08 号设计稿）：分辨率/帧率/编码分段 + 去闪操作
 * （分析量化结果 + warnings 零静默展示）+ 渲染队列（进度走事件泵 + 3s 轮询）+
 * 开始渲染（含预计时长）。
 */

import { useEffect, useState } from 'react';
import { useTimelapse } from './store/timelapseStore';
import { useJobProgress } from '../../api/hooks';
import { Button, Segmented, Slider } from '../../components/primitives';
import { formatEta, formatCount } from '../../utils/format';

const DEFlicker_STRENGTH_DEFAULT = 50;

export default function RenderPanel() {
  const detail = useTimelapse((s) => s.detail);
  const resolution = useTimelapse((s) => s.resolution);
  const fps = useTimelapse((s) => s.fps);
  const codec = useTimelapse((s) => s.codec);
  const renderJobs = useTimelapse((s) => s.renderJobs);
  const analyzeJobId = useTimelapse((s) => s.analyzeJobId);
  const deflickerJobId = useTimelapse((s) => s.deflickerJobId);
  const analyzeResult = useTimelapse((s) => s.analyzeResult);
  const analyzeJob = useJobProgress(analyzeJobId);
  const deflickerJob = useJobProgress(deflickerJobId);
  const [deflickerStrength, setDeflickerStrength] = useState(DEFlicker_STRENGTH_DEFAULT);

  // 队列轮询；分析/去闪 job 完成后刷新详情（deflicker 状态回写）
  useEffect(() => {
    void useTimelapse.getState().refreshJobs();
    const t = setInterval(() => void useTimelapse.getState().refreshJobs(), 3000);
    return () => clearInterval(t);
  }, []);
  useEffect(() => {
    if (analyzeJob?.state === 'done' || deflickerJob?.state === 'done') void useTimelapse.getState().refreshDetail();
  }, [analyzeJob?.state, deflickerJob?.state]);

  // 预计渲染时长：粗略按帧数 × 分辨率系数估算（仅展示用途）
  const estimateSec = detail ? Math.round((detail.frameCount / fps) * (resolution === '6K' ? 2.2 : resolution === '4K' ? 1.6 : 1)) : 0;

  const activeCount = renderJobs.filter((j) => j.state === 'running' || j.state === 'queued').length;

  return (
    <aside className="shrink-0 w-[300px] border-l border-line flex flex-col overflow-y-auto" style={{ background: 'var(--Lumina-Tokens-panel)' }}>
      <div className="p-3 flex flex-col gap-3">
        <div className="flex items-center">
          <span className="text-xs font-medium">渲染输出</span>
          <div className="flex-1" />
          {activeCount > 0 && <span className="num text-2xs text-accent">{activeCount} 个进行中</span>}
        </div>

        {/* 去闪操作 */}
        <div className="rounded-lg border border-line p-2.5 flex flex-col gap-2" style={{ background: 'var(--Lumina-Tokens-card)' }}>
          <div className="flex items-center gap-1.5">
            <span className="text-2xs text-txt2 font-medium">去闪（Deflicker）</span>
            <div className="flex-1" />
            {(analyzeJob?.state === 'running' || deflickerJob?.state === 'running') && <span className="text-2xs text-accent">进行中…</span>}
          </div>
          <Button size="sm" onClick={() => void useTimelapse.getState().analyzeFlicker()} disabled={analyzeJob?.state === 'running'}>
            {analyzeJob?.state === 'running' ? `分析中 ${Math.round(analyzeJob.progress)}%` : '去闪分析'}
          </Button>
          {/* 分析量化结果（引擎 result：{brightnessGainPct, flickerIndex}） */}
          {analyzeResult && (
            <div className="rounded-md border border-line p-2 flex flex-col gap-1" style={{ background: 'var(--Lumina-Tokens-panel)' }}>
              <span className="num text-2xs text-txt2">
                {analyzeResult.brightnessGainPct !== null ? <>亮度提升 <span className="text-accent">{analyzeResult.brightnessGainPct.toFixed(1)}%</span></> : '分析完成（无量化数据）'}
                {analyzeResult.flickerIndex !== null && <> · 闪变指数 <span className="num">{analyzeResult.flickerIndex.toFixed(3)}</span></>}
              </span>
              {analyzeResult.warnings.map((w, i) => (
                <span key={i} className="text-2xs leading-4" style={{ color: 'var(--Lumina-Tokens-danger)' }}>
                  ⚠ {w}
                </span>
              ))}
            </div>
          )}
          <Slider
            label="应用强度"
            value={deflickerStrength}
            min={10}
            max={100}
            defaultValue={DEFlicker_STRENGTH_DEFAULT}
            onChange={(v) => setDeflickerStrength(v)}
            format={(v) => `${v}%`}
          />
          <Button
            size="sm"
            variant="primary"
            disabled={deflickerJob?.state === 'running'}
            onClick={() => void useTimelapse.getState().applyDeflicker(deflickerStrength)}
          >
            应用去闪（强度 {deflickerStrength}%）
          </Button>
        </div>

        {/* 输出参数 */}
        <div className="flex flex-col gap-2.5">
          <div>
            <div className="text-2xs text-txt2 mb-1">分辨率</div>
            <Segmented
              value={resolution}
              onChange={(v) => useTimelapse.getState().setResolution(v)}
              options={[
                { value: '1080p', label: '1080p' },
                { value: '4K', label: '4K' },
                { value: '6K', label: '6K' },
              ]}
            />
          </div>
          <div>
            <div className="text-2xs text-txt2 mb-1">帧率</div>
            <Segmented
              value={String(fps)}
              onChange={(v) => useTimelapse.getState().setFps(Number(v) as 25 | 30 | 60)}
              options={[
                { value: '25', label: '25' },
                { value: '30', label: '30' },
                { value: '60', label: '60' },
              ]}
            />
          </div>
          <div>
            <div className="text-2xs text-txt2 mb-1">编码</div>
            <Segmented
              value={codec}
              onChange={(v) => useTimelapse.getState().setCodec(v)}
              options={[
                { value: 'prores', label: 'ProRes 422' },
                { value: 'h265', label: 'H.265' },
                { value: 'h264', label: 'H.264' },
              ]}
            />
          </div>
          <div className="text-2xs text-txt3">输出位置 · 落盘后经任务详情给出完整路径</div>
        </div>

        {/* 渲染队列 */}
        <div>
          <div className="flex items-center mb-1.5">
            <span className="text-2xs text-txt2 font-medium">渲染队列</span>
            <div className="flex-1" />
            <button className="text-2xs text-txt3 hover:text-txt" onClick={() => void useTimelapse.getState().refreshJobs()}>
              刷新
            </button>
          </div>
          {renderJobs.length === 0 && <div className="text-2xs text-txt3 py-2">暂无渲染任务</div>}
          <div className="flex flex-col gap-1.5">
            {renderJobs.slice(0, 4).map((j) => (
              <div key={j.jobId} className="rounded-md border border-line p-2" style={{ background: 'var(--Lumina-Tokens-card)' }}>
                <div className="flex items-center gap-1.5">
                  <span className="num text-2xs text-txt2 truncate flex-1">任务 {j.jobId}</span>
                  <span
                    className="text-2xs shrink-0"
                    style={{
                      color:
                        j.state === 'running' ? 'var(--Lumina-Tokens-accent)' : j.state === 'done' ? 'var(--Lumina-Tokens-success)' : j.state === 'failed' ? 'var(--Lumina-Tokens-danger)' : 'var(--Lumina-Tokens-textMuted)',
                    }}
                  >
                    {j.state === 'running' ? '渲染中' : j.state === 'done' ? '完成' : j.state === 'failed' ? '失败' : j.state === 'queued' ? '排队' : '已取消'}
                  </span>
                </div>
                {(j.state === 'running' || j.state === 'queued') && (
                  <div className="mt-1 h-1 rounded-full overflow-hidden" style={{ background: 'var(--Lumina-Tokens-hover)' }}>
                    <div className="h-full rounded-full bg-accent transition-all" style={{ width: `${Math.max(2, j.progress)}%` }} />
                  </div>
                )}
                {j.state === 'running' && j.etaMs !== null && (
                  <div className="num text-2xs text-txt3 mt-0.5">剩余 {formatEta(j.etaMs)}</div>
                )}
                {j.state === 'failed' && j.detail && <div className="text-2xs mt-0.5" style={{ color: 'var(--Lumina-Tokens-danger)' }}>{j.detail}</div>}
              </div>
            ))}
          </div>
        </div>

        {/* 开始渲染 */}
        <div className="flex flex-col gap-1.5">
          <Button variant="primary" size="lg" disabled={!detail || detail.frameCount === 0} onClick={() => void useTimelapse.getState().render()}>
            开始渲染
          </Button>
          {detail && (
            <span className="num text-2xs text-txt3 text-center">
              {formatCount(detail.frameCount)} 帧 · {resolution} · {fps}fps · 预计 ≈ {estimateSec < 60 ? `${estimateSec} 秒` : `${Math.floor(estimateSec / 60)} 分 ${estimateSec % 60} 秒`}
            </span>
          )}
        </div>
      </div>
    </aside>
  );
}
