/**
 * 画笔蒙版逐笔距离场（WP-B5 / WP-B6.5）。
 *
 * 取代 v1.0 的「所有笔画质心 + 平均半径」单圆近似：把一笔画里的相邻印记连成折线，
 * 对每个场点取到各折线的距离，按引擎同款平滑衰减（半程 r/2 → r）合成因子。
 * GPU（shader 采样场纹理）、CPU（软渲染直接采样）与红色浮层（同一场数据上屏）
 * 三条路共用这一个实现——浮层边界与实际因子边界一致是靠"同一份数据"保证的。
 *
 * 场分辨率 FIELD×FIELD/格，最多 32 个蒙版槽（4 列 × 8 行图集，T2.7 6→32 扩容）。
 */

import type { MaskLayer } from '../../../api/protocol';

/** 每个蒙版格的边长（px） */
export const FIELD_CELL = 256;
/** 图集列数 / 行数（T2.7：32 个蒙版槽，数据纹理同容量） */
export const FIELD_COLS = 4;
export const FIELD_ROWS = 8;
/** GPU 蒙版槽容量（mask pass 数据纹理行数 = MAX_MASK_SLOTS × MASK_DATA_ROWS） */
export const MAX_MASK_SLOTS = 32;
/** 数据纹理每蒙版行数（见 buildMaskDataTex 行布局） */
export const MASK_DATA_ROWS = 6;

/** 因子衰减：d ≤ r/2 全强度，d ≥ r 为 0（与 v1.0 径向蒙版同款 smoothstep） */
function smoothstep5(edge0: number, edge1: number, x: number): number {
  const t = Math.max(0, Math.min(1, (x - edge0) / (edge1 - edge0)));
  return t * t * (3 - 2 * t);
}

interface StrokeSeg {
  x1: number;
  y1: number;
  x2: number;
  y2: number;
  radius: number;
  flow: number;
  erase: boolean;
}

/** 相邻印记连成线段；erase 标志翻转处断开（画与擦是两条折线，擦除走乘法衰减） */
function toSegments(strokes: NonNullable<MaskLayer['strokes']>): StrokeSeg[] {
  const segs: StrokeSeg[] = [];
  for (let i = 0; i < strokes.length; i++) {
    const cur = strokes[i];
    const prev = i > 0 ? strokes[i - 1] : null;
    const connect = prev !== null && prev.erase === cur.erase;
    segs.push({
      x1: connect ? prev!.x : cur.x,
      y1: connect ? prev!.y : cur.y,
      x2: cur.x,
      y2: cur.y,
      radius: cur.radius,
      flow: cur.flow,
      erase: cur.erase,
    });
  }
  return segs;
}

/** 点到线段距离（归一化坐标空间，与 v1.0 半径语义一致） */
function segDistance(px: number, py: number, s: StrokeSeg): number {
  const dx = s.x2 - s.x1;
  const dy = s.y2 - s.y1;
  const len2 = dx * dx + dy * dy;
  let t = 0;
  if (len2 > 1e-12) {
    t = ((px - s.x1) * dx + (py - s.y1) * dy) / len2;
    t = Math.max(0, Math.min(1, t));
  }
  const ex = px - (s.x1 + t * dx);
  const ey = py - (s.y1 + t * dy);
  return Math.sqrt(ex * ex + ey * ey);
}

/**
 * 构建单个画笔蒙版的因子场（行 0 = 画面顶部，值 0..1）。
 * 合成语义：画笔组取 max（叠加取深），擦除组做乘法衰减 factor *= 1-w。
 */
