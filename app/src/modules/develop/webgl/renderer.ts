/**
 * WebGL 参数近似渲染器（架构 §2.2 develop/webgl/renderer.ts）。
 * 底图 = preview.getBase 返回路径；参数变更 → GPU 逐参数重渲染（不重新解码）。
 * 前后对比：uSplit 分割线，左侧用会话初始参数。
 */

import { FRAG_SRC, VERT_SRC } from './shaders';
import { buildFieldAtlas, FIELD_COLS, FIELD_ROWS } from './maskField';
import type { EditParams, Hsl, HslBand, MaskLayer } from '../../../api/protocol';

export interface RenderOptions {
  params: EditParams;
  /** 前后对比：分割位置 0..1；null = 不对比 */
  split?: number | null;
  beforeParams?: EditParams | null;
}

const MAX_MASKS = 6;

/** HSL 8 色带固定顺序（与引擎 kBandCenterHue 0/30/60/120/180/240/280/320° 一一对应） */
const HSL_ORDER: HslBand[] = ['red', 'orange', 'yellow', 'green', 'aqua', 'blue', 'purple', 'magenta'];

/**
 * 8 个色带 → 两个 mat4（列 = 一个色带的 (hue, sat, lum, 0)）。
 * 着色器只用字面量列索引，规避 GLSL ES 1.0 的「常量索引表达式」限制。
 */
function hslMatrices(hsl: Record<HslBand, Hsl> | undefined): [Float32Array, Float32Array] {
  const m0 = new Float32Array(16);
  const m1 = new Float32Array(16);
  HSL_ORDER.forEach((band, i) => {
    const b = hsl?.[band];
    const col = [Number(b?.hue ?? 0), Number(b?.sat ?? 0), Number(b?.lum ?? 0), 0];
    const target = i < 4 ? m0 : m1;
    target.set(col, (i % 4) * 4);
  });
  return [m0, m1];
}
/** 参数向量索引 */
const IDX: Record<string, number> = {
  exposure: 0,
  contrast: 1,
  highlights: 2,
  shadows: 3,
  whites: 4,
  blacks: 5,
  temperature: 6,
  tint: 7,
  vibrance: 8,
  saturation: 9,
  clarity: 10,
  dehaze: 11,
};

function paramVector(p: EditParams): Float32Array {
  const v = new Float32Array(12);
  v[IDX.exposure] = p.exposure;
  v[IDX.contrast] = p.contrast;
  v[IDX.highlights] = p.highlights;
  v[IDX.shadows] = p.shadows;
  v[IDX.whites] = p.whites;
  v[IDX.blacks] = p.blacks;
  v[IDX.temperature] = p.whiteBalance === 'custom' ? ((p.temperature ?? 5500) - 5500) / 25 : 0;
  v[IDX.tint] = p.whiteBalance === 'custom' ? (p.tint ?? 0) / 2 : 0;
  v[IDX.vibrance] = p.vibrance;
  v[IDX.saturation] = p.saturation;
  v[IDX.clarity] = p.clarity;
  v[IDX.dehaze] = p.dehaze;
  return v;
}

/** 色调曲线 → 256 LUT（复合 RGB 曲线；CPU 计算线性插值）
 *  防御：引擎 beginSession / undo / redo 回传的 params 可能缺 toneCurveRGB（实测只回 14~17 个键），
 *  缺失时直接返回恒等 LUT —— 曾经这里 `[...undefined]` 抛 "points is not iterable"，
 *  整棵 React 树被打穿、窗口全黑无提示（2026-10-22 定位）。 */
function buildCurveLut(points: { x: number; y: number }[] | undefined): Uint8Array {
  const lut = new Uint8Array(256);
  for (let i = 0; i < 256; i++) lut[i] = i;
  if (!Array.isArray(points) || points.length < 2) return lut;
  const pts = [...points].sort((a, b) => a.x - b.x);
  const at = (i: number) => pts[Math.max(0, Math.min(pts.length - 1, i))];
  for (let i = 0; i < 256; i++) {
    let y = i;
    let lo = 0;
    while (lo < pts.length && pts[lo].x <= i) lo++;
    if (lo === 0) y = at(0).y;
    else if (lo >= pts.length) y = at(pts.length - 1).y;
    else {
      const a = pts[lo - 1];
      const b = pts[lo];
      const t = (i - a.x) / Math.max(1, b.x - a.x);
      y = a.y + (b.y - a.y) * t;
    }
    lut[i] = Math.max(0, Math.min(255, Math.round(y)));
  }
  return lut;
}

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

