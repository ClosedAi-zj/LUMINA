/**
 * 右侧面板（对照 LapseOne/Polarr 参考重排）：
 * 顶部 RGB 直方图大图（panel 变体）→ 去闪 → 调色（Polarr 风格曲线编辑器 + 彩虹色轮）→
 * 输出参数 → 开始渲染。调色参数随渲染透传引擎 colorAdjust（不再是 Phase B 占位）。
 */

import { useEffect, useRef, useState } from 'react';
import { useTimelapse } from './store/timelapseStore';
import { useJobProgress } from '../../api/hooks';
import { Button, Segmented, Slider } from '../../components/primitives';
import { LrPanel } from '../../components/LrPanel';
import { formatEta, formatCount } from '../../utils/format';
import Histogram from './Histogram';

const DEFlicker_STRENGTH_DEFAULT = 50;

export default function RenderPanel() {
  const detail = useTimelapse((s) => s.detail);
  const selectedSeqId = useTimelapse((s) => s.selectedSeqId);
  const frameNo = useTimelapse((s) => s.frameNo);
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
          <span className="text-xs font-medium">调色与修复</span>
          <div className="flex-1" />
          {activeCount > 0 && (
            <span className="num text-2xs" style={{ color: 'var(--Lumina-Tokens-accent)' }}>
              {activeCount} 个任务进行中
            </span>
          )}
        </div>

        {/* RGB 直方图（右栏顶部大图，对照 LapseOne 参考布局） */}
        <Histogram seqId={selectedSeqId} frameNo={frameNo} variant="panel" />

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

        {/* 调色：参数随渲染透传引擎 colorAdjust（引擎对零值≈恒等） */}
        <LrPanel title="调色" right={<span className="text-2xs text-txt3">随渲染生效</span>}>
          <div className="px-2.5 flex flex-col gap-3">
            <PolarrCurveEditor />
            <PolarrColorWheels />
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

// ---------- 色调曲线（Polarr 风格：4×4 暗网格 + 白 S 曲线 + 圆形控制点 + 对角参考线 + 黑白渐变轴） ----------

type ToneZone = 'shadow' | 'mid' | 'high';

function PolarrCurveEditor() {
  const toneCurve = useTimelapse((s) => s.toneCurve);
  const setToneCurve = useTimelapse((s) => s.setToneCurve);
  const ref = useRef<SVGSVGElement>(null);
  const dragRef = useRef<ToneZone | null>(null);

  // 控制点锚在 4×4 网格交点上（Polarr 质感的关键之一）
  const X: Record<ToneZone, number> = { shadow: 25, mid: 50, high: 75 };
  const yOf = (v: number) => 50 - (v / 100) * 40; // value -100..100 → y 90..10
  const pts: [number, number][] = [
    [X.shadow, yOf(toneCurve.shadow)],
    [X.mid, yOf(toneCurve.mid)],
    [X.high, yOf(toneCurve.high)],
  ];

  // Catmull-Rom → 三次贝塞尔：让 S 曲线像 Polarr 一样圆滑而非折线
  const path = (() => {
    const anchors: [number, number][] = [[0, 100], ...pts, [100, 0]];
    let d = `M${anchors[0][0]},${anchors[0][1]}`;
    for (let i = 0; i < anchors.length - 1; i++) {
      const p0 = anchors[Math.max(0, i - 1)];
      const p1 = anchors[i];
      const p2 = anchors[i + 1];
      const p3 = anchors[Math.min(anchors.length - 1, i + 2)];
      const c1x = p1[0] + (p2[0] - p0[0]) / 6;
      const c1y = p1[1] + (p2[1] - p0[1]) / 6;
      const c2x = p2[0] - (p3[0] - p1[0]) / 6;
      const c2y = p2[1] - (p3[1] - p1[1]) / 6;
      d += ` C${c1x.toFixed(2)},${c1y.toFixed(2)} ${c2x.toFixed(2)},${c2y.toFixed(2)} ${p2[0]},${p2[1]}`;
    }
    return d;
  })();

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
    let best: ToneZone = 'mid';
    for (const z of ['shadow', 'mid', 'high'] as ToneZone[]) {
      if (Math.abs(X[z] - x) < Math.abs(X[best] - x)) best = z;
    }
    return best;
  };

  const gridLines = [25, 50, 75];

  return (
    <div className="flex flex-col gap-1.5">
      <svg
        ref={ref}
        viewBox="0 0 100 100"
        preserveAspectRatio="none"
        className="w-full rounded-md border border-line cursor-ns-resize"
        style={{ height: 150, background: 'var(--Lumina-Tokens-viewer)', touchAction: 'none' }}
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
        {/* 4×4 暗网格 */}
        {gridLines.map((g) => (
          <line key={`v${g}`} x1={g} y1={0} x2={g} y2={100} stroke="var(--Lumina-Tokens-border)" strokeWidth="0.5" vectorEffect="non-scaling-stroke" />
        ))}
        {gridLines.map((g) => (
          <line key={`h${g}`} x1={0} y1={g} x2={100} y2={g} stroke="var(--Lumina-Tokens-border)" strokeWidth="0.5" vectorEffect="non-scaling-stroke" />
        ))}
        {/* 对角参考线（恒等曲线） */}
        <line x1={0} y1={100} x2={100} y2={0} stroke="var(--Lumina-Tokens-textMuted)" strokeWidth="0.6" strokeDasharray="3 3" vectorEffect="non-scaling-stroke" opacity={0.5} />
        {/* 白色 S 曲线 */}
        <path d={path} fill="none" stroke="var(--Lumina-Tokens-textStrong)" strokeWidth="1.8" vectorEffect="non-scaling-stroke" />
        {/* 方形端点（左下 / 右上，Polarr 同款） */}
        <rect x={0.8} y={97.6} width={4.4} height={4.4} fill="var(--Lumina-Tokens-textStrong)" vectorEffect="non-scaling-stroke" />
        <rect x={94.8} y={-2} width={4.4} height={4.4} fill="var(--Lumina-Tokens-textStrong)" vectorEffect="non-scaling-stroke" />
        {/* 圆形控制点（白底深环） */}
        {pts.map(([cx, cy], i) => (
          <circle key={i} cx={cx} cy={cy} r={2.6} fill="var(--Lumina-Tokens-textStrong)" stroke="var(--Lumina-Tokens-viewer)" strokeWidth={1} vectorEffect="non-scaling-stroke" />
        ))}
      </svg>
      {/* 黑白渐变轴 = 输入色调图例 + 三区数值 */}
      <div className="h-1.5 rounded-full" style={{ background: 'linear-gradient(to right, #000, #fff)' }} />
      <div className="flex items-center justify-between num text-2xs text-txt3">
        <span>阴影 <span className="text-txt2">{toneCurve.shadow > 0 ? '+' : ''}{toneCurve.shadow}</span></span>
        <span>中间调 <span className="text-txt2">{toneCurve.mid > 0 ? '+' : ''}{toneCurve.mid}</span></span>
        <span>高光 <span className="text-txt2">{toneCurve.high > 0 ? '+' : ''}{toneCurve.high}</span></span>
      </div>
    </div>
  );
}

