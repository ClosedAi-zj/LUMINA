/**
 * T1.4 · 93 叶 TS oracle（engine plan/ops/*.cpp @ 8ba9c53 的逐式镜像）。
 *
 * 数据域：t = log16 码 / 65535（感知坐标），交错 RGB Float32 —— 与引擎 uint16 码域
 * 语义一致（parity 以 8bit 输出对比，t 量化精度 1/65535 高于 8bit 门槛）。
 * Stage 顺序 = 引擎 OpTable：S0 WB → S1 geometry+lens → S1.5 NR → S2 tone（含
 * clarity/texture 局部对比）→ S3 toneMap → S4 curve/hsl/colorGrade → S5 sharpen
 * → S6 effects → S7 encode8。
 * GPU（ops/glsl.ts + ops/pipeline.ts）逐 pass 与本文件逐式对应，公式不允许分叉。
 */

import type { EditParams, CurvePoint } from '../../../../api/protocol';
import {
  boxBlurPlane,
  clamp01,
  hashNoise,
  hslToRgb,
  linearToSrgb,
  log16Decode,
  log16Encode,
  rgbToHsl,
  smooth01,
  tpdfDither,
} from './log16';

export interface PlaneBuf {
  /** 交错 RGB，t = log16 码 / 65535 */
  t: Float32Array;
  w: number;
  h: number;
}

export interface OracleResult {
  t: Float32Array;
  w: number;
  h: number;
  /** S7 输出：8bit sRGB + 确定性 TPDF（parity 对比域） */
  out8: Uint8Array;
}

const num = (v: number | undefined, def: number): number =>
  typeof v === 'number' && Number.isFinite(v) ? v : def;

// ---- 曲线 LUT（engine curve.cpp lutFromPoints 精确镜像） ----
export function lutFromPoints(points: CurvePoint[] | undefined): Float64Array {
  const lut = new Float64Array(256);
  for (let i = 0; i < 256; ++i) lut[i] = i;
  if (!Array.isArray(points) || points.length < 2) return lut;
  const p = [...points]
    .map((pt) => ({ x: num(pt.x, 0), y: num(pt.y, 0) }))
    .sort((a, b) => a.x - b.x);
  if (p[0].x > 0) p.unshift({ x: 0, y: p[0].y });
  if (p[p.length - 1].x < 255) p.push({ x: 255, y: p[p.length - 1].y });
  for (let i = 0; i < 256; ++i) {
    const u = i;
    let k = 0;
    while (k + 1 < p.length && p[k + 1].x < u) ++k;
    const t = (u - p[k].x) / Math.max(1e-9, p[k + 1].x - p[k].x);
    lut[i] = p[k].y + (p[k + 1].y - p[k].y) * t;
  }
  return lut;
}

// ---- 几何单应（engine plan/geometry.cpp 精确镜像，含 double 精度与截断口径） ----
interface Mat3 {
  m: number[]; // 行主序 9
}
function mul3(a: Mat3, b: Mat3): Mat3 {
  const r = new Array<number>(9).fill(0);
  for (let i = 0; i < 3; ++i)
    for (let j = 0; j < 3; ++j) {
      let s = 0;
      for (let k = 0; k < 3; ++k) s += a.m[i * 3 + k] * b.m[k * 3 + j];
      r[i * 3 + j] = s;
    }
  return { m: r };
}
function invert3(a: Mat3): Mat3 | null {
  const m = a.m;
  const c00 = m[4] * m[8] - m[5] * m[7];
  const c01 = m[5] * m[6] - m[3] * m[8];
  const c02 = m[3] * m[7] - m[4] * m[6];
  const det = m[0] * c00 + m[1] * c01 + m[2] * c02;
  if (Math.abs(det) < 1e-12) return null;
  const s = 1 / det;
  return {
    m: [
      c00 * s,
      (m[2] * m[7] - m[1] * m[8]) * s,
      (m[1] * m[5] - m[2] * m[4]) * s,
      c01 * s,
      (m[0] * m[8] - m[2] * m[6]) * s,
      (m[2] * m[3] - m[0] * m[7]) * s,
      c02 * s,
      (m[1] * m[6] - m[0] * m[7]) * s,
      (m[0] * m[4] - m[1] * m[3]) * s,
    ],
  };
}
function mapQuadBounds(fwd: Mat3, w: number, h: number): [number, number, number, number] {
  const xs = [0, w, w, 0];
  const ys = [0, 0, h, h];
  let minX = 0;
  let minY = 0;
  let maxX = 0;
  let maxY = 0;
  let first = true;
  for (let i = 0; i < 4; ++i) {
    const sw = fwd.m[6] * xs[i] + fwd.m[7] * ys[i] + fwd.m[8];
    const u = (fwd.m[0] * xs[i] + fwd.m[1] * ys[i] + fwd.m[2]) / sw;
    const v = (fwd.m[3] * xs[i] + fwd.m[4] * ys[i] + fwd.m[5]) / sw;
    if (first) {
      minX = maxX = u;
      minY = maxY = v;
      first = false;
    } else {
      minX = Math.min(minX, u);
      maxX = Math.max(maxX, u);
      minY = Math.min(minY, v);
      maxY = Math.max(maxY, v);
    }
  }
  return [minX, minY, maxX, maxY];
}