export function buildBrushField(strokes: NonNullable<MaskLayer['strokes']>): Float32Array {
  const field = new Float32Array(FIELD_CELL * FIELD_CELL);
  if (!strokes || strokes.length === 0) return field;
  const segs = toSegments(strokes);
  // 每段包围盒 + 半径外扩：像素级裁剪，免掉整场 × 全部线段的空算
  const boxes = segs.map((s) => ({
    minX: Math.min(s.x1, s.x2) - s.radius,
    maxX: Math.max(s.x1, s.x2) + s.radius,
    minY: Math.min(s.y1, s.y2) - s.radius,
    maxY: Math.max(s.y1, s.y2) + s.radius,
    seg: s,
  }));
  for (let py = 0; py < FIELD_CELL; py++) {
    const ny = (py + 0.5) / FIELD_CELL; // 画面纵向比例（顶=0）
    for (let px = 0; px < FIELD_CELL; px++) {
      const nx = (px + 0.5) / FIELD_CELL;
      let factor = 0;
      for (const b of boxes) {
        if (nx < b.minX || nx > b.maxX || ny < b.minY || ny > b.maxY) continue;
        const s = b.seg;
        const d = segDistance(nx, ny, s);
        const w = (1 - smoothstep5(s.radius * 0.5, s.radius, d)) * (s.flow / 100);
        if (w <= 0) continue;
        if (s.erase) factor *= 1 - w;
        else if (w > factor) factor = w;
      }
      field[py * FIELD_CELL + px] = factor;
    }
  }
  return field;
}

/** 多个画笔蒙版 → 图集字节（LUMINANCE 上传用；非画笔槽位为 0） */
export function buildFieldAtlas(masks: MaskLayer[]): { bytes: Uint8Array; brushSlots: boolean[] } {
  const bytes = new Uint8Array(FIELD_COLS * FIELD_CELL * FIELD_ROWS * FIELD_CELL);
  const brushSlots: boolean[] = [];
  masks.slice(0, MAX_MASK_SLOTS).forEach((m, i) => {
    brushSlots.push(m.type === 'brush');
    if (m.type !== 'brush' || !m.enabled) return;
    const field = buildBrushField(m.strokes ?? []);
    const col = i % FIELD_COLS;
    const row = Math.floor(i / FIELD_COLS);
    for (let y = 0; y < FIELD_CELL; y++) {
      const srcRow = y * FIELD_CELL;
      // 图集行主序：目标行 = row*CELL + y，列偏移 col*CELL
      const dstRow = (row * FIELD_CELL + y) * (FIELD_COLS * FIELD_CELL) + col * FIELD_CELL;
      for (let x = 0; x < FIELD_CELL; x++) {
        bytes[dstRow + x] = Math.max(0, Math.min(255, Math.round(field[srcRow + x] * 255)));
      }
    }
  });
  return { bytes, brushSlots };
}

/** 双线性采样图集上某个槽位的因子（CPU 软渲染与浮层共用；u/v 为画面归一化坐标，顶=0） */
export function sampleAtlas(atlas: Uint8Array, slot: number, u: number, v: number): number {
  const col = slot % FIELD_COLS;
  const row = Math.floor(slot / FIELD_COLS);
  const fx = Math.max(0, Math.min(FIELD_CELL - 1, u * FIELD_CELL - 0.5));
  const fy = Math.max(0, Math.min(FIELD_CELL - 1, v * FIELD_CELL - 0.5));
  const x0 = Math.floor(fx);
  const y0 = Math.floor(fy);
  const tx = fx - x0;
  const ty = fy - y0;
  const stride = FIELD_COLS * FIELD_CELL;
  const base = (row * FIELD_CELL + y0) * stride + col * FIELD_CELL;
  const x1 = Math.min(FIELD_CELL - 1, x0 + 1);
  const y1 = Math.min(FIELD_CELL - 1, y0 + 1) * stride;
  const a = atlas[base + x0];
  const b = atlas[base + x1];
  const c = atlas[base + y1 + x0];
  const d = atlas[base + y1 + x1];
  return ((a * (1 - tx) + b * tx) * (1 - ty) + (c * (1 - tx) + d * tx) * ty) / 255;
}

// ---- T2.7 mask pass 数据纹理（32 槽 × 6 行 RGBA32F；GPU mask pass 与 CPU oracle
// 共用同一打包，避免 uniforms 数组常量索引限制）----
// 行布局（行 k = slot*6 + k）：
//   r0 = (type, combine, enabled, opacity/100)
//   r1 = (amount/100, p1, p2, p3)
//   r2 = (p4, p5, p6, p7)
//   r3 = (exposure, contrast, highlights, shadows)
//   r4 = (temperature, tint, saturation, clarity)
//   r5 = (dehaze, 0, 0, 0)
// 参数打包（p1..p7，未用位补 0）：
//   linear(0)          p1..p4 = x1, y1, x2, y2
//   radial(1)          p1..p5 = cx, cy, radius, ratio, feather
//   brush(2)           无（画笔因子走图集纹理）
//   luminanceRange(3)  p1..p3 = low, high, smooth
//   colorRange(4)      p1..p5 = hueCenter, hueWidth, satLow, satHigh, smooth

