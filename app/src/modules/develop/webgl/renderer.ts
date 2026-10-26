/**
 * WebGL 参数渲染器（T1.4：93 叶统一管线，架构 §2.2 develop/webgl/renderer.ts）。
 *
 * 底图 = preview.getBase 路径；参数变更 → GPU 多 pass 重渲染（不重新解码）。
 * 管线 = ops/pipeline.ts（log16 float FBO 多 pass，与引擎 CPU oracle 逐叶 parity）；
 * 旧单 pass 近似 shader 已退役（T1.5 将删除三套旧实现）。
 *
 * 保留的既有修复（不回退）：
 *  - canvas 尺寸必须量宿主容器 [data-viewer-host]（ui-phase2-a 修的自引用死锁）；
 *  - 曲线 LUT 纹理绑定顺序（管线内部先 LUT 上传后底图，单元 1/0 分离）；
 *  - 画笔蒙版距离场图集 WP-B5 上传路径与键控。
 */

import { PipelineGL } from './ops/pipeline';
import { buildCurveLutData } from './ops/oracle93';
import { log16Encode, srgbToLinear } from './ops/log16';
import { buildFieldAtlas, FIELD_COLS, FIELD_ROWS } from './maskField';
import type { CurvePoint, EditParams, MaskLayer } from '../../../api/protocol';

export interface RenderOptions {
  params: EditParams;
  /** 前后对比：分割位置 0..1；null = 不对比 */
  split?: number | null;
  beforeParams?: EditParams | null;
}

const MAX_MASKS = 6;

/** 预览底图最长边（性能预算：1600px，与 CPU 保底路径一致） */
const MAX_PREVIEW = 1600;

function maskGeo(m: MaskLayer): { a: [number, number, number, number]; b: [number, number, number, number] } {
  if (m.type === 'linear' && m.geometry && 'x1' in m.geometry) {
    return { a: [m.geometry.x1, 1 - m.geometry.y1, 0, 0], b: [m.geometry.x2, 1 - m.geometry.y2, 0, 0] };
  }
  if (m.type === 'radial' && m.geometry && 'cx' in m.geometry) {
    return { a: [m.geometry.cx, 1 - m.geometry.cy, 0, 0], b: [0, 0, m.geometry.radius, m.geometry.ratio] };
  }
  // brush：以笔画质心 + 平均半径近似
  const strokes = m.strokes ?? [];
  if (strokes.length > 0) {
    const cx = strokes.reduce((s, k) => s + k.x, 0) / strokes.length;
    const cy = strokes.reduce((s, k) => s + k.y, 0) / strokes.length;
    const r = strokes.reduce((s, k) => s + k.radius, 0) / strokes.length;
    return { a: [cx, 1 - cy, 0, 0], b: [0, 0, r * 2, 1] };
  }
  return { a: [0.5, 0.5, 0, 0], b: [0.5, 0.5, 0.001, 1] };
}

/** sRGB 画布像素 → log16 码 t 域（预览底图统一进管线输入域）。
 *  T1.5：导出=预览断言（smoke）复用同一函数，保证两端进域口径单一来源。 */
export function imageToCodes(img: HTMLImageElement): { t: Float32Array; w: number; h: number } | null {
  const k = Math.min(1, MAX_PREVIEW / Math.max(img.naturalWidth, img.naturalHeight));
  const w = Math.max(1, Math.round(img.naturalWidth * k));
  const h = Math.max(1, Math.round(img.naturalHeight * k));
  const cv = document.createElement('canvas');
  cv.width = w;
  cv.height = h;
  const ctx = cv.getContext('2d', { willReadFrequently: true });
  if (!ctx) return null;
  ctx.drawImage(img, 0, 0, w, h);
  const data = ctx.getImageData(0, 0, w, h).data;
  const t = new Float32Array(w * h * 3);
  for (let i = 0, j = 0; i < w * h; ++i, j += 3) {
    // 显示 sRGB → 线性 → log16 码（近似 raw 解码域；导出像素管线仍由引擎负责）
    t[j] = log16Encode(srgbToLinear(data[i * 4] / 255)) / 65535;
    t[j + 1] = log16Encode(srgbToLinear(data[i * 4 + 1] / 255)) / 65535;
    t[j + 2] = log16Encode(srgbToLinear(data[i * 4 + 2] / 255)) / 65535;
  }
  return { t, w, h };
}

/** 4 条曲线 → LUT 纹理数据 + 恒等标志（与 parity/oracle 共用 builder，口径一致） */
function buildCurveLut(p: EditParams): { data: Float32Array; skip: boolean[] } {
  return buildCurveLutData(p);
}

export class DevelopRenderer {
  private gl: WebGLRenderingContext | null = null;
  private pipeline = new PipelineGL();
  private imageWidth = 0;
  private imageHeight = 0;
  private shaderWarnings = '';
  private fieldKey = '';

  init(canvas: HTMLCanvasElement): boolean {
    // WP-B6 验证通道：主进程以 ?nowebgl=1 告知渲染层禁用 WebGL（LUMINA_NO_WEBGL=1）
    if (new URLSearchParams(window.location.search).get('nowebgl') === '1') return false;
    const gl = (canvas.getContext('webgl', { preserveDrawingBuffer: true, antialias: false }) ??
      canvas.getContext('experimental-webgl')) as WebGLRenderingContext | null;
    if (!gl) return false;
    if (!this.pipeline.init(gl)) return false;
    this.gl = gl;
    this.shaderWarnings = this.pipeline.warnings;
    if (this.shaderWarnings) console.warn('[develop] shader 编译告警:', this.shaderWarnings);
    return true;
  }