export class DevelopRenderer {
  private gl: WebGLRenderingContext | null = null;
  private program: WebGLProgram | null = null;
  private tex: WebGLTexture | null = null;
  private curveTex: WebGLTexture | null = null;
  /** 画笔蒙版距离场图集（WP-B5）：3x2 格 LUMINANCE，仅在笔画变化时重建 */
  private fieldTex: WebGLTexture | null = null;
  private fieldKey = '';
  private uniforms: Record<string, WebGLUniformLocation | null> = {};
  private imageWidth = 0;
  private imageHeight = 0;
  private shaderWarnings = '';

  init(canvas: HTMLCanvasElement): boolean {
    // WP-B6 验证通道：主进程以 ?nowebgl=1 告知渲染层禁用 WebGL（LUMINA_NO_WEBGL=1）
    if (new URLSearchParams(window.location.search).get('nowebgl') === '1') return false;
    const gl = (canvas.getContext('webgl', { preserveDrawingBuffer: true, antialias: false }) ?? canvas.getContext('experimental-webgl')) as WebGLRenderingContext | null;
    if (!gl) return false;
    this.gl = gl;

    const compile = (type: number, src: string): WebGLShader | null => {
      const sh = gl.createShader(type);
      if (!sh) return null;
      gl.shaderSource(sh, src);
      gl.compileShader(sh);
      if (!gl.getShaderParameter(sh, gl.COMPILE_STATUS)) {
        console.warn('[develop] shader 编译失败:', gl.getShaderInfoLog(sh));
        return null;
      }
      return sh;
    };
    const vs = compile(gl.VERTEX_SHADER, VERT_SRC);
    const fs = compile(gl.FRAGMENT_SHADER, FRAG_SRC);
    if (!vs || !fs) return false;
    const prog = gl.createProgram();
    if (!prog) return false;
    gl.attachShader(prog, vs);
    gl.attachShader(prog, fs);
    gl.linkProgram(prog);
    if (!gl.getProgramParameter(prog, gl.LINK_STATUS)) return false;
    this.program = prog;
    gl.useProgram(prog);

    // 全屏 quad
    const buf = gl.createBuffer();
    gl.bindBuffer(gl.ARRAY_BUFFER, buf);
    gl.bufferData(gl.ARRAY_BUFFER, new Float32Array([-1, -1, 1, -1, -1, 1, 1, 1]), gl.STATIC_DRAW);
    const loc = gl.getAttribLocation(prog, 'aPos');
    gl.enableVertexAttribArray(loc);
    gl.vertexAttribPointer(loc, 2, gl.FLOAT, false, 0, 0);

    for (const name of ['uImage', 'uCurve', 'uFlip', 'pA[0]', 'pB[0]', 'uSplit', 'uVignette', 'uGrain', 'uMaskCount', 'uMaskField', 'uMaskGeoA[0]', 'uMaskGeoB[0]', 'uMaskLocal0[0]', 'uMaskLocal1[0]', 'uMaskAmount[0]', 'uMaskType[0]', 'uMaskOpacity[0]', 'uHslA0', 'uHslA1', 'uHslB0', 'uHslB1']) {
      this.uniforms[name] = gl.getUniformLocation(prog, name);
    }
    // 编译告警要看得见：shader 编译期曾因常量索引问题整屏黑过（红线 R1），
    // 这里把 infoLog 原样打出来，空字符串才算干净。
    const fsLog = (gl.getShaderInfoLog(fs) ?? '').trim();
    const vsLog = (gl.getShaderInfoLog(vs) ?? '').trim();
    this.shaderWarnings = [vsLog, fsLog].filter((s) => s.length > 0).join(' | ');
    if (this.shaderWarnings) console.warn('[develop] shader 编译告警:', this.shaderWarnings);
    return true;
  }

  /** 着色器编译告警原文（空串=零告警；冒烟据此出证据行） */
  get warnings(): string {
    return this.shaderWarnings;
  }

  get ready(): boolean {
    return this.gl !== null && this.tex !== null;
  }

