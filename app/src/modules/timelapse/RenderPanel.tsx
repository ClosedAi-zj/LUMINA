/**
 * 右侧渲染输出面板（Phase A）：LR 风格可折叠分区（去闪 / 输出参数 / 渲染队列 / 调色）+
 * 开始渲染。调色（色调曲线 + 颜色分级三色轮）仅存 UI 状态，引擎接入留 Phase B。
 */

import { useEffect, useRef, useState } from 'react';
import { useTimelapse } from './store/timelapseStore';
import { useJobProgress } from '../../api/hooks';
import { Button, Segmented, Slider } from '../../components/primitives';
import { LrPanel } from '../../components/LrPanel';
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
          {activeCount > 0 && (
            <span className="num text-2xs" style={{ color: 'var(--Lumina-Tokens-gold)' }}>
              {activeCount} 个进行中
            </span>
          )}
        </div>

        {/* 去闪操作 */}
        <LrPanel title="去闪（Deflicker）" right={(analyzeJob?.state === 'running' || deflickerJob?.state === 'running') ? <span className="text-2xs" style={{ color: 'var(--Lumina-Tokens-accent)' }}>进行中…</span> : undefined}>
          <div className="px-2.5 flex flex-col gap-2">
            <Button size="sm" onClick={() => void useTimelapse.getState().analyzeFlicker()} disabled={analyzeJob?.state === 'running'}>
              {analyzeJob?.state === 'running' ? `分析中 ${Math.round(analyzeJob.progress)}%` : '去闪分析'}
            </Button>
            {/* 分析量化结果（引擎 result：{brightnessGainPct, flickerIndex}） */}
            {analyzeResult && (
              <div className="rounded-md border border-line p-2 flex flex-col gap-1" style={{ background: 'var(--Lumina-Tokens-panel)' }}>
                <span className="num text-2xs text-txt2">
                  {analyzeResult.brightnessGainPct !== null ? (
                    <>
                      亮度提升 <span style={{ color: 'var(--Lumina-Tokens-gold)' }}>{analyzeResult.brightnessGainPct.toFixed(1)}%</span>
                    </>
                  ) : (
                    '分析完成（无量化数据）'
                  )}
                  {analyzeResult.flickerIndex !== null && (
                    <>
                      {' · 闪变指数 '}
                      <span className="num">{analyzeResult.flickerIndex.toFixed(3)}</span>
                    </>
                  )}
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
        </LrPanel>

        {/* 输出参数 */}
        <LrPanel title="输出参数">
          <div className="px-2.5 flex flex-col gap-2.5">
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
        </LrPanel>

        {/* 渲染队列 */}
        <LrPanel title="渲染队列">
          <div className="px-2.5">
            <div className="flex items-center mb-1.5">
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
                  {j.state === 'running' && j.etaMs !== null && <div className="num text-2xs text-txt3 mt-0.5">剩余 {formatEta(j.etaMs)}</div>}
                  {j.state === 'failed' && j.detail && <div className="text-2xs mt-0.5" style={{ color: 'var(--Lumina-Tokens-danger)' }}>{j.detail}</div>}
                </div>
              ))}
            </div>
          </div>
        </LrPanel>

        {/* 调色（仅存 UI 状态 · 引擎接入留 Phase B） */}
        <LrPanel title="调色" right={<span className="text-2xs text-txt3">Phase B 接入</span>}>
          <div className="px-2.5 flex flex-col gap-2">
            <div className="text-2xs text-txt3">色调曲线与颜色分级为预览态参数，将于 Phase B 接入渲染。</div>
            <ToneCurveEditor />
            <ColorGradeWheels />
          </div>
        </LrPanel>

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

// ---------- 色调曲线（参数化 S 曲线：阴影 / 中间调 / 高光 三点可调） ----------

type ToneZone = 'shadow' | 'mid' | 'high';

function ToneCurveEditor() {
  const toneCurve = useTimelapse((s) => s.toneCurve);
  const setToneCurve = useTimelapse((s) => s.setToneCurve);
  const ref = useRef<SVGSVGElement>(null);
  const dragRef = useRef<ToneZone | null>(null);

  const X: Record<ToneZone, number> = { shadow: 15, mid: 50, high: 85 };
  const yOf = (v: number) => 50 - (v / 100) * 40; // value -100..100 → y 10..90
  const path = `M0,50 L${X.shadow},${yOf(toneCurve.shadow)} L${X.mid},${yOf(toneCurve.mid)} L${X.high},${yOf(toneCurve.high)} L100,50`;

  const update = (zone: ToneZone, clientY: number) => {
    const rect = ref.current?.getBoundingClientRect();
    if (!rect) return;
    const ratio = 1 - (clientY - rect.top) / rect.height;
    const v = Math.max(-100, Math.min(100, Math.round((ratio - 0.5) * 200)));
    setToneCurve({ [zone]: v } as Partial<{ shadow: number; mid: number; high: number }>);
  };

  const pickZone = (clientX: number): ToneZone => {
    const rect = ref.current?.getBoundingClientRect();
    const x = rect ? ((clientX - rect.left) / rect.width) * 100 : 50;
    const dists: [ToneZone, number][] = [
      ['shadow', X.shadow],
      ['mid', X.mid],
      ['high', X.high],
    ];
    let best = dists[0];
    for (const d of dists) if (Math.abs(d[1] - x) < Math.abs(best[1] - x)) best = d;
    return best[0];
  };

  return (
    <svg
      ref={ref}
      viewBox="0 0 100 100"
      preserveAspectRatio="none"
      className="w-full rounded-md border border-line cursor-ns-resize"
      style={{ height: 120, background: 'var(--Lumina-Tokens-card)', touchAction: 'none' }}
      role="group"
      aria-label="色调曲线"
      onPointerDown={(e) => {
        const zone = pickZone(e.clientX);
        dragRef.current = zone;
        (e.target as Element).setPointerCapture?.(e.pointerId);
        update(zone, e.clientY);
      }}
      onPointerMove={(e) => {
        if (!dragRef.current) return;
        update(dragRef.current, e.clientY);
      }}
      onPointerUp={() => {
        dragRef.current = null;
      }}
    >
      <line x1="0" y1="50" x2="100" y2="50" stroke="var(--Lumina-Tokens-border)" strokeWidth="0.5" vectorEffect="non-scaling-stroke" />
      <path d={path} fill="none" stroke="var(--Lumina-Tokens-gold)" strokeWidth="1.5" vectorEffect="non-scaling-stroke" />
      {(['shadow', 'mid', 'high'] as ToneZone[]).map((z) => (
        <circle key={z} cx={X[z]} cy={yOf(toneCurve[z])} r="3" fill="var(--Lumina-Tokens-gold)" stroke="var(--Lumina-Tokens-card)" strokeWidth="1" vectorEffect="non-scaling-stroke" />
      ))}
    </svg>
  );
}