export interface GeomTransform {
  /** 逆单应（dst 像素 → 源全图像素坐标）；null = 无几何（直通） */
  inv: Mat3 | null;
  outW: number;
  outH: number;
  cropX: number;
  cropY: number;
}

/** buildForwardHomography + invert3（含 perspective 矩阵的引擎原样展开） */
export function buildGeomTransform(p: EditParams, srcW: number, srcH: number): GeomTransform {
  const g = p.geometry;
  if (!g) return { inv: null, outW: srcW, outH: srcH, cropX: 0, cropY: 0 };
  const hasCrop = !!g.hasCrop;
  const angle = num(g.cropAngle, 0);
  const cropActive = hasCrop || angle !== 0;
  const ps = g.perspective;
  const perspActive =
    !!ps &&
    (num(ps.x, 0) !== 0 ||
      num(ps.y, 0) !== 0 ||
      num(ps.horizontal, 0) !== 0 ||
      num(ps.vertical, 0) !== 0 ||
      num(ps.rotate, 0) !== 0 ||
      num(ps.scale, 1) !== 1);
  if (!cropActive && !perspActive) {
    return { inv: null, outW: srcW, outH: srcH, cropX: 0, cropY: 0 };
  }
  const cropX = hasCrop ? Math.trunc((num(g.cropLeft, 0) / 100) * srcW) : 0;
  const cropY = hasCrop ? Math.trunc((num(g.cropTop, 0) / 100) * srcH) : 0;
  const cw = hasCrop ? Math.max(1, Math.trunc((num(g.cropRight, 100) / 100) * srcW) - cropX) : srcW;
  const ch = hasCrop
    ? Math.max(1, Math.trunc((num(g.cropBottom, 100) / 100) * srcH) - cropY)
    : srcH;
  // 引擎为两个独立算子（crop 单应 → perspective 单应）顺序执行；此处合并为一次
  // 单应——单侧激活时与引擎逐式一致，双侧同开为合并近似（偏差④，T2.4 精修）。
  const angleDeg = (cropActive ? angle : 0) + (perspActive ? num(ps.rotate, 0) : 0);
  const pH = perspActive ? num(ps.horizontal, 0) / 100 + (num(ps.x, 0) / 100) * 0.1 : 0;
  const pV = perspActive ? num(ps.vertical, 0) / 100 + (num(ps.y, 0) / 100) * 0.1 : 0;
  // scale 语义 = 倍数（UI 滑杆 0.5–2，默认 1=恒等；与 protocol.ts/TransformBar 一致）。
  // 曾按引擎旧口径写成 num(ps.scale,100)/100 → 会话默认 scale:1 被判激活且缩到 5%，
  // 主画布缓冲塌成 4x3（T1.4 回归根因，2026-10-25 修复，两端同步改倍数语义）。
  const scale = perspActive ? Math.max(0.05, num(ps.scale, 1)) : 1.0;
  const halfW = cw / 2;
  const halfH = ch / 2;
  const theta = (angleDeg * Math.PI) / 180;
  const cosT = Math.cos(theta);
  const sinT = Math.sin(theta);
  // persp：w' = pH/halfW·x + pV/halfH·y + (1 - pH - pV)（engine 原样展开结果）
  const w0 = 1 - pH - pV;
  const persp: Mat3 = { m: [1, 0, 0, 0, 1, 0, pH / halfW, pV / halfH, w0] };
  const toCenter: Mat3 = { m: [1, 0, -halfW, 0, 1, -halfH, 0, 0, 1] };
  const rot: Mat3 = { m: [cosT, -sinT, 0, sinT, cosT, 0, 0, 0, 1] };
  const scl: Mat3 = { m: [scale, 0, 0, 0, scale, 0, 0, 0, 1] };
  const fwdNoOut = mul3(scl, mul3(rot, mul3(toCenter, persp)));
  const [minX, minY, maxX, maxY] = mapQuadBounds(fwdNoOut, cw, ch);
  const outW = Math.max(1, Math.ceil(maxX - minX));
  const outH = Math.max(1, Math.ceil(maxY - minY));
  const fwd = mul3({ m: [1, 0, -minX, 0, 1, -minY, 0, 0, 1] }, fwdNoOut);
  const inv = invert3(fwd);
  if (!inv) return { inv: null, outW: srcW, outH: srcH, cropX: 0, cropY: 0 };
  return { inv, outW, outH, cropX: Math.max(0, cropX), cropY: Math.max(0, cropY) };
}

// ---- 主管线 ----

const kMidCode = 65535.0 * 0.45;