// ---------- 颜色分级色轮（Polarr 风格：彩虹色盘 + 白灰中心 + 控制点 + 饱和度滑块） ----------

type GradeZone = 'shadows' | 'mid' | 'high';

const ZONE_LABEL: Record<GradeZone, string> = { shadows: '阴影', mid: '中间调', high: '高光' };

/** 彩虹色盘（红在右、品红在下、蓝在左下、绿在左上，Polarr 同向）：外圈彩虹、向心过渡到白灰 */
const WHEEL_BG =
  'radial-gradient(circle, rgba(250,250,252,0.95) 0%, rgba(250,250,252,0.55) 42%, rgba(250,250,252,0) 72%), ' +
  'conic-gradient(from 90deg, #ff3b30, #ff2d92, #af52ff, #5e5ce6, #0a84ff, #64d2ff, #30d158, #ffd60a, #ff9f0a, #ff3b30)';

function PolarrColorWheels() {
  const colorGrade = useTimelapse((s) => s.colorGrade);
  const setColorGrade = useTimelapse((s) => s.setColorGrade);

  return (
    <div className="flex flex-col gap-3">
      {/* 中间调大轮（Polarr 布局：主轮在上居中） */}
      <ColorWheel
        label={ZONE_LABEL.mid}
        size={112}
        hue={colorGrade.mid.hue}
        amount={colorGrade.mid.amount}
        onChange={(hue, amount) => setColorGrade('mid', { hue, amount })}
      />
      {/* 阴影 / 高光小轮并排 */}
      <div className="grid grid-cols-2 gap-2">
        <ColorWheel
          label={ZONE_LABEL.shadows}
          size={84}
          hue={colorGrade.shadows.hue}
          amount={colorGrade.shadows.amount}
          onChange={(hue, amount) => setColorGrade('shadows', { hue, amount })}
        />
        <ColorWheel
          label={ZONE_LABEL.high}
          size={84}
          hue={colorGrade.high.hue}
          amount={colorGrade.high.amount}
          onChange={(hue, amount) => setColorGrade('high', { hue, amount })}
        />
      </div>
    </div>
  );
}

