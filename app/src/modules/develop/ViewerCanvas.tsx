/**
 * 修图台画面（ViewerSurface 恒色 #08090B · 红线 R5）：
 * WebGL 实时渲染 + 前后对比分割（\ 键 / 拖动分割线）+ 直方图采样。
 */

import { useEffect, useRef, useState } from 'react';
import { useDevelop } from '../../store/developStore';
import { useJobStore } from '../../store/jobStore';
import { DevelopRenderer } from './webgl/renderer';
import { buildBrushField, FIELD_CELL } from './webgl/maskField';
import { fileUrl } from '../../api/rpc-client';
import { api } from '../../api/engine';
import { ViewerSurface } from '../../theme/ThemeProvider';
import { Icon } from '../../components/shell';
import type { EditParams } from '../../api/protocol';

/** C1 基线诊断开关（?baseline=1）：修图台画布在基线走查中纯黑（px=0,0,0），需要链路日志定位 */
const IS_BASELINE = new URLSearchParams(window.location.search).get('baseline') === '1';

/** 兜底：WebGL 不可用/渲染失败时用 2D 画布至少把底图画出来（参数调整不生效，但不黑屏） */
function drawFallback2d(canvas: HTMLCanvasElement, img: HTMLImageElement): boolean {
  const ctx = canvas.getContext('2d');
  if (!ctx || !img.naturalWidth || !img.naturalHeight) return false;
  const rect = canvas.getBoundingClientRect();
  const cssW = rect.width > 0 ? rect.width : img.naturalWidth;
  const ratio = img.naturalHeight / img.naturalWidth;
  const cssH = Math.min(rect.height > 0 ? rect.height : cssW * ratio, cssW * ratio);
  const w = Math.max(64, Math.round(cssW));
  const h = Math.max(36, Math.round(cssH));
  if (canvas.width !== w || canvas.height !== h) {
    canvas.width = w;
    canvas.height = h;
  }
  ctx.clearRect(0, 0, w, h);
  ctx.drawImage(img, 0, 0, w, h);
  return true;
}

/**
 * T1.5：CPU 降级渲染 = 引擎 develop.render（compilePlan/executePlan，与导出同一实现）。
 * 旧 webgl/cpu-render.ts 近似公式已销毁——应用侧不再维护第二套 CPU 参数公式。
 * 异步语义：渲染序号防串台（慢请求返回后不覆盖新请求）；RPC 失败由调用方回退纯底图。
 */
async function renderViaEngine(
  canvas: HTMLCanvasElement,
  seqRef: { current: number },
  opts: { params: EditParams; split?: number | null; beforeParams?: EditParams | null },
): Promise<boolean> {
  const seq = ++seqRef.current;
  const st = useDevelop.getState();
  const inputPath = st.baseImageUrl ?? '';
  if (!inputPath) return false;
  const photoId = st.photoId ?? 0;
  const loadImg = async (p: EditParams): Promise<HTMLImageElement | null> => {
    const r = await api.develop.render({ photoId, inputPath, params: p });
    if (seq !== seqRef.current) return null;
    const im = new Image();
    im.src = fileUrl(r.path);
    await im.decode();
    if (seq !== seqRef.current) return null;
    return im;
  };
  try {
    const afterImg = await loadImg(opts.params);
    if (!afterImg) return true; // 已被更新的渲染请求取代（非失败）
    const w = afterImg.naturalWidth;
    const h = afterImg.naturalHeight;
    if (canvas.width !== w || canvas.height !== h) {
      canvas.width = w;
      canvas.height = h;
    }
    // CSS 尺寸与 GPU 路径同口径：量宿主容器 [data-viewer-host]，contain 适配
    const host = (canvas.closest('[data-viewer-host]') as HTMLElement | null) ?? canvas.parentElement;
    const hostRect = host ? host.getBoundingClientRect() : canvas.getBoundingClientRect();
    const availW = Math.max(64, hostRect.width || w);
    const availH = Math.max(64, hostRect.height || h * (availW / w));
    let cssW = availW;
    let cssH = cssW * (h / w);
    if (cssH > availH) {
      cssH = availH;
      cssW = cssH * (w / h);
    }
    canvas.style.width = `${Math.round(cssW)}px`;
    canvas.style.height = `${Math.round(cssH)}px`;
    const ctx = canvas.getContext('2d');
    if (!ctx) return false;
    ctx.clearRect(0, 0, w, h);
    ctx.drawImage(afterImg, 0, 0);
    if (opts.split !== null && opts.split !== undefined && opts.beforeParams) {
      const beforeImg = await loadImg(opts.beforeParams);
      if (!beforeImg) return true;
      ctx.save();
      ctx.beginPath();
      ctx.rect(0, 0, Math.round(w * opts.split), h);
      ctx.clip();
      ctx.drawImage(beforeImg, 0, 0);
      ctx.restore();
    }
    return true;
  } catch {
    return false;
  }
}

