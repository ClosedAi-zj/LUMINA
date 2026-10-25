/**
 * CPU 软渲染保底（WP-B6）：WebGL 不可用时的 2D 像素管线。
 *
 * 范围：曝光/对比/高光/阴影/白色色阶/黑色色阶/白平衡（色温色调）/自然饱和度/饱和度/色调曲线，
 * 与 GPU shader 同一套参数语义（近似实现，不是逐算子等价）。
 * 性能预算：1600px 预览 <100ms/帧 —— 单遍扫描 + 256 项查表，避免每像素函数调用。
 *
 * 调用方：ViewerCanvas 在 DevelopRenderer.init 失败或 render 失败时走这里。
 */

import type { EditParams, HslBand } from '../../../api/protocol';

/** 色带顺序与中心色相（度）：与引擎 imaging.cpp kBandCenterHue 一致 */
const HSL_ORDER: HslBand[] = ['red', 'orange', 'yellow', 'green', 'aqua', 'blue', 'purple', 'magenta'];
const HSL_CENTER = [0, 30, 60, 120, 180, 240, 280, 320];
const HSL_HALF_WIDTH = 45;

/** 8 色带参数摊平成 [hue0..7, sat0..7, lum0..7]，避免逐像素读对象属性 */
function flattenHsl(p: EditParams): Float64Array | null {
  const hsl = p.hsl;
  if (!hsl) return null;
  const out = new Float64Array(24);
  let any = false;
  for (let i = 0; i < 8; i++) {
    const b = hsl[HSL_ORDER[i]];
    if (!b) continue;
    out[i] = b.hue ?? 0;
    out[8 + i] = b.sat ?? 0;
    out[16 + i] = b.lum ?? 0;
    if (out[i] !== 0 || out[8 + i] !== 0 || out[16 + i] !== 0) any = true;
  }
  return any ? out : null;
}

/** 构建组合色调 LUT：曝光 → 对比度 → 黑白场 → 色调曲线（每通道同曲线，近似） */
function buildToneLut(p: EditParams): Float32Array {
  const lut = new Float32Array(256);
  const ev = Math.pow(2, p.exposure ?? 0);
  const contrast = 1 + (p.contrast ?? 0) / 100;
  const blacks = (p.blacks ?? 0) / 100;
  const whites = (p.whites ?? 0) / 100;
  const curve = Array.isArray(p.toneCurveRGB) && p.toneCurveRGB.length >= 2 ? [...p.toneCurveRGB].sort((a, b) => a.x - b.x) : null;
  const curveAt = (i: number): number => {
    if (!curve) return i;
    let lo = 0;
    while (lo < curve.length && curve[lo].x <= i) lo++;
    if (lo === 0) return curve[0].y;
    if (lo >= curve.length) return curve[curve.length - 1].y;
    const a = curve[lo - 1];
    const b = curve[lo];
    const t = (i - a.x) / Math.max(1, b.x - a.x);
    return a.y + (b.y - a.y) * t;
  };
  for (let i = 0; i < 256; i++) {
    let v = i * ev;
    // 对比度：围绕中灰拉伸
    v = (v - 128) * contrast + 128;
    // 黑场（提/压黑）与白场（提/压白）：端点附近的非线性推拉
    v += blacks * 60 * Math.max(0, 1 - i / 96);
    v += whites * 60 * Math.max(0, (i - 160) / 96);
    v = curveAt(Math.max(0, Math.min(255, Math.round(v))));
    lut[i] = Math.max(0, Math.min(255, v));
  }
  return lut;
}

/** 高光/阴影作用的亮度权重（平滑，避免分段硬边） */
function smoothstep(edge0: number, edge1: number, x: number): number {
  const t = Math.max(0, Math.min(1, (x - edge0) / (edge1 - edge0)));
  return t * t * (3 - 2 * t);
}

/** RGB(0..1) → HSL（h/s/l 均 0..1，h 为色相环比例） */
function rgbToHsl(r: number, g: number, b: number): [number, number, number] {
  const mx = Math.max(r, Math.max(g, b));
  const mn = Math.min(r, Math.min(g, b));
  const l = (mx + mn) / 2;
  const d = mx - mn;
  if (d <= 1e-6) return [0, 0, l];
  const s = l > 0.5 ? d / (2 - mx - mn) : d / (mx + mn);
  let h: number;
  if (mx === r) h = (g - b) / d + (g < b ? 6 : 0);
  else if (mx === g) h = (b - r) / d + 2;
  else h = (r - g) / d + 4;
  return [h / 6, s, l];
}