function ColorWheel({ label, size, hue, amount, onChange }: { label: string; size: number; hue: number; amount: number; onChange: (hue: number, amount: number) => void }) {
  const dragging = useRef(false);
  const ref = useRef<HTMLDivElement>(null);

  // 控制点位置：角度 = hue（0°=正右、逆时针增大，与色盘同向），距离 ∝ amount
  const rad = (hue * Math.PI) / 180;
  const R = size / 2;
  const dist = (amount / 100) * R * 0.78;
  const dx = Math.cos(rad) * dist;
  const dy = -Math.sin(rad) * dist;

  const update = (clientX: number, clientY: number) => {
    const rect = ref.current?.getBoundingClientRect();
    if (!rect) return;
    const cx = rect.left + rect.width / 2;
    const cy = rect.top + rect.height / 2;
    const x = clientX - cx;
    const y = clientY - cy;
    let ang = (Math.atan2(-y, x) * 180) / Math.PI;
    if (ang < 0) ang += 360;
    const maxDist = (rect.width / 2) * 0.78;
    const amt = Math.max(0, Math.min(100, Math.round((Math.hypot(x, y) / maxDist) * 100)));
    onChange(Math.round(ang), amt);
  };

  return (
    <div className="flex flex-col items-center gap-1.5">
      <div
        ref={ref}
        className="relative rounded-full cursor-crosshair select-none"
        style={{
          width: size,
          height: size,
          background: WHEEL_BG,
          boxShadow: 'inset 0 0 0 1px var(--Lumina-Tokens-borderStrong)',
          touchAction: 'none',
        }}
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
        {/* 控制点（白底深环，Polarr 同款灰白点） */}
        <span
          className="absolute w-3 h-3 rounded-full pointer-events-none"
          style={{
            left: `calc(50% + ${dx}px - 6px)`,
            top: `calc(50% + ${dy}px - 6px)`,
            background: '#f2f2f5',
            border: '1.5px solid rgba(40,42,48,0.85)',
            boxShadow: '0 1px 3px rgba(0,0,0,0.4)',
          }}
        />
      </div>
      <div className="w-full px-1">
        <div className="flex items-center justify-between mb-0.5">
          <span className="text-2xs text-txt2">{label}</span>
          <span className="num text-2xs text-txt3">{amount}%</span>
        </div>
        <Slider label={`${label} 强度`} value={amount} min={0} max={100} defaultValue={0} onChange={(v) => onChange(hue, v)} format={(v) => `${v}%`} />
      </div>
    </div>
  );
}