export default function ViewerCanvas({ onHistogram }: { onHistogram?: (h: { r: number[]; g: number[]; b: number[] }) => void }) {
  const photoId = useDevelop((s) => s.photoId);
  const params = useDevelop((s) => s.params);
  const beforeParams = useDevelop((s) => s.beforeParams);
  const baseImageUrl = useDevelop((s) => s.baseImageUrl);
  const baseImageReady = useDevelop((s) => s.baseImageReady);
  const selectedMaskId = useDevelop((s) => s.selectedMaskId);
  const maskTool = useDevelop((s) => s.maskTool);
  const appendStroke = useDevelop((s) => s.appendStroke);
  const selectedMask = params?.masks.find((m) => m.id === selectedMaskId) ?? null;
  const brushSelected = selectedMask?.type === 'brush';
  const painting = brushSelected && maskTool === 'brush';
  const loading = useDevelop((s) => s.loading);
  const [compare, setCompare] = useState(false);
  const [spaceHeld, setSpaceHeld] = useState(false);
  const [split, setSplit] = useState(0.5);
  const maskOverlayRef = useRef<HTMLCanvasElement>(null);
  const paintingRef = useRef(false);
  const lastPtRef = useRef<{ x: number; y: number } | null>(null);
  // 渲染降级可见化：曾经因 shader 编译失败整页黑屏且无任何提示（2026-10-22 修复）
  const [renderNotice, setRenderNotice] = useState<string | null>(null);
  const canvasRef = useRef<HTMLCanvasElement>(null);
  const rendererRef = useRef<DevelopRenderer | null>(null);
  const imgRef = useRef<HTMLImageElement | null>(null);
  /** 底图是否已成功解码为像素（失败时保留 2D 直出，不被空纹理覆盖） */
  const imageOkRef = useRef(false);
  const histTimer = useRef<ReturnType<typeof setTimeout> | null>(null);
  /** CPU 软渲染最近一次耗时（ms，诊断/冒烟用） */
  const cpuMsRef = useRef<number | null>(null);
  /** 引擎 CPU 渲染请求序号（防慢请求串台覆盖新渲染） */
  const engineRenderSeq = useRef(0);
  const logCpuOnce = (ms: number | null): void => {
    cpuMsRef.current = ms;
    if (ms !== null && window.lumina?.isSmoke) console.log(`[smoke-scenario] cpu-render: ms=${ms}`);
  };

  // '\' 前后对比（PRD §6.3）+ 长按空格临时看原图（专业模式）
  useEffect(() => {
    const onKey = (e: KeyboardEvent) => {
      const tag = (e.target as HTMLElement)?.tagName;
      if (tag === 'INPUT' || tag === 'TEXTAREA') return;
      if (e.key === '\\') setCompare((c) => !c);
      // batch 1 快捷键对齐：V = 前后对比（LR Y/Y 语境；与 \ 同效，写进速查表）
      if (e.key.toLowerCase() === 'v' && !e.ctrlKey && !e.metaKey && !e.altKey) setCompare((c) => !c);
      if (e.code === 'Space' && !e.repeat) {
        e.preventDefault();
        setSpaceHeld(true);
      }
    };
    const onKeyUp = (e: KeyboardEvent) => {
      if (e.code === 'Space') setSpaceHeld(false);
    };
    const onBlur = () => setSpaceHeld(false);
    window.addEventListener('keydown', onKey);
    window.addEventListener('keyup', onKeyUp);
    window.addEventListener('blur', onBlur);
    return () => {
      window.removeEventListener('keydown', onKey);
      window.removeEventListener('keyup', onKeyUp);
      window.removeEventListener('blur', onBlur);
    };
  }, []);

  // v1.3.0-m3 OptionBar 前后对比按钮：与 \ 键共用同一 state（事件来自上方选项栏）
  useEffect(() => {
    const onToggle = () => setCompare((c) => !c);
    window.addEventListener('lumina:compare-toggle', onToggle);
    return () => window.removeEventListener('lumina:compare-toggle', onToggle);
  }, []);

  // 加载底图
  useEffect(() => {
    if (!baseImageUrl) return;
    const img = new Image();
    img.src = fileUrl(baseImageUrl);
    img.onload = () => {
      imgRef.current = img;
      if (window.lumina?.isSmoke) {
        console.log(`[smoke-scenario] vc-onload: ${img.naturalWidth}x${img.naturalHeight} src=${img.src.split('/').pop()}`);
      }
      if (IS_BASELINE) {
        console.log(`[baseline] vc-onload: ${img.naturalWidth}x${img.naturalHeight} src=${img.src.split('/').pop()} canvas=${canvasRef.current ? 'mounted' : 'not-yet'}`);
      }
      const canvas = canvasRef.current;
      if (canvas) {
        const r = ensureRenderer(canvas);
        if (IS_BASELINE) {
          console.log(`[baseline] vc-ensure: ${r ? 'gpu-ok' : 'gpu-fail→cpu'}`);
        }
        if (r) {
          // v1.4.1：setImage 的返回值曾被调用方丢弃 —— 底图解码不成像素时
          // （Mock 会话底图是矢量 SVG / 文件损坏 / 跨域受限）整块画布静默全黑，
          // 既无提示也无回退。现在失败必须可见：先告知，再 2D 直出原图。
          const okImg = r.setImage(img);
          imageOkRef.current = okImg;
          if (IS_BASELINE) console.log(`[baseline] vc-setimage: ${okImg ? 'ok' : 'fail→2d'}`);
          if (okImg) {
            setRenderNotice(null);
            // 冒烟证据：着色器编译告警必须为空（红线 R1：编译期常量索引问题曾整屏黑）
            if (window.lumina?.isSmoke) {
              console.log(`[smoke-scenario] shader: warnings=${r.warnings.length > 0 ? r.warnings : 'none'}`);
            }
          } else {
            console.warn(`[viewer] 底图解码失败，已 2D 直出原图（参数不生效）：src=${img.src.split('/').pop()} ${img.naturalWidth}x${img.naturalHeight}`);
            setRenderNotice('底图无法解码为像素，已直出原图（参数调整暂不生效）');
            drawFallback2d(canvas, img);
          }
        } else {
          // 无 WebGL：引擎 Plan CPU 渲染保底（与导出同一实现，T1.5），保证不黑屏
          const p = useDevelop.getState().params;
          if (p && canvas) {
            void renderViaEngine(canvas, engineRenderSeq, { params: p }).then((ok) => {
              cpuMsRef.current = ok ? 0 : null;
              logCpuOnce(cpuMsRef.current);
              if (!ok) drawFallback2d(canvas, img);
            });
          } else if (canvas) {
            drawFallback2d(canvas, img);
          }
        }
      }
      // 触发一次渲染
      setRenderTick((t) => t + 1);
    };
    img.onerror = () => {
      if (IS_BASELINE) console.log(`[baseline] vc-onerror: ${img.src.split('/').pop()}`);
      useJobStore.getState().toast('底图加载失败（引擎产物缺失）', 'error', 6000);
    };
  }, [baseImageUrl]);

  // 参数变化 → 重渲染（拖动中间态零延迟）
  const [renderTick, setRenderTick] = useState(0);
  const effectiveCompare = compare || spaceHeld;
  // 对比选中态广播（OptionBar 按钮高亮与真实 state 同源）
  useEffect(() => {
    window.dispatchEvent(new CustomEvent('lumina:compare-state', { detail: { on: effectiveCompare } }));
  }, [effectiveCompare]);
  useEffect(() => {
    const canvas = canvasRef.current;
    const r = rendererRef.current;
    if (!canvas || !params || !imgRef.current) return;
    // 底图解码失败且已 2D 直出：不要再用空纹理把它刷黑（v1.4.1）
    if (r && !imageOkRef.current) return;
    if (r) {
      const ok = r.render(canvas, {
        params,
        split: effectiveCompare ? split : null,
        beforeParams: beforeParams ?? undefined,
      });
      if (!ok) {
        // GPU 渲染失败：降级引擎 Plan CPU 渲染并显式告知（不允许静默黑屏）
        setRenderNotice('软件渲染模式：参数已生效（引擎 Plan 求值，与导出同实现）');
        void renderViaEngine(canvas, engineRenderSeq, {
          params,
          split: effectiveCompare ? split : null,
          beforeParams: beforeParams ?? undefined,
        }).then((ok2) => {
          cpuMsRef.current = ok2 ? 0 : null;
          logCpuOnce(cpuMsRef.current);
        });
      } else {
        setRenderNotice(null);
        // 直方图 150ms 防抖（架构 §8 性能要点）
        if (histTimer.current) clearTimeout(histTimer.current);
        histTimer.current = setTimeout(() => {
          const h = r.sampleHistogram(canvas);
          if (h && onHistogram) onHistogram(h);
        }, 150);
      }
    } else {
      // 无 WebGL：引擎 Plan CPU 渲染（与导出同一实现，T1.5）
      void renderViaEngine(canvas, engineRenderSeq, {
        params,
        split: effectiveCompare ? split : null,
        beforeParams: beforeParams ?? undefined,
      }).then((ok) => {
        cpuMsRef.current = ok ? 0 : null;
        logCpuOnce(cpuMsRef.current);
        if (!ok && imgRef.current) drawFallback2d(canvas, imgRef.current);
      });
    }
  }, [params, effectiveCompare, split, beforeParams, renderTick, onHistogram]);

  // 画笔蒙版红色浮层（WP-B5）：与 GPU/CPU 共用同一个 buildBrushField 因子场，
  // 浮层边界与实际渲染边界一致靠"同一份数据"保证，不是两套近似。
  const overlayStrokes = brushSelected ? selectedMask!.strokes ?? [] : null;
  useEffect(() => {
    const oc = maskOverlayRef.current;
    if (!oc) return;
    const ctx = oc.getContext('2d');
    if (!ctx) return;
    if (!overlayStrokes || overlayStrokes.length === 0) {
      ctx.clearRect(0, 0, oc.width, oc.height);
      return;
    }
    const field = buildBrushField(overlayStrokes);
    const off = document.createElement('canvas');
    off.width = FIELD_CELL;
    off.height = FIELD_CELL;
    const octx = off.getContext('2d');
    if (!octx) return;
    const imgData = octx.createImageData(FIELD_CELL, FIELD_CELL);
    for (let i = 0; i < field.length; i++) {
      imgData.data[i * 4] = 255;
      imgData.data[i * 4 + 1] = 40;
      imgData.data[i * 4 + 2] = 40;
      imgData.data[i * 4 + 3] = Math.round(field[i] * 110);
    }
    octx.putImageData(imgData, 0, 0);
    ctx.clearRect(0, 0, oc.width, oc.height);
    ctx.imageSmoothingEnabled = true;
    ctx.drawImage(off, 0, 0, oc.width, oc.height);
  }, [overlayStrokes, brushSelected]);

  // 画笔落点：拖动中 commit=false 不刷历史，抬笔补一个同点印记并 commit=true 记一次「画笔」
  const uvFromEvent = (e: React.PointerEvent): { x: number; y: number } | null => {
    const el = maskOverlayRef.current;
    if (!el) return null;
    const rect = el.getBoundingClientRect();
    if (rect.width === 0 || rect.height === 0) return null;
    return {
      x: Math.max(0, Math.min(1, (e.clientX - rect.left) / rect.width)),
      y: Math.max(0, Math.min(1, (e.clientY - rect.top) / rect.height)),
    };
  };
  const strokeRadius = brushSelected ? selectedMask!.strokes?.[selectedMask!.strokes.length - 1]?.radius ?? 0.08 : 0.08;
  const makeStroke = (uv: { x: number; y: number }, erase: boolean) => ({
    x: uv.x,
    y: uv.y,
    radius: strokeRadius,
    flow: 80,
    density: 100,
    erase,
  });
  const onOverlayPointerDown = (e: React.PointerEvent): void => {
    if (!painting || !selectedMask || e.button !== 0) return;
    e.preventDefault();
    (e.target as HTMLElement).setPointerCapture(e.pointerId);
    const uv = uvFromEvent(e);
    if (!uv) return;
    paintingRef.current = true;
    lastPtRef.current = uv;
    void appendStroke(selectedMask.id, makeStroke(uv, e.altKey), false);
  };
  const onOverlayPointerMove = (e: React.PointerEvent): void => {
    if (!paintingRef.current || !selectedMask) return;
    const uv = uvFromEvent(e);
    const last = lastPtRef.current;
    if (!uv || !last) return;
    if (Math.hypot(uv.x - last.x, uv.y - last.y) < 0.004) return;
    lastPtRef.current = uv;
    void appendStroke(selectedMask.id, makeStroke(uv, e.altKey), false);
  };
  const endPainting = (e: React.PointerEvent): void => {
    if (!paintingRef.current || !selectedMask) return;
    paintingRef.current = false;
    lastPtRef.current = null;
    const uv = uvFromEvent(e) ?? { x: 0, y: 0 };
    void appendStroke(selectedMask.id, makeStroke(uv, e.altKey), true);
  };

  function ensureRenderer(canvas: HTMLCanvasElement): DevelopRenderer | null {
    if (!rendererRef.current) {
      const r = new DevelopRenderer();
      if (r.init(canvas)) {
        rendererRef.current = r;
      } else {
        setRenderNotice('软件渲染模式：参数已生效（部分高级项除外）');
        useJobStore.getState().toast('WebGL 不可用，修图预览进入软件渲染模式（基础参数仍生效）', 'warn', 8000);
        return null;
      }
    }
    return rendererRef.current;
  }

  // baseImageReady / loading 状态显示
  void baseImageReady;
  void photoId;

  // 零静默：曲线若读回"全零"（引擎 parseCurve 把 "0, 0 255, 255" 按空格切散，y 恒为 0），
  // 预览必然全黑。这是引擎侧已知缺陷，UI 不能装作正常，也不能偷偷替换用户数据。
  const curveAllZero =
    !!params &&
    Array.isArray(params.toneCurveRGB) &&
    params.toneCurveRGB.length >= 2 &&
    params.toneCurveRGB.every((pt) => Number(pt.y) === 0);

  return (
    <ViewerSurface className="relative flex-1 min-h-0 flex items-center justify-center overflow-hidden" style={{ padding: 24 }}>
      {/* data-viewer-host：渲染器量**这个容器**来算 contain 尺寸。
          旧实现量 canvas 自身 → CSS 盒与缓冲尺寸互相推导，永远卡在默认 300×150。 */}
      <div data-viewer-host className="relative flex-1 min-h-0 w-full h-full flex items-center justify-center">
        <div className="relative">
          <canvas
            ref={canvasRef}
            data-viewer-canvas="true"
            className="block rounded-xlC cursor-col-resize"
            style={{ display: loading ? 'none' : 'block' }}
            onPointerDown={(e) => {
              if (!effectiveCompare) return;
              const rect = (e.target as HTMLCanvasElement).getBoundingClientRect();
              const move = (ev: PointerEvent) => {
                setSplit(Math.max(0.02, Math.min(0.98, (ev.clientX - rect.left) / rect.width)));
              };
              const up = () => {
                window.removeEventListener('pointermove', move);
                window.removeEventListener('pointerup', up);
              };
              window.addEventListener('pointermove', move);
              window.addEventListener('pointerup', up);
              move(e.nativeEvent);
            }}
          />
          {/* 画笔蒙版浮层：红色 = 因子>0 区域，软边 = 因子衰减（与渲染同一场数据） */}
          <canvas
            ref={maskOverlayRef}
            width={FIELD_CELL}
            height={FIELD_CELL}
            className="absolute inset-0 w-full h-full rounded-md"
            style={{
              cursor: painting ? 'crosshair' : 'default',
              pointerEvents: painting ? 'auto' : 'none',
              touchAction: 'none',
              opacity: brushSelected ? 1 : 0,
            }}
            onPointerDown={onOverlayPointerDown}
            onPointerMove={onOverlayPointerMove}
            onPointerUp={endPainting}
            onPointerCancel={endPainting}
          />
        </div>
      </div>
      {loading && (
        <div className="absolute inset-0 flex items-center justify-center text-txt3 text-xs">解码底图…</div>
      )}
      {!loading && curveAllZero && <CurveAllZeroBanner />}

      {!loading && renderNotice && (
        <div
          className="absolute top-3 left-3 right-3 px-3 py-1.5 rounded-md text-2xs"
          style={{ background: 'rgba(185,138,46,0.14)', border: '1px solid rgba(185,138,46,0.5)', color: 'var(--Lumina-Tokens-accent)' }}
        >
          {renderNotice}
        </div>
      )}
      {effectiveCompare && !loading && (
        <>
          <div className="absolute top-3 left-1/2 -translate-x-1/2 px-2 py-1 rounded-md text-2xs" style={{ background: 'rgba(12,13,16,0.75)', color: 'var(--Lumina-Tokens-textSecondary)' }}>
            {spaceHeld && !compare ? '原图（松开空格恢复）' : '前 · Before｜拖动分割线对比｜按 \\ 关闭'}
          </div>
          <div className="absolute top-0 bottom-0 w-px bg-accent/70 pointer-events-none" style={{ left: `calc(${split * 100}% )` }} />
        </>
      )}
      {!loading && baseImageUrl && (
        <div className="absolute bottom-3 left-3 px-2 py-1 rounded-md text-2xs flex items-center gap-1.5" style={{ background: 'rgba(12,13,16,0.75)', color: 'var(--Lumina-Tokens-textSecondary)' }}>
          <Icon name="pin" size={10} />
          {baseImageUrl.split('/').pop()}
        </div>
      )}
    </ViewerSurface>
  );
}