function sampleBilinearT(
  src: Float32Array,
  w: number,
  h: number,
  x: number,
  y: number,
  out: Float64Array,
): void {
  const x0 = Math.floor(x);
  const y0 = Math.floor(y);
  const fx = x - x0;
  const fy = y - y0;
  const x1 = Math.min(x0 + 1, w - 1);
  const y1 = Math.min(y0 + 1, h - 1);
  const cx0 = Math.max(0, Math.min(x0, w - 1));
  const cy0 = Math.max(0, Math.min(y0, h - 1));
  for (let c = 0; c < 3; ++c) {
    const p00 = src[(cy0 * w + cx0) * 3 + c];
    const p10 = src[(cy0 * w + x1) * 3 + c];
    const p01 = src[(y1 * w + cx0) * 3 + c];
    const p11 = src[(y1 * w + x1) * 3 + c];
    const top = p00 + (p10 - p00) * fx;
    const bot = p01 + (p11 - p01) * fx;
    out[c] = Math.max(0, Math.min(1, top + (bot - top) * fy));
  }
}

/** S0：温度/色调（码域 EV 偏移；引擎 basic.cpp WbGainOp::shiftChannels） */
function applyWb(p: EditParams, buf: Float32Array): void {
  if (p.whiteBalance !== 'custom') return;
  const tTemp = num(p.temperature, 0) * 0.002;
  const tTint = num(p.tint, 0) * 0.001;
  if (tTemp === 0 && tTint === 0) return;
  for (let i = 0; i + 2 < buf.length; i += 3) {
    buf[i] = clamp01((buf[i] * 65535 + tTemp * 2048) / 65535);
    buf[i + 2] = clamp01((buf[i + 2] * 65535 - tTemp * 2048) / 65535);
  }
  for (let i = 0; i + 2 < buf.length; i += 3) {
    buf[i] = clamp01((buf[i] * 65535 + tTint * 0.5 * 2048) / 65535);
    buf[i + 1] = clamp01((buf[i + 1] * 65535 - tTint * 2048) / 65535);
    buf[i + 2] = clamp01((buf[i + 2] * 65535 + tTint * 0.5 * 2048) / 65535);
  }
}

/** S1 几何：crop/perspective 单应 + 手动畸变（线性域重映射） */
function applyGeometryStage(p: EditParams, src: PlaneBuf): PlaneBuf {
  const geom = buildGeomTransform(p, src.w, src.h);
  let cur: PlaneBuf = src;
  if (geom.inv) {
    const out = new Float32Array(geom.outW * geom.outH * 3);
    const px = new Float64Array(3);
    const m = geom.inv.m;
    for (let dy = 0; dy < geom.outH; ++dy) {
      for (let dx = 0; dx < geom.outW; ++dx) {
        const sw = m[6] * dx + m[7] * dy + m[8];
        const sx = (m[0] * dx + m[1] * dy + m[2]) / sw;
        const sy = (m[3] * dx + m[4] * dy + m[5]) / sw;
        const fullX = sx + geom.cropX;
        const fullY = sy + geom.cropY;
        const q = (dy * geom.outW + dx) * 3;
        // 出界判定带 1e-4 像素容差（与 GPU FRAG_GEO 同步，见 glsl.ts 注）
        if (fullX >= -1e-4 && fullY >= -1e-4 && fullX <= src.w - 1 + 1e-4 && fullY <= src.h - 1 + 1e-4) {
          sampleBilinearT(src.t, src.w, src.h, fullX, fullY, px);
          out[q] = px[0];
          out[q + 1] = px[1];
          out[q + 2] = px[2];
        } // 出界填黑（t=0）
      }
    }
    cur = { t: out, w: geom.outW, h: geom.outH };
  }
  // lens.manualDistortion：线性域重映射（engine lens.cpp 精确镜像）
  const k = (num(p.optics?.manualDistortion, 0) / 100) * 0.3;
  if (k !== 0 && cur.w > 0 && cur.h > 0) {
    const { w, h } = cur;
    const lin = new Float32Array(cur.t.length);
    for (let i = 0; i < cur.t.length; ++i) lin[i] = log16Decode(cur.t[i] * 65535);
    const out = new Float32Array(cur.t.length);
    const cx = (w - 1) / 2;
    const cy = (h - 1) / 2;
    const maxR = Math.sqrt(cx * cx + cy * cy);
    for (let y = 0; y < h; ++y) {
      for (let x = 0; x < w; ++x) {
        const dx = x - cx;
        const dy = y - cy;
        const r = Math.sqrt(dx * dx + dy * dy) / maxR;
        const rf = r * (1 + k * r * r);
        const sx = cx + (dx * rf) / Math.max(1e-9, r);
        const sy = cy + (dy * rf) / Math.max(1e-9, r);
        const x0 = Math.max(0, Math.min(w - 1, Math.floor(sx)));
        const y0 = Math.max(0, Math.min(h - 1, Math.floor(sy)));
        const x1 = Math.min(x0 + 1, w - 1);
        const y1 = Math.min(y0 + 1, h - 1);
        const fx = Math.max(0, Math.min(1, sx - x0));
        const fy = Math.max(0, Math.min(1, sy - y0));
        const q = (y * w + x) * 3;
        for (let c = 0; c < 3; ++c) {
          const p00 = lin[(y0 * w + x0) * 3 + c];
          const p10 = lin[(y0 * w + x1) * 3 + c];
          const p01 = lin[(y1 * w + x0) * 3 + c];
          const p11 = lin[(y1 * w + x1) * 3 + c];
          const top = p00 + (p10 - p00) * fx;
          const bot = p01 + (p11 - p01) * fx;
          const v = top + (bot - top) * fy;
          out[q + c] = log16Encode(v) / 65535;
        }
      }
    }
    cur = { t: out, w, h };
  }
  return cur;
}

