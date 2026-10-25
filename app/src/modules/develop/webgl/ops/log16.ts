/**
 * T1.4 · log16 编码域 + 传递函数 + 确定性噪声（与引擎 imaging/image_buffer.h、
 * output_encode.cpp、plan/ops/ops.h 同口径的 TS 镜像）。
 *
 * 引擎口径（T0 基线，禁改）：
 *  - log16：code = (log2(v) + 12) * 2048，v ∈ [2^-12, 2^20]；1EV = +2048 码。
 *  - sRGB 传递：linearToSrgb / srgbToLinear 精确分段式（output_encode.cpp）。
 *  - hashNoise：uint32 哈希（Math.imul 镜像 C++ 无符号回绕）。
 *  - TPDF：引擎 C++ 用 rand()（非确定性）；TS/GPU 侧用确定性像素哈希
 *    （分布同为 [-0.5,0.5] 三角）——见 T1.4 回报偏差③。
 */

export const LOG_STEP = 2048;
export const CODE_MAX = 65535;

/** 线性值 → log16 码（0..65535），越界 clamp（与引擎 log16Encode 一致） */
export function log16Encode(v: number, logStep: number = LOG_STEP): number {
  const floor = Math.pow(2, -12);
  const x = v < floor ? floor : v;
  const code = (Math.log2(x) + 12) * logStep;
  return Math.max(0, Math.min(65535, Math.round(code)));
}

/** log16 码 → 线性值 */
export function log16Decode(code: number, logStep: number = LOG_STEP): number {
  return Math.pow(2, code / logStep - 12);
}

/** 码 → 归一化 t ∈ [0,1]（感知坐标，S2 分区/S4 曲线类的算子域） */
export function tFromCode(code: number): number {
  return code / 65535;
}

/** t ∈ [0,1] → 码 */
export function codeFromT(t: number): number {
  return Math.max(0, Math.min(65535, Math.round(t * 65535)));
}

/** t → 线性（v = 2^(t*32-12)） */
export function linearFromT(t: number): number {
  return Math.pow(2, t * 32 - 12);
}

/** 线性 → t */
export function tFromLinear(v: number): number {
  return (Math.log2(Math.max(v, Math.pow(2, -12))) + 12) / 32;
}

/** sRGB 传递（engine output_encode.cpp linearToSrgb 精确镜像） */
export function linearToSrgb(linear: number): number {
  if (linear <= 0) return 0;
  if (linear >= 1) return 1;
  if (linear <= 0.0031308) return linear * 12.92;
  return 1.055 * Math.pow(linear, 1 / 2.4) - 0.055;
}

/** sRGB 逆传递（engine srgbToLinear 精确镜像） */
export function srgbToLinear(s: number): number {
  if (s <= 0) return 0;
  if (s >= 1) return 1;
  if (s <= 0.04045) return s / 12.92;
  return Math.pow((s + 0.055) / 1.055, 2.4);
}

/** uint32 哈希噪声（C++ hashNoise 的 Math.imul 精确镜像）→ [0,1) */
export function hashNoise(x: number, y: number, seed: number): number {
  let h =
    (Math.imul(x, 374761393) + Math.imul(y, 668265263) + Math.imul(seed, 2147483647)) >>> 0;
  h = (Math.imul(h ^ (h >>> 13), 1274126177)) >>> 0;
  h = (h ^ (h >>> 16)) >>> 0;
  return (h & 0xffffff) / 16777216;
}

/** 确定性 TPDF：两次独立像素哈希均值 → [-0.5,0.5] 三角分布。
 *  通道无关（GPU 噪声纹理 G/B 为逐像素同源值，c 参数仅为兼容保留）；
 *  引擎 C++ 用 rand() 非确定，本确定性方案为 TS/GPU 共享口径（偏差③）。 */
export function tpdfDither(x: number, y: number, c: number = 0): number {
  void c;
  const u1 = hashNoise(x, y, 1000);
  const u2 = hashNoise(x + 7919, y + 104729, 2000);
  return (u1 + u2) * 0.5 - 0.5;
}

/** smoothstep（engine ops.h smooth01 精确镜像） */
export function smooth01(x: number, edge0: number, edge1: number): number {
  if (edge1 <= edge0) return x >= edge1 ? 1 : 0;
  let t = (x - edge0) / (edge1 - edge0);
  t = t < 0 ? 0 : t > 1 ? 1 : t;
  return t * t * (3 - 2 * t);
}

export function clamp01(v: number): number {
  return v < 0 ? 0 : v > 1 ? 1 : v;
}

/** RGB↔HSL（engine opsmath.cpp 精确镜像，0..1 域） */
export function rgbToHsl(r: number, g: number, b: number): [number, number, number] {
  const mx = Math.max(r, Math.max(g, b));
  const mn = Math.min(r, Math.min(g, b));
  const d = mx - mn;
  const l = (mx + mn) / 2;
  if (d <= 1e-12) return [0, 0, l];
  const s = l > 0.5 ? d / (2 - mx - mn) : d / (mx + mn);
  let h: number;
  if (mx === r) h = (g - b) / d + (g < b ? 6 : 0);
  else if (mx === g) h = (b - r) / d + 2;
  else h = (r - g) / d + 4;
  return [h / 6, s, l];
}

function hue2rgb(p: number, q: number, t0: number): number {
  let t = t0;
  if (t < 0) t += 1;
  if (t > 1) t -= 1;
  if (t < 1 / 6) return p + (q - p) * 6 * t;
  if (t < 0.5) return q;
  if (t < 2 / 3) return p + (q - p) * (2 / 3 - t) * 6;
  return p;
}

export function hslToRgb(h: number, s: number, l: number): [number, number, number] {
  if (s <= 1e-12) return [l, l, l];
  const q = l < 0.5 ? l * (1 + s) : l + s - l * s;
  const p = 2 * l - q;
  return [hue2rgb(p, q, h + 1 / 3), hue2rgb(p, q, h), hue2rgb(p, q, h - 1 / 3)];
}

/**
 * 可分离盒式模糊（engine boxBlurPlane 滑动窗口精确镜像；float 平面）。
 */
export function boxBlurPlane(
  src: Float32Array,
  dst: Float32Array,
  w: number,
  h: number,
  radius: number,
): void {
  if (radius <= 0) {
    dst.set(src);
    return;
  }
  const tmp = new Float32Array(w * h);
  const win = radius * 2 + 1;
  for (let y = 0; y < h; ++y) {
    const row = y * w;
    let sum = 0;
    let lo = 0;
    let hi = 0;
    for (let x = 0; x < w; ++x) {
      while (hi < Math.min(w, x + radius + 1)) sum += src[row + hi++];
      while (lo < x - radius) sum -= src[row + lo++];
      tmp[row + x] = sum / Math.min(hi - lo, win);
    }
  }
  for (let x = 0; x < w; ++x) {
    let sum = 0;
    let lo = 0;
    let hi = 0;
    for (let y = 0; y < h; ++y) {
      while (hi < Math.min(h, y + radius + 1)) sum += tmp[hi++ * w + x];
      while (lo < y - radius) sum -= tmp[lo++ * w + x];
      dst[y * w + x] = sum / Math.min(hi - lo, win);
    }
  }
}