/** HSL → RGB(0..1) */
function hslToRgb(h: number, s: number, l: number): [number, number, number] {
  if (s <= 1e-6) return [l, l, l];
  const q = l < 0.5 ? l * (1 + s) : l + s - l * s;
  const p = 2 * l - q;
  const comp = (t0: number): number => {
    let t = t0;
    if (t < 0) t += 1;
    if (t > 1) t -= 1;
    if (t < 1 / 6) return p + (q - p) * 6 * t;
    if (t < 0.5) return q;
    if (t < 2 / 3) return p + (q - p) * (2 / 3 - t) * 6;
    return p;
  };
  return [comp(h + 1 / 3), comp(h), comp(h - 1 / 3)];
}

export interface CpuRenderOptions {
  params: EditParams;
  /** 前后对比：分割位置 0..1；null/undefined = 不对比 */
  split?: number | null;
  beforeParams?: EditParams | null;
}

let scratch: HTMLCanvasElement | null = null;

/** 单遍参数处理：对 imageData 就地应用（WB → 色调 LUT → 高光阴影 → 饱和） */
function applyParams(d: Uint8ClampedArray, lut: Float32Array, p: EditParams): void {
  const customWb = p.whiteBalance === 'custom';
  const temp = customWb ? Math.max(-1, Math.min(1, ((p.temperature ?? 5500) - 5500) / 4500)) : 0;
  const tint = customWb ? (p.tint ?? 0) / 100 : 0;
  const rGain = 1 + temp * 0.1;
  const bGain = 1 - temp * 0.1;
  const gGain = 1 - tint * 0.06;
  const highlights = (p.highlights ?? 0) / 100;
  const shadows = (p.shadows ?? 0) / 100;
  const saturation = 1 + (p.saturation ?? 0) / 100;
  const vibrance = (p.vibrance ?? 0) / 100;
  const bands = flattenHsl(p);
  const n = d.length;
  for (let i = 0; i < n; i += 4) {
    // 白平衡增益
    let r = d[i] * rGain;
    let g = d[i + 1] * gGain;
    let b = d[i + 2] * bGain;
    // 色调 LUT（曝光/对比/黑白场/曲线）
    r = lut[r < 0 ? 0 : r > 255 ? 255 : r | 0];
    g = lut[g < 0 ? 0 : g > 255 ? 255 : g | 0];
    b = lut[b < 0 ? 0 : b > 255 ? 255 : b | 0];
    // 亮度权重（Rec.601 近似）
    const lum = (r * 0.299 + g * 0.587 + b * 0.114) / 255;
    if (highlights !== 0) {
      const w = smoothstep(0.45, 1, lum) * highlights * 70;
      r += w;
      g += w;
      b += w;
    }
    if (shadows !== 0) {
      const w = (1 - smoothstep(0, 0.55, lum)) * shadows * 70;
      r += w;
      g += w;
      b += w;
    }
    // 饱和度 / 自然饱和度（vibrance 对低饱和像素加权）
    const mx = r > g ? (r > b ? r : b) : g > b ? g : b;
    const mn = r < g ? (r < b ? r : b) : g < b ? g : b;
    const sat = mx === 0 ? 0 : (mx - mn) / mx;
    let s = saturation;
    if (vibrance !== 0) s += vibrance * (1 - sat);
    if (s !== 1) {
      const gray = r * 0.299 + g * 0.587 + b * 0.114;
      r = gray + (r - gray) * s;
      g = gray + (g - gray) * s;
      b = gray + (b - gray) * s;
    }
    // HSL 8 色带（WP-B4，口径同引擎 applyHslApprox：45° 三角权重，hue×0.15°，sat×1+Σ/100，lum+Σ/200）
    if (bands) {
      const rn = (r < 0 ? 0 : r > 255 ? 255 : r) / 255;
      const gn = (g < 0 ? 0 : g > 255 ? 255 : g) / 255;
      const bn = (b < 0 ? 0 : b > 255 ? 255 : b) / 255;
      const [hue, satH, li] = rgbToHsl(rn, gn, bn);
      if (satH > 1e-4) {
        let hueSum = 0;
        let satSum = 0;
        let lumSum = 0;
        let wSum = 0;
        for (let k = 0; k < 8; k++) {
          const hk = bands[k];
          const sk = bands[8 + k];
          const lk = bands[16 + k];
          if (hk === 0 && sk === 0 && lk === 0) continue;
          let dist = Math.abs(hue * 360 - HSL_CENTER[k]);
          if (dist > 180) dist = 360 - dist;
          if (dist >= HSL_HALF_WIDTH) continue;
          const w = 1 - dist / HSL_HALF_WIDTH;
          wSum += w;
          hueSum += w * hk;
          satSum += w * sk;
          lumSum += w * lk;
        }
        if (wSum > 0) {
          let h2 = hue * 360 + hueSum * 0.15;
          if (h2 < 0) h2 += 360;
          if (h2 >= 360) h2 -= 360;
          const s2 = Math.max(0, Math.min(1, satH * (1 + satSum / 100)));
          const l2 = Math.max(0, Math.min(1, li + lumSum / 200));
          const rgb = hslToRgb(h2 / 360, s2, l2);
          r = rgb[0] * 255;
          g = rgb[1] * 255;
          b = rgb[2] * 255;
        }
      }
    }
    d[i] = r < 0 ? 0 : r > 255 ? 255 : r;
    d[i + 1] = g < 0 ? 0 : g > 255 ? 255 : g;
    d[i + 2] = b < 0 ? 0 : b > 255 ? 255 : b;
  }
}