/** S1.5 NR：色度模糊 + 亮度平滑（engine detail.cpp DenoiseOp 精确镜像） */
function applyDenoise(p: EditParams, buf: PlaneBuf): void {
  const cnr = num(p.detail?.colorNR, 0);
  const cnrDetail = num(p.detail?.colorNRDetail, 50);
  const luma = num(p.detail?.luminanceSmoothing, 0);
  if (cnr === 0 && luma === 0) return;
  const { w, h, t } = buf;
  const lum = new Float32Array(w * h);
  for (let i = 0, j = 0; j < w * h; i += 3, ++j) {
    lum[j] = (t[i] + t[i + 1] + t[i + 2]) / 3;
  }
  const chromaR = Math.max(1, Math.round((cnr / 100) * 5));
  const lumaR = Math.max(1, Math.round((luma / 100) * 3));
  const chromaKeep = (cnrDetail / 100) * 0.5;
  const plane = new Float32Array(w * h);
  const blurred = new Float32Array(w * h);
  for (let c = 0; c < 3; ++c) {
    if (cnr > 0) {
      for (let j = 0; j < w * h; ++j) plane[j] = t[j * 3 + c] - lum[j];
      boxBlurPlane(plane, blurred, w, h, chromaR);
      for (let j = 0; j < w * h; ++j) {
        t[j * 3 + c] = clamp01(t[j * 3 + c] + (blurred[j] - plane[j]) * (1 - chromaKeep));
      }
    }
    if (luma > 0) {
      for (let j = 0; j < w * h; ++j) plane[j] = t[j * 3 + c];
      boxBlurPlane(plane, blurred, w, h, lumaR);
      for (let j = 0; j < w * h; ++j) {
        t[j * 3 + c] = clamp01(t[j * 3 + c] + (blurred[j] - plane[j]) * (luma / 100));
      }
    }
  }
}