const clampN = (v: number, lo: number, hi: number): number => Math.max(lo, Math.min(hi, v));

const geoNum = (o: object | undefined, key: string, def: number): number => {
  const v = (o as Record<string, number | undefined> | undefined)?.[key];
  return typeof v === 'number' && Number.isFinite(v) ? v : def;
};

/** 蒙版 local 是否需要共享 clarity 模糊平面（enabled + clarity ≠ 0） */
export function maskNeedClarityBlur(m: MaskLayer): boolean {
  return m.enabled && m.type !== 'depth' && m.type !== 'aiSky' &&
    m.type !== 'aiSubject' && m.type !== 'aiBackground' &&
    (m.local?.clarity ?? 0) !== 0;
}

export function buildMaskDataTex(masks: MaskLayer[]): { data: Float32Array; count: number } {
  const data = new Float32Array(MAX_MASK_SLOTS * MASK_DATA_ROWS * 4);
  const list = masks.slice(0, MAX_MASK_SLOTS);
  list.forEach((m, i) => {
    const o = i * MASK_DATA_ROWS * 4;
    const type = m.type === 'linear' ? 0 : m.type === 'radial' ? 1 : m.type === 'brush' ? 2
      : m.type === 'luminanceRange' ? 3 : m.type === 'colorRange' ? 4 : -1;
    const combine = m.combine === 'intersect' ? 1 : m.combine === 'subtract' ? 2 : 0;
    data[o + 0] = type;
    data[o + 1] = combine;
    data[o + 2] = m.enabled && type >= 0 ? 1 : 0;  // 不支持类型按 disabled 透传（预览侧；引擎侧显式失败）
    data[o + 3] = clampN(m.opacity ?? 100, 0, 100) / 100;
    data[o + 4] = clampN(m.amount ?? 100, -100, 100) / 100;
    const g = m.geometry as Record<string, number> | undefined;
    const rg = m.range as Record<string, number> | undefined;
    if (type === 0) {
      data[o + 5] = geoNum(g, 'x1', 0);
      data[o + 6] = geoNum(g, 'y1', 0);
      data[o + 7] = geoNum(g, 'x2', 1);
      data[o + 8] = geoNum(g, 'y2', 1);
    } else if (type === 1) {
      data[o + 5] = geoNum(g, 'cx', 0.5);
      data[o + 6] = geoNum(g, 'cy', 0.5);
      data[o + 7] = geoNum(g, 'radius', 0.4);
      data[o + 8] = geoNum(g, 'ratio', 1);
      data[o + 9] = geoNum(g, 'feather', 0.45);
    } else if (type === 3) {
      data[o + 5] = geoNum(rg, 'low', 0);
      data[o + 6] = geoNum(rg, 'high', 1);
      data[o + 7] = clampN(geoNum(rg, 'smooth', 0.1), 0, 1);
    } else if (type === 4) {
      data[o + 5] = geoNum(rg, 'hueCenter', 0);
      data[o + 6] = geoNum(rg, 'hueWidth', 0.1);
      data[o + 7] = geoNum(rg, 'satLow', 0);
      data[o + 8] = geoNum(rg, 'satHigh', 1);
      data[o + 9] = clampN(geoNum(rg, 'smooth', 0.1), 0, 1);
    }
    const lo = m.local ?? ({} as MaskLayer['local']);
    data[o + 12] = lo.exposure ?? 0;
    data[o + 13] = lo.contrast ?? 0;
    data[o + 14] = lo.highlights ?? 0;
    data[o + 15] = lo.shadows ?? 0;
    data[o + 16] = lo.temperature ?? 0;
    data[o + 17] = lo.tint ?? 0;
    data[o + 18] = lo.saturation ?? 0;
    data[o + 19] = lo.clarity ?? 0;
    data[o + 20] = lo.dehaze ?? 0;
  });
  return { data, count: list.length };
}