/** 把图像按 contain 适配画进画布（供参数前的底图绘制） */
function drawFit(canvas: HTMLCanvasElement, img: HTMLImageElement): { w: number; h: number } | null {
  const ctx = canvas.getContext('2d');
  if (!ctx || !img.naturalWidth || !img.naturalHeight) return null;
  // 与 GPU 路径同一口径：量宿主容器，不量 canvas 自己（否则 CSS 盒↔缓冲尺寸自引用死锁）
  const host = (canvas.closest('[data-viewer-host]') as HTMLElement | null) ?? canvas.parentElement;
  const hostRect = host ? host.getBoundingClientRect() : canvas.getBoundingClientRect();
  const availW = hostRect.width > 0 ? hostRect.width : img.naturalWidth;
  const availH = hostRect.height > 0 ? hostRect.height : availW * (img.naturalHeight / img.naturalWidth);
  const dpr = Math.min(window.devicePixelRatio || 1, 1.5);
  let cssW = availW;
  let cssH = cssW * (img.naturalHeight / img.naturalWidth);
  if (cssH > availH) {
    cssH = availH;
    cssW = cssH * (img.naturalWidth / img.naturalHeight);
  }
  // 性能预算：解码尺寸超过 1600px 时先降采样（长边 1600）
  const cap = 1600;
  const k = Math.min(cssW / img.naturalWidth, cap / Math.max(img.naturalWidth, img.naturalHeight));
  const w = Math.max(64, Math.round(img.naturalWidth * k));
  const h = Math.max(36, Math.round(img.naturalHeight * k));
  if (canvas.width !== w || canvas.height !== h) {
    canvas.width = w;
    canvas.height = h;
  }
  canvas.style.width = `${Math.round(w / dpr)}px`;
  canvas.style.height = `${Math.round(h / dpr)}px`;
  ctx.clearRect(0, 0, w, h);
  ctx.drawImage(img, 0, 0, w, h);
  return { w, h };
}

/**
 * CPU 渲染入口。返回耗时 ms（诊断/冒烟用），画布内容为参数处理后的图像。
 * split 有效时：整幅按 after 参数渲染，再以 before 参数渲染左半（clip 到分割线）。
 */
export function renderCpu(canvas: HTMLCanvasElement, img: HTMLImageElement, opts: CpuRenderOptions): number {
  const t0 = performance.now();
  const ctx = canvas.getContext('2d');
  if (!ctx) return -1;
  const fit = drawFit(canvas, img);
  if (!fit) return -1;

  const after = ctx.getImageData(0, 0, fit.w, fit.h);
  applyParams(after.data, buildToneLut(opts.params), opts.params);
  ctx.putImageData(after, 0, 0);

  const split = opts.split;
  if (split !== null && split !== undefined && opts.beforeParams) {
    if (!scratch) scratch = document.createElement('canvas');
    scratch.width = fit.w;
    scratch.height = fit.h;
    const sctx = scratch.getContext('2d');
    if (sctx) {
      sctx.clearRect(0, 0, fit.w, fit.h);
      sctx.drawImage(img, 0, 0, fit.w, fit.h);
      const before = sctx.getImageData(0, 0, fit.w, fit.h);
      applyParams(before.data, buildToneLut(opts.beforeParams), opts.beforeParams);
      sctx.putImageData(before, 0, 0);
      ctx.save();
      ctx.beginPath();
      ctx.rect(0, 0, Math.round(fit.w * split), fit.h);
      ctx.clip();
      ctx.drawImage(scratch, 0, 0);
      ctx.restore();
    }
  }
  return Math.round(performance.now() - t0);
}