/** S2+S3+S4 逐像素段：曝光/对比/分区/去雾/饱和 + 校准 + toneMap + 曲线/HSL/分级 */
function applyToneColor(p: EditParams, buf: PlaneBuf): void {
  const t = buf.t;
  const n = buf.w * buf.h;
  // basic 曝光系（引擎 basic.cpp 逐式）
  const ev = num(p.exposure, 0);
  const contrast = num(p.contrast, 0);
  const highlights = num(p.highlights, 0);
  const shadows = num(p.shadows, 0);
  const whites = num(p.whites, 0);
  const blacks = num(p.blacks, 0);
  const dehaze = num(p.dehaze, 0);
  const vibrance = num(p.vibrance, 0);
  const saturation = num(p.saturation, 0);
  const zones: [number, number][] = [
    [highlights, 0],
    [shadows, 1],
    [whites, 2],
    [blacks, 3],
  ];
  const calib = p.calibration;
  const primaries: [string, number, number, number][] = calib
    ? [
        // 元组序 = [名, ch(-1=shadowTint), hue, sat]；shadowTint 走 ch=-1 专用分支
        ['shadowTint', -1, num(calib.shadowTint, 0), 0],
        ['red', 0, num(calib.redHue, 0), num(calib.redSat, 0)],
        ['green', 1, num(calib.greenHue, 0), num(calib.greenSat, 0)],
        ['blue', 2, num(calib.blueHue, 0), num(calib.blueSat, 0)],
      ]
    : [];
  const lutRgb = lutFromPoints(p.toneCurveRGB);
  const lutR = lutFromPoints(p.toneCurveR);
  const lutG = lutFromPoints(p.toneCurveG);
  const lutB = lutFromPoints(p.toneCurveB);
  // 引擎 curve.cpp：恒等 LUT 直接跳过（恒等查表会引入 257 栅格再量化，语义不同）
  const isIdLut = (lut: Float64Array): boolean => {
    for (let i = 0; i < 256; ++i) if (Math.abs(lut[i] - i) > 1e-9) return false;
    return true;
  };
  const idRgb = isIdLut(lutRgb);
  const idR = isIdLut(lutR);
  const idG = isIdLut(lutG);
  const idB = isIdLut(lutB);
  const param = p.parametric;
  const pmHi = num(param?.highlights, 0);
  const pmLi = num(param?.lights, 0);
  const pmDa = num(param?.darks, 0);
  const pmSh = num(param?.shadows, 0);
  const s0 = (num(param?.shadowSplit, 25) / 100) * 255;
  const s1 = (num(param?.midSplit, 75) / 100) * 255;
  const sL = (num(param?.lightSplit, 50) / 100) * 255;
  const grade = p.colorGrade;
  const gSh = num(grade?.shadowHue, 0);
  const gShs = num(grade?.shadowSat, 0);
  const gMh = num(grade?.midtoneHue, 0);
  const gMs = num(grade?.midtoneSat, 0);
  const gHh = num(grade?.highlightHue, 0);
  const gHs = num(grade?.highlightSat, 0);
  const gBlend = num(grade?.blend, 50);
  const gBalance = num(grade?.balance, 0);
  const gradeActive =
    gSh !== 0 || gShs !== 0 || gMh !== 0 || gMs !== 0 || gHh !== 0 || gHs !== 0;
  const bBalance = (gBalance / 100) * 0.15;
  const spread = 0.25 + (gBlend / 100) * 0.25;
  const bands = p.hsl;
  const bandCenters = [0, 1 / 12, 1 / 6, 1 / 3, 0.5, 2 / 3, 5 / 6, 11 / 12];
  const bandKeys = ['red', 'orange', 'yellow', 'green', 'aqua', 'blue', 'purple', 'magenta'];

  for (let px = 0; px < n; ++px) {
    const i = px * 3;
    const x = px % buf.w;
    const y = (px / buf.w) | 0;
    // --- 曝光 ---
    if (ev !== 0) {
      const d = ev * 2048;
      t[i] = clamp01((t[i] * 65535 + d) / 65535);
      t[i + 1] = clamp01((t[i + 1] * 65535 + d) / 65535);
      t[i + 2] = clamp01((t[i + 2] * 65535 + d) / 65535);
    }
    // --- 对比（kMidCode 锚点拉伸） ---
    if (contrast !== 0) {
      const k = 1 + (contrast / 100) * 0.3;
      for (let c = 0; c < 3; ++c) {
        t[i + c] = clamp01((kMidCode + (t[i + c] * 65535 - kMidCode) * k) / 65535);
      }
    }
    // --- 高光/阴影/白/黑 分区 ---
    for (const [amt, kind] of zones) {
      if (amt === 0) continue;
      const l = (t[i] + t[i + 1] + t[i + 2]) / 3;
      let w: number;
      if (kind === 0) w = smooth01(l, 0.5, 1.0);
      else if (kind === 1) w = 1 - smooth01(l, 0.0, 0.5);
      else if (kind === 2) w = smooth01(l, 0.75, 1.0);
      else w = 1 - smooth01(l, 0.0, 0.25);
      const d = ((amt / 100) * 3000 * w) / 65535;
      for (let c = 0; c < 3; ++c) t[i + c] = clamp01(t[i + c] + d);
    }
    // --- 去雾（简化组合） ---
    if (dehaze !== 0) {
      const dz = dehaze / 100;
      const kShadow = dz * 1500;
      const kBlack = -dz * 500;
      const kSat = dz * 0.25;
      const l = (t[i] + t[i + 1] + t[i + 2]) / 3;
      const mean = (t[i] + t[i + 1] + t[i + 2]) / 3;
      for (let c = 0; c < 3; ++c) {
        let v = t[i + c] * 65535;
        v += kShadow * (1 - smooth01(l, 0, 0.5)) + kBlack * (1 - smooth01(l, 0, 0.25));
        v = mean * 65535 + (v - mean * 65535) * (1 + kSat);
        t[i + c] = clamp01(v / 65535);
      }
    }
    // --- 校准（shadowTint + 三原色） ---
    for (const [, ch, hue, sat] of primaries) {
      if (hue === 0 && sat === 0) continue;
      if (ch === -1) {
        // shadowTint
        const l = (t[i] + t[i + 1] + t[i + 2]) / 3;
        const w = 1 - smooth01(l, 0, 0.4);
        const dR = ((hue / 100) * 400 * 0.5 * w) / 65535;
        const dG = ((-hue / 100) * 400 * w) / 65535;
        t[i] = clamp01(t[i] + dR);
        t[i + 1] = clamp01(t[i + 1] + dG);
        t[i + 2] = clamp01(t[i + 2] + dR);
        continue;
      }
      const o1 = (ch + 1) % 3;
      const o2 = (ch + 2) % 3;
      const pxv = [t[i] * 65535, t[i + 1] * 65535, t[i + 2] * 65535];
      const mean = (pxv[0] + pxv[1] + pxv[2]) / 3;
      pxv[ch] += (pxv[o1] - pxv[o2]) * ((hue / 100) * 0.25);
      pxv[ch] = mean + (pxv[ch] - mean) * (1 + sat / 100);
      for (let c = 0; c < 3; ++c) t[i + c] = clamp01(pxv[c] / 65535);
    }
    // --- 饱和 / 自然饱和 ---
    if (vibrance !== 0 || saturation !== 0) {
      // 引擎注册序：vibrance 先于 saturation，两趟独立 clamp
      for (const [amt, vib] of [
        [vibrance, true],
        [saturation, false],
      ] as [number, boolean][]) {
        if (amt === 0) continue;
        const mean = (t[i] + t[i + 1] + t[i + 2]) / 3;
        let factor: number;
        if (vib) {
          const mx = Math.max(t[i], t[i + 1], t[i + 2]) * 65535;
          const mn = Math.min(t[i], t[i + 1], t[i + 2]) * 65535;
          const sat = (mx - mn) / (mx + mn + 1);
          factor = 1 + (amt / 100) * (1 - sat) * 0.6;
        } else {
          factor = 1 + amt / 100;
        }
        for (let c = 0; c < 3; ++c) {
          // 引擎 SaturationOp：v = mean + (code - mean)*factor（码域）→ t 域同式
          t[i + c] = clamp01(mean + (t[i + c] - mean) * factor);
        }
      }
    }
    // --- S3 toneMap（Reinhard，唯一域分界） ---
    for (let c = 0; c < 3; ++c) {
      const lin = log16Decode(t[i + c] * 65535);
      t[i + c] = log16Encode(lin / (1 + lin)) / 65535;
    }
    // --- S4 点曲线：引擎注册序 RGB复合 → R → G → B，逐条在最新码值上顺序作用 ---
    for (let c = 0; c < 3; ++c) {
      const applyOne = (lut: Float64Array): void => {
        const u = t[i + c] * 255;
        const idx = Math.max(0, Math.min(255, Math.round(u)));
        t[i + c] = clamp01(lut[idx] / 255);
      };
      if (!idRgb) applyOne(lutRgb);
      if (c === 0 && !idR) applyOne(lutR);
      else if (c === 1 && !idG) applyOne(lutG);
      else if (c === 2 && !idB) applyOne(lutB);
    }
    // --- S4 参数曲线 ---
    if (pmHi !== 0 || pmLi !== 0 || pmDa !== 0 || pmSh !== 0) {
      for (let c = 0; c < 3; ++c) {
        const u = t[i + c] * 255;
        const t0 = smooth01(u, 0, s0);
        const t1 = smooth01(u, s0, sL);
        const t2 = smooth01(u, sL, s1);
        const wHi = t2;
        const wLi = t1 - t2;
        const wDa = t0 - t1;
        const wSh = 1 - t0;
        const delta = ((pmHi * wHi + pmLi * wLi + pmDa * wDa + pmSh * wSh) / 100) * 60;
        const v = Math.max(0, Math.min(255, u + delta));
        t[i + c] = v / 255;
      }
    }
    // --- S4 HSL 8 带 ---
    let r = t[i];
    let g = t[i + 1];
    let b = t[i + 2];
    const [h0, s0h, l0] = rgbToHsl(r, g, b);
    if (s0h > 1e-6) {
      let dHue = 0;
      let dSat = 0;
      let dLum = 0;
      for (let bi = 0; bi < 8; ++bi) {
        const bk = bands?.[bandKeys[bi] as keyof typeof bands];
        const bh = num(bk?.hue, 0);
        const bs = num(bk?.sat, 0);
        const bl = num(bk?.lum, 0);
        if (bh === 0 && bs === 0 && bl === 0) continue;
        let dist = Math.abs(h0 - bandCenters[bi]);
        if (dist > 0.5) dist = 1 - dist;
        const w = Math.max(0, 1 - dist / (1 / 12));
        if (w <= 0) continue;
        dHue += (bh / 100) * (30 / 360) * w;
        dSat += (bs / 100) * w;
        dLum += (bl / 100) * w * 0.3;
      }
      let h = h0 + dHue;
      if (h < 0) h += 1;
      if (h >= 1) h -= 1;
      const s = Math.max(0, Math.min(1, s0h * (1 + dSat)));
      const l = Math.max(0, Math.min(1, l0 + dLum * 0.3));
      const rgb = hslToRgb(h, s, l);
      r = clamp01(rgb[0]);
      g = clamp01(rgb[1]);
      b = clamp01(rgb[2]);
      t[i] = Math.round(r * 65535) / 65535;
      t[i + 1] = Math.round(g * 65535) / 65535;
      t[i + 2] = Math.round(b * 65535) / 65535;
    }
    // --- S4 颜色分级 ---
    if (gradeActive) {
      const [hg, sg, lg] = rgbToHsl(t[i], t[i + 1], t[i + 2]);
      if (sg > 1e-6 || gShs !== 0 || gMs !== 0 || gHs !== 0) {
        const wHi = smooth01(lg, 0.5 - bBalance - spread / 2, 0.5 - bBalance + spread / 2);
        const wSh = 1 - smooth01(lg, 0.35 - bBalance - spread / 2, 0.35 - bBalance + spread / 2);
        const wMid = Math.max(0, 1 - wHi - wSh);
        const tz: [number, number, number][] = [
          [gSh, gShs, wSh],
          [gMh, gMs, wMid],
          [gHh, gHs, wHi],
        ];
        let dHue = 0;
        let dSat = 0;
        for (const [hue, sat, w] of tz) {
          if (w <= 0 || sat === 0) continue;
          const target = hue / 360;
          let d = target - hg;
          if (d > 0.5) d -= 1;
          if (d < -0.5) d += 1;
          const strength = Math.min(1, Math.abs(sat) / 100) * w;
          dHue += d * strength * 0.5;
          dSat += (sat / 100) * w * 0.3;
        }
        let h = hg + dHue;
        if (h < 0) h += 1;
        if (h >= 1) h -= 1;
        const s = Math.max(0, Math.min(1, sg * (1 + dSat)));
        const rgb = hslToRgb(h, s, lg);
        t[i] = Math.round(clamp01(rgb[0]) * 65535) / 65535;
        t[i + 1] = Math.round(clamp01(rgb[1]) * 65535) / 65535;
        t[i + 2] = Math.round(clamp01(rgb[2]) * 65535) / 65535;
      }
    }
    void x;
    void y;
  }
}