  /** 着色器编译告警原文（空串=零告警；冒烟据此出证据行） */
  get warnings(): string {
    return this.shaderWarnings;
  }

  get ready(): boolean {
    return this.gl !== null && this.pipeline.ready;
  }

  get aspect(): number {
    return this.imageWidth > 0 ? this.imageWidth / this.imageHeight : 1.5;
  }

  setImage(img: HTMLImageElement): boolean {
    if (!this.gl) return false;
    this.imageWidth = img.naturalWidth;
    this.imageHeight = img.naturalHeight;
    const codes = imageToCodes(img);
    if (!codes) return false;
    return this.pipeline.setSourceCodes(codes.t, codes.w, codes.h);
  }

  render(canvas: HTMLCanvasElement, opts: RenderOptions): boolean {
    const { params, split = null, beforeParams = null } = opts;
    if (!this.gl || !this.pipeline.ready) return false;

    // CSS 尺寸仍量宿主容器（既有修复：不量 canvas 自己）；缓冲尺寸由管线输出决定
    const host = (canvas.closest('[data-viewer-host]') as HTMLElement | null) ?? canvas.parentElement;
    const hostRect = host ? host.getBoundingClientRect() : canvas.getBoundingClientRect();
    const availW = Math.max(64, hostRect.width);
    const availH = Math.max(64, hostRect.height);
    const aspect = this.aspect > 0 ? this.aspect : 1.5;
    let cssW = availW;
    let cssH = cssW / aspect;
    if (cssH > availH) {
      cssH = availH;
      cssW = cssH * aspect;
    }
    const cssWpx = `${Math.round(cssW)}px`;
    const cssHpx = `${Math.round(cssH)}px`;
    if (canvas.style.width !== cssWpx) canvas.style.width = cssWpx;
    if (canvas.style.height !== cssHpx) canvas.style.height = cssHpx;

    // 曲线 LUT（RGBA = RGB复合/R/G/B；先于管线绘制上传，单元绑定由管线内部分离）
    const curve = buildCurveLut(params);
    this.pipeline.setCurves(curve.data, curve.skip);

    // 画笔蒙版距离场图集（WP-B5）：仅笔画变化时重建上传
    const fieldKey = params.masks
      .filter((m) => m.type === 'brush')
      .map((m) => `${m.id}:${m.enabled ? 1 : 0}:${JSON.stringify(m.strokes ?? [])}`)
      .join('|');
    if (fieldKey !== this.fieldKey) {
      this.fieldKey = fieldKey;
      const { bytes } = buildFieldAtlas(params.masks);
      this.pipeline.setFieldAtlas(bytes, FIELD_COLS, FIELD_ROWS);
    }

    // 蒙版 uniforms（最多 6 个，超出截断；蒙版语义 T2.2 前为既有近似）
    const masks = params.masks.filter((m) => m.enabled).slice(0, MAX_MASKS);
    const geoA = new Float32Array(MAX_MASKS * 4);
    const geoB = new Float32Array(MAX_MASKS * 4);
    const local0 = new Float32Array(MAX_MASKS * 4);
    const local1 = new Float32Array(MAX_MASKS * 4);
    const amount = new Float32Array(MAX_MASKS);
    const type = new Float32Array(MAX_MASKS);
    const opacity = new Float32Array(MAX_MASKS);
    masks.forEach((m, i) => {
      const g = maskGeo(m);
      geoA.set(g.a, i * 4);
      geoB.set(g.b, i * 4);
      local0.set([m.local.exposure, m.local.contrast, m.local.highlights, m.local.shadows], i * 4);
      local1.set([m.local.temperature, m.local.tint, m.local.saturation, m.local.clarity], i * 4);
      amount[i] = m.amount ?? 100;
      type[i] = m.type === 'linear' ? 0 : m.type === 'radial' ? 1 : 2;
      opacity[i] = m.opacity;
    });

    return this.pipeline.render(canvas, params, beforeParams, split, {
      count: masks.length,
      geoA,
      geoB,
      local0,
      local1,
      amount,
      type,
      opacity,
    });
  }

  /** 直方图采样（150ms 防抖由调用方控制）：从渲染结果低分辨率采样 64×64 */
  sampleHistogram(canvas: HTMLCanvasElement): { r: number[]; g: number[]; b: number[] } | null {
    const tmp = document.createElement('canvas');
    tmp.width = 64;
    tmp.height = 64;
    const ctx = tmp.getContext('2d');
    if (!ctx) return null;
    ctx.drawImage(canvas, 0, 0, 64, 64);
    const data = ctx.getImageData(0, 0, 64, 64).data;
    const r = new Array<number>(256).fill(0);
    const g = new Array<number>(256).fill(0);
    const b = new Array<number>(256).fill(0);
    for (let i = 0; i < data.length; i += 4) {
      r[data[i]]++;
      g[data[i + 1]]++;
      b[data[i + 2]]++;
    }
    return { r, g, b };
  }
}