// ---------- 颜色分级三色轮（阴影 / 中间调 / 高光） ----------

type GradeZone = 'shadows' | 'mid' | 'high';

const ZONE_LABEL: Record<GradeZone, string> = { shadows: '阴影', mid: '中间调', high: '高光' };

function ColorGradeWheels() {
  const colorGrade = useTimelapse((s) => s.colorGrade);
  const setColorGrade = useTimelapse((s) => s.setColorGrade);

  return (
    <div className="flex flex-col gap-1.5">
      {(['shadows', 'mid', 'high'] as GradeZone[]).map((z) => (
        <ColorWheelRow
          key={z}
          label={ZONE_LABEL[z]}
          hue={colorGrade[z].hue}
          amount={colorGrade[z].amount}
          onChange={(hue, amount) => setColorGrade(z, { hue, amount })}
        />
      ))}
    </div>
  );
}

function ColorWheelRow({ label, hue, amount, onChange }: { label: string; hue: number; amount: number; onChange: (hue: number, amount: number) => void }) {
  const R = 30;
  const cx = 40;
  const cy = 40;
  const rad = (hue * Math.PI) / 180;
  const dist = (amount / 100) * R * 0.82;
  const hx = cx + Math.cos(rad) * dist;
  const hy = cy - Math.sin(rad) * dist;
  const dragging = useRef(false);
  const ref = useRef<SVGSVGElement>(null);

  const update = (clientX: number, clientY: number) => {
    const rect = ref.current?.getBoundingClientRect();
    if (!rect) return;
    const x = ((clientX - rect.left) / rect.width) * 80 - cx;
    const y = ((clientY - rect.top) / rect.height) * 80 - cy;
    let ang = (Math.atan2(-y, x) * 180) / Math.PI;
    if (ang < 0) ang += 360;
    const amt = Math.max(0, Math.min(100, Math.round((Math.hypot(x, y) / (R * 0.82)) * 100)));
    onChange(Math.round(ang), amt);
  };

  return (
    <div className="flex items-center gap-2">
      <svg
        ref={ref}
        viewBox="0 0 80 80"
        className="cursor-crosshair shrink-0"
        style={{ width: 56, height: 56, touchAction: 'none' }}
        role="slider"
        aria-label={`${label} 颜色分级`}
        aria-valuenow={hue}
        aria-valuemin={0}
        aria-valuemax={360}
        onPointerDown={(e) => {
          dragging.current = true;
          (e.target as Element).setPointerCapture?.(e.pointerId);
          update(e.clientX, e.clientY);
        }}
        onPointerMove={(e) => {
          if (!dragging.current) return;
          update(e.clientX, e.clientY);
        }}
        onPointerUp={() => {
          dragging.current = false;
        }}
      >
        <circle cx={cx} cy={cy} r={R} fill="var(--Lumina-Tokens-card)" stroke="var(--Lumina-Tokens-border)" strokeWidth="1" />
        {/* 色相环（颜色分级色轮本质需展示色相，hsl 为领域固有而非 UI 色值） */}
        {Array.from({ length: 60 }, (_, i) => i * 6).map((a) => {
          const rr = (a * Math.PI) / 180;
          const x1 = cx + Math.cos(rr) * R;
          const y1 = cy - Math.sin(rr) * R;
          const x2 = cx + Math.cos(rr) * (R - 6);
          const y2 = cy - Math.sin(rr) * (R - 6);
          return <line key={a} x1={x1} y1={y1} x2={x2} y2={y2} stroke={`hsl(${a},65%,55%)`} strokeWidth="1.2" />;
        })}
        <line x1={cx} y1={cy} x2={hx} y2={hy} stroke="var(--Lumina-Tokens-gold)" strokeWidth="1.5" />
        <circle cx={hx} cy={hy} r="4" fill="var(--Lumina-Tokens-gold)" stroke="var(--Lumina-Tokens-card)" strokeWidth="1" />
      </svg>
      <div className="flex-1 min-w-0">
        <div className="text-2xs text-txt2 mb-1">{label}</div>
        <Slider label="强度" value={amount} min={0} max={100} defaultValue={0} onChange={(v) => onChange(hue, v)} format={(v) => `${v}%`} />
      </div>
    </div>
  );
}