/** S5 锐化：unsharp + masking（engine detail.cpp SharpenOp 精确镜像） */
function applySharpen(p: EditParams, buf: PlaneBuf): void {
  const amount = num(p.detail?.sharpness, 0);
  if (amount === 0) return;
  const radius = Math.max(1, num(p.detail?.sharpenRadius, 1));
  const detail = num(p.detail?.sharpenDetail, 25);
  const masking = num(p.detail?.sharpenMasking, 0);
  const { w, h, t } = buf;
  const r = Math.max(1, Math.round(radius));
  const k = (amount / 100) * 0.9;
  const dBoost = 1 + (detail / 100) * 0.5;
  const threshold = (masking / 100) * 30;
  const plane = new Float32Array(w * h);
  const blurred = new Float32Array(w * h);
  for (let c = 0; c < 3; ++c) {
    for (let j = 0; j < w * h; ++j) plane[j] = t[j * 3 + c] * 65535;
    boxBlurPlane(plane, blurred, w, h, r);
    for (let j = 0; j < w * h; ++j) {
      const resid = (plane[j] - blurred[j]) * dBoost;
      const delta = Math.abs(resid) < threshold ? 0 : resid * k;
      t[j * 3 + c] = clamp01((plane[j] + delta) / 65535);
    }
  }
}