  get aspect(): number {
    return this.imageWidth > 0 ? this.imageWidth / this.imageHeight : 1.5;
  }

  setImage(img: HTMLImageElement): boolean {
    const gl = this.gl;
    if (!gl) return false;
    this.imageWidth = img.naturalWidth;
    this.imageHeight = img.naturalHeight;
    if (this.tex) gl.deleteTexture(this.tex);
    this.tex = gl.createTexture();
    gl.bindTexture(gl.TEXTURE_2D, this.tex);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_S, gl.CLAMP_TO_EDGE);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_T, gl.CLAMP_TO_EDGE);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MIN_FILTER, gl.LINEAR);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MAG_FILTER, gl.LINEAR);
    gl.texImage2D(gl.TEXTURE_2D, 0, gl.RGBA, gl.RGBA, gl.UNSIGNED_BYTE, img);
    return true;
  }

  private ensureCurveTexture(lut: Uint8Array): void {
    const gl = this.gl;
    if (!gl) return;
    if (this.curveTex) gl.deleteTexture(this.curveTex);
    this.curveTex = gl.createTexture();
    gl.bindTexture(gl.TEXTURE_2D, this.curveTex);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_S, gl.CLAMP_TO_EDGE);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_T, gl.CLAMP_TO_EDGE);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MIN_FILTER, gl.LINEAR);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MAG_FILTER, gl.LINEAR);
    const data = new Uint8Array(256 * 4);
    for (let i = 0; i < 256; i++) {
      data[i * 4] = lut[i];
      data[i * 4 + 1] = lut[i];
      data[i * 4 + 2] = lut[i];
      data[i * 4 + 3] = 255;
    }
    gl.texImage2D(gl.TEXTURE_2D, 0, gl.RGBA, 256, 1, 0, gl.RGBA, gl.UNSIGNED_BYTE, data);
  }

  render(canvas: HTMLCanvasElement, opts: RenderOptions): boolean {
    const gl = this.gl;
    if (!gl || !this.tex || !this.program) return false;
    const { params, split = null, beforeParams = null } = opts;

    // canvas 尺寸：**必须量宿主容器**，不能量 canvas 自己。
    // 旧实现用 canvas.getBoundingClientRect()：canvas 的 CSS 盒由 width/height 属性推导，
    // 属性又由 CSS 盒推导 → 自引用死锁，永远停在默认 300×150（C1 基线裁到 px=0,0,0 的诱因之一）。
    // 现在：宿主 rect → contain 适配 → 同时设 CSS 尺寸（可见大小）与缓冲尺寸（×dpr）。
    const host = (canvas.closest('[data-viewer-host]') as HTMLElement | null) ?? canvas.parentElement;
    const hostRect = host ? host.getBoundingClientRect() : canvas.getBoundingClientRect();
    const dpr = Math.min(window.devicePixelRatio || 1, 1.5);
    const availW = Math.max(64, hostRect.width);
    const availH = Math.max(64, hostRect.height);
    const aspect = this.aspect > 0 ? this.aspect : 1.5;
    let cssW = availW;
    let cssH = cssW / aspect;
    if (cssH > availH) {
      cssH = availH;
      cssW = cssH * aspect;
    }
    const w = Math.max(64, Math.round(cssW * dpr));
    const h = Math.max(64, Math.round(cssH * dpr));
    if (canvas.width !== w || canvas.height !== h) {
      canvas.width = w;
      canvas.height = h;
    }
    const cssWpx = `${Math.round(cssW)}px`;
    const cssHpx = `${Math.round(cssH)}px`;
    if (canvas.style.width !== cssWpx) canvas.style.width = cssWpx;
    if (canvas.style.height !== cssHpx) canvas.style.height = cssHpx;
    gl.viewport(0, 0, w, h);
    gl.useProgram(this.program);

    // 纹理单元绑定顺序（P0，2026-10-22 HSL 冒烟暴露）：必须先把曲线 LUT 绑到单元 1，
    // 再把底图绑回单元 0。ensureCurveTexture 内部 bindTexture 作用于「当前活跃单元」，
    // 旧顺序先绑底图到 0 再传 LUT，导致单元 0 被 curveTex 覆盖 —— uImage 实际采样的是
    // 256 级灰阶 LUT，预览永远是一条灰渐变（参数效果全部不可见，HSL 色带测试 d=0 暴露）。
    this.ensureCurveTexture(buildCurveLut(params.toneCurveRGB));
    gl.activeTexture(gl.TEXTURE1);
    gl.bindTexture(gl.TEXTURE_2D, this.curveTex);
    gl.uniform1i(this.uniforms['uCurve']!, 1);

    gl.activeTexture(gl.TEXTURE0);
    gl.bindTexture(gl.TEXTURE_2D, this.tex);
    gl.uniform1i(this.uniforms['uImage']!, 0);

    // 画笔蒙版距离场图集（WP-B5）：768x512 LUMINANCE，仅笔画变化时重建上传
    const fieldKey = params.masks
      .filter((m) => m.type === 'brush')
      .map((m) => `${m.id}:${m.enabled ? 1 : 0}:${JSON.stringify(m.strokes ?? [])}`)
      .join('|');
    if (!this.fieldTex || fieldKey !== this.fieldKey) {
      this.fieldKey = fieldKey;
      if (!this.fieldTex) this.fieldTex = gl.createTexture();
      const { bytes } = buildFieldAtlas(params.masks);
      gl.activeTexture(gl.TEXTURE2);
      gl.bindTexture(gl.TEXTURE_2D, this.fieldTex);
      gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_S, gl.CLAMP_TO_EDGE);
      gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_T, gl.CLAMP_TO_EDGE);
      gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MIN_FILTER, gl.LINEAR);
      gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MAG_FILTER, gl.LINEAR);
      gl.texImage2D(gl.TEXTURE_2D, 0, gl.LUMINANCE, FIELD_COLS * 256, FIELD_ROWS * 256, 0, gl.LUMINANCE, gl.UNSIGNED_BYTE, bytes);
    }
    gl.activeTexture(gl.TEXTURE2);
    gl.bindTexture(gl.TEXTURE_2D, this.fieldTex);
    gl.uniform1i(this.uniforms['uMaskField']!, 2);

    gl.uniform2f(this.uniforms['uFlip']!, 0, 1);
    const pa = paramVector(params);
    const pb = beforeParams ? paramVector(beforeParams) : pa;
    gl.uniform1fv(this.uniforms['pA[0]']!, pa);
    gl.uniform1fv(this.uniforms['pB[0]']!, pb);
    // HSL 8 色带（A = 当前参数，B = 对比参数）
    const [hslA0, hslA1] = hslMatrices(params.hsl);
    const [hslB0, hslB1] = hslMatrices((beforeParams ?? params).hsl);
    gl.uniformMatrix4fv(this.uniforms['uHslA0']!, false, hslA0);
    gl.uniformMatrix4fv(this.uniforms['uHslA1']!, false, hslA1);
    gl.uniformMatrix4fv(this.uniforms['uHslB0']!, false, hslB0);
    gl.uniformMatrix4fv(this.uniforms['uHslB1']!, false, hslB1);
    gl.uniform1f(this.uniforms['uSplit']!, split === null ? -1 : split);
    gl.uniform1f(this.uniforms['uVignette']!, params.optics.vignetteAmount);
    gl.uniform1f(this.uniforms['uGrain']!, params.optics.grainAmount);

    // 蒙版（最多 6 个，超出截断）
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
      amount[i] = m.amount;
      type[i] = m.type === 'linear' ? 0 : m.type === 'radial' ? 1 : 2;
      opacity[i] = m.opacity;
    });
    gl.uniform1i(this.uniforms['uMaskCount']!, masks.length);
    gl.uniform4fv(this.uniforms['uMaskGeoA[0]']!, geoA);
    gl.uniform4fv(this.uniforms['uMaskGeoB[0]']!, geoB);
    gl.uniform4fv(this.uniforms['uMaskLocal0[0]']!, local0);
    gl.uniform4fv(this.uniforms['uMaskLocal1[0]']!, local1);
    gl.uniform1fv(this.uniforms['uMaskAmount[0]']!, amount);
    gl.uniform1fv(this.uniforms['uMaskType[0]']!, type);
    gl.uniform1fv(this.uniforms['uMaskOpacity[0]']!, opacity);

    gl.drawArrays(gl.TRIANGLE_STRIP, 0, 4);
    return true;
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