/**
 * 曲线全零红条：一行摘要 + 「详情」展开两行说明（引擎已知缺陷，零静默原则）。
 * 样式仅用 danger 令牌与既有 rgba 蒙层口径，不引入新色值。
 */
function CurveAllZeroBanner(): React.JSX.Element {
  const [expanded, setExpanded] = useState(false);
  const bannerStyle: React.CSSProperties = {
    background: 'rgba(217,96,79,0.14)',
    border: '1px solid rgba(217,96,79,0.5)',
    color: 'var(--Lumina-Tokens-danger)',
  };
  return (
    <div className="absolute top-3 left-3 right-3 px-3 py-1.5 rounded-md text-2xs z-10" style={bannerStyle}>
      <div className="flex items-center gap-2">
        <span className="flex-1">曲线读回全零，预览为黑（引擎已知缺陷）</span>
        <button
          type="button"
          onClick={() => setExpanded((v) => !v)}
          className="shrink-0 underline underline-offset-2 cursor-pointer"
          style={{ color: 'var(--Lumina-Tokens-danger)' }}
        >
          {expanded ? '收起' : '详情'}
        </button>
      </div>
      {expanded && (
        <div className="mt-1 leading-relaxed opacity-90">
          <div>· XMP 曲线解析缺陷：`0, 0 255, 255` 被解析成 y=0</div>
          <div>· 未修改你的 sidecar，修复随 v1.2.0 M0 引擎交付</div>
        </div>
      )}
    </div>
  );
}