/** S6 暗角 + 颗粒（码域，engine effects.cpp 精确镜像；颗粒噪源 = hashNoise 块） */
function applyEffects(
  p: EditParams,
  buf: PlaneBuf,
  noise?: { grain: Float32Array; gw: number; gh: number; size: number } | null,
): void {
  const optics = p.optics;
  const amount = num(optics?.vignetteAmount, 0);
  if (amount !== 0) {
    const midpoint = num(optics?.vignetteMidpoint, 50) / 100;
    const feather = Math.max(1, num(optics?.vignetteFeather, 50));
    const { w, h, t } = buf;
    const cx = (w - 1) / 2;
    const cy = (h - 1) / 2;
    const maxR = Math.sqrt(cx * cx + cy * cy);
    for (let y = 0; y < h; ++y) {
      for (let x = 0; x < w; ++x) {
        const r = Math.sqrt((x - cx) * (x - cx) + (y - cy) * (y - cy)) / maxR;
        const tt = smooth01(r, midpoint, midpoint + feather / 100);
        const f = Math.max(0, Math.min(2, 1 + (amount / 100) * tt));
        const q = (y * w + x) * 3;
        for (let c = 0; c < 3; ++c) t[q + c] = clamp01(t[q + c] * f);
      }
    }
  }
  const gAmount = num(optics?.grainAmount, 0);
  if (gAmount !== 0 && noise) {
    const size = Math.max(1, num(optics?.grainSize, 0));
    const freq = num(optics?.grainFrequency, 0);
    const amp = ((gAmount / 100) * 1500 * (1 + (freq / 100) * 0.5)) / 65535;
    const { w, h, t } = buf;
    for (let y = 0; y < h; ++y) {
      for (let x = 0; x < w; ++x) {
        const bx = Math.trunc(x / size);
        const by = Math.trunc(y / size);
        const nVal = (hashNoise(bx, by, 7) * 2 - 1) * amp;
        const q = (y * w + x) * 3;
        for (let c = 0; c < 3; ++c) t[q + c] = clamp01(t[q + c] + nVal);
      }
    }
  }
}

/** S7 输出编码：sRGB + 确定性 TPDF（与 GPU 同噪源；引擎 rand() 偏差见回报） */
export function encode8(buf: PlaneBuf): Uint8Array {
  const out = new Uint8Array(buf.t.length);
  const { w, h, t } = buf;
  for (let y = 0; y < h; ++y) {
    for (let x = 0; x < w; ++x) {
      const q = (y * w + x) * 3;
      for (let c = 0; c < 3; ++c) {
        const lin = log16Decode(t[q + c] * 65535);
        const enc = linearToSrgb(lin);
        const d = tpdfDither(x, y, c);
        out[q + c] = Math.max(0, Math.min(255, Math.floor(enc * 255 + 0.5 + d)));
      }
    }
  }
  return out;
}

/** 4 条曲线 → LUT 纹理数据 + 恒等跳过标志（pipeline.setCurves 契约；
 *  renderer 与 parity harness 共用，保证两侧曲线口径一致） */
export function buildCurveLutData(p: EditParams): {
  data: Float32Array;
  skip: boolean[];
} {
  const luts = [
    lutFromPoints(p.toneCurveRGB),
    lutFromPoints(p.toneCurveR),
    lutFromPoints(p.toneCurveG),
    lutFromPoints(p.toneCurveB),
  ];
  const isId = (lut: Float64Array): boolean => {
    for (let i = 0; i < 256; ++i) if (Math.abs(lut[i] - i) > 1e-9) return false;
    return true;
  };
  const data = new Float32Array(256 * 4);
  for (let i = 0; i < 256; ++i) {
    for (let c = 0; c < 4; ++c) {
      data[i * 4 + c] = Math.max(0, Math.min(255, luts[c][i]));
    }
  }
  return { data, skip: luts.map(isId) };
}

/** 颗粒噪声表（engine hashNoise 块噪声；GPU 以纹理同源上传） */
export function buildGrainNoise(
  w: number,
  h: number,
  size: number,
): { grain: Float32Array; gw: number; gh: number; size: number } {
  const gw = Math.max(1, Math.ceil(w / size));
  const gh = Math.max(1, Math.ceil(h / size));
  const grain = new Float32Array(gw * gh);
  for (let y = 0; y < gh; ++y) for (let x = 0; x < gw; ++x) grain[y * gw + x] = hashNoise(x, y, 7);
  return { grain, gw, gh, size };
}

/** 调试快照（parity 排查用）：记录指定像素各阶段 t */
export interface StageSnapshot {
  src: number[];
  afterToneColor: number[];
  final: number[];
}

/**
 * 93 叶全管线 oracle。输入 = log16 码 t 域平面（由 makeDomainGradient / 预览底图
 * 统一产生）。返回最终 t 域与 8bit 输出。dbgPixel 提供时记录该像素阶段快照。
 */
export function oracle93(
  p: EditParams,
  src: PlaneBuf,
  withNoise = true,
  dbgPixel?: { x: number; y: number },
): OracleResult & { dbg?: StageSnapshot } {
  const snapIdx = dbgPixel ? (dbgPixel.y * src.w + dbgPixel.x) * 3 : -1;
  const snap = (arr: Float32Array): number[] =>
    snapIdx >= 0 ? [arr[snapIdx], arr[snapIdx + 1], arr[snapIdx + 2]] : [];
  const dbg: StageSnapshot | undefined = dbgPixel
    ? { src: snap(src.t), afterToneColor: [], final: [] }
    : undefined;
  // S0
  const s0: PlaneBuf = { t: src.t.slice(), w: src.w, h: src.h };
  applyWb(p, s0.t);
  // S1
  const s1 = applyGeometryStage(p, s0);
  // S1.5
  applyDenoise(p, s1);
  // S2 局部对比（clarity → texture 顺序两步，各带独立模糊）
  const clarityAmt = num(p.clarity, 0);
  const textureAmt = num(p.texture, 0);
  const localContrast = (amt: number, radius: number, gain: number): void => {
    if (amt === 0) return;
    const { w, h, t } = s1;
    const plane = new Float32Array(w * h);
    const blurred = new Float32Array(w * h);
    const k = (amt / 100) * gain;
    for (let c = 0; c < 3; ++c) {
      for (let j = 0; j < w * h; ++j) plane[j] = t[j * 3 + c] * 65535;
      boxBlurPlane(plane, blurred, w, h, radius);
      for (let j = 0; j < w * h; ++j) {
        t[j * 3 + c] = clamp01((plane[j] + k * (plane[j] - blurred[j])) / 65535);
      }
    }
  };
  localContrast(clarityAmt, 10, 0.6);
  localContrast(textureAmt, 2, 0.8);
  // S2 逐像素 + S3 + S4
  applyToneColor(p, s1);
  if (dbg) dbg.afterToneColor = snap(s1.t);
  // S5
  applySharpen(p, s1);
  // S6
  const gAmount = num(p.optics?.grainAmount, 0);
  const grainSize = Math.max(1, num(p.optics?.grainSize, 0));
  const noise =
    withNoise && gAmount !== 0 ? buildGrainNoise(s1.w, s1.h, grainSize) : null;
  applyEffects(p, s1, noise);
  // S7
  if (dbg) dbg.final = snap(s1.t);
  return { t: s1.t, w: s1.w, h: s1.h, out8: encode8(s1), dbg };
}
