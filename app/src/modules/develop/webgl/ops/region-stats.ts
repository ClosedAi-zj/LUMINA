/**
 * M2 T2.1 · 分区统计器（GPU 侧同式实现）
 *
 * 与引擎 `engine/tests/support/region_stats.{h,cpp}` **逐字同式**：
 * 同一批指标、同一套阈值、同一套「生效 / 无操作」互斥语义。
 * 用途：smoke ⑦d「导出 = 预览（含蒙版）」读回像素后，用同一口径判定
 * 「蒙版局部调整是否真的生效」，而不是拿 8bit 整幅均值糊过去（K2/K3 教训）。
 *
 * 口径（与引擎一致，改这里必须同步改引擎，否则 ⑦d 判据会分叉）：
 *   亮度   : CIE Lab 的 L-star → lum = L-star 除以 100 再乘 255（0..255，≈ 8bit 码 LSB）
 *   色度   : CIE Lab 的 a-star 与 b-star（显示 sRGB 反算，D65 白点）
 *   局部方差: 16×16 块方差均值（三通道方差取平均）
 *   边缘能量: Sobel 幅值均值（亮度平面）
 *   桶划分 : 亮度四分位，桶归属由**基线**固定，cur 复用基线的归属
 *
 * 归属：app/src/modules/develop/webgl/ops/**（只读像素，不参与渲染）。
 */

/** 5×5 网格中的一格 */
export interface RegionCell {
  meanLum: number;
  meanA: number;
  meanB: number;
  varLocal: number;
  edgeEnergy: number;
  count: number;
}

/** 亮度四分位桶（[0] = 最暗） */
export interface RegionBucket {
  meanLum: number;
  meanA: number;
  meanB: number;
  varLocal: number;
  edgeEnergy: number;
  count: number;
}

/** 一幅图的完整分区统计（逐像素平面用于「基线桶归属」复用） */
export interface RegionStats {
  w: number;
  h: number;
  cells: RegionCell[];
  quartiles: RegionBucket[];
  lumPlane: Float64Array;
  aPlane: Float64Array;
  bPlane: Float64Array;
  varPlane: Float64Array;
  edgePlane: Float64Array;
  quartileOf: Uint8Array;
  globalMeanLum: number;
  chromaVar: number;
}

/** 差分结果（字段与引擎 LeafDelta 一一对应） */
export interface LeafDelta {
  maxCellLumDelta: number;
  maxCellChromaDelta: number;
  maxCellChromaDeltaAbs: number;
  varRelChange: number;
  edgeRelChange: number;
  quartileLumDelta: [number, number, number, number];
  globalMeanDelta: number;
  globalMaxDelta: number;
  chromaVarRelChange: number;
  worstCell: number;
}

export const GRID_COLS = 5;
export const GRID_ROWS = 5;
export const QUARTILE_COUNT = 4;
export const VAR_BLOCK = 16;
export const NO_OP_FACTOR = 0.5;

function srgbToLinear(v: number): number {
  if (v <= 0) return 0;
  if (v <= 0.04045) return v / 12.92;
  return Math.pow((v + 0.055) / 1.055, 2.4);
}

function labF(t: number): number {
  const kEps = 216 / 24389;
  const kKappa = 24389 / 27;
  if (t > kEps) return Math.cbrt(t);
  return (kKappa * t + 16) / 116;
}

/** 显示 sRGB（0..255）→ CIE Lab（D65） */
function srgbToLab(r8: number, g8: number, b8: number): [number, number, number] {
  const r = srgbToLinear(r8 / 255);
  const g = srgbToLinear(g8 / 255);
  const b = srgbToLinear(b8 / 255);
  const X = 0.4124564 * r + 0.3575761 * g + 0.1804375 * b;
  const Y = 0.2126729 * r + 0.7151522 * g + 0.072175 * b;
  const Z = 0.0193339 * r + 0.119192 * g + 0.9503041 * b;
  const fx = labF(X / 0.95047);
  const fy = labF(Y / 1.0);
  const fz = labF(Z / 1.08883);
  return [116 * fy - 16, 500 * (fx - fy), 200 * (fy - fz)];
}

function planeMean(v: Float64Array): number {
  if (v.length === 0) return 0;
  let s = 0;
  for (let i = 0; i < v.length; i += 1) s += v[i];
  return s / v.length;
}

function planeVarianceAll(v: Float64Array): number {
  if (v.length === 0) return 0;
  const mean = planeMean(v);
  let acc = 0;
  for (let i = 0; i < v.length; i += 1) {
    const d = v[i] - mean;
    acc += d * d;
  }
  return acc / v.length;
}

function planeVarianceAt(v: Float64Array, idx: number[]): number {
  if (idx.length === 0) return 0;
  let s = 0;
  for (const i of idx) s += v[i];
  const mean = s / idx.length;
  let acc = 0;
  for (const i of idx) {
    const d = v[i] - mean;
    acc += d * d;
  }
  return acc / idx.length;
}

function gridBoundary(i: number, n: number): number {
  return Math.floor((i * n) / GRID_COLS);
}

/**
 * 计算分区统计。
 * @param rgb 交错 RGB 8bit 像素（长度必须 ≥ width*height*3）
 */
export function computeRegionStats(
  rgb: Uint8Array | Uint8ClampedArray,
  width: number,
  height: number,
): RegionStats {
  if (width <= 0 || height <= 0) throw new Error('region-stats: empty image');
  const n = width * height;
  if (rgb.length < n * 3) throw new Error('region-stats: pixel buffer too small');

  const rp = new Float64Array(n);
  const gp = new Float64Array(n);
  const bp = new Float64Array(n);
  const lumPlane = new Float64Array(n);
  const aPlane = new Float64Array(n);
  const bPlane = new Float64Array(n);
  for (let i = 0; i < n; i += 1) {
    rp[i] = rgb[i * 3];
    gp[i] = rgb[i * 3 + 1];
    bp[i] = rgb[i * 3 + 2];
    const [L, A, B] = srgbToLab(rp[i], gp[i], bp[i]);
    lumPlane[i] = (L / 100) * 255;
    aPlane[i] = A;
    bPlane[i] = B;
  }

  // 16×16 块方差（三通道平均，块内同值）
  const varPlane = new Float64Array(n);
  for (let by = 0; by < height; by += VAR_BLOCK) {
    for (let bx = 0; bx < width; bx += VAR_BLOCK) {
      const x1 = Math.min(width, bx + VAR_BLOCK);
      const y1 = Math.min(height, by + VAR_BLOCK);
      const idx: number[] = [];
      for (let y = by; y < y1; y += 1) {
        for (let x = bx; x < x1; x += 1) idx.push(y * width + x);
      }
      const v = (planeVarianceAt(rp, idx) + planeVarianceAt(gp, idx) + planeVarianceAt(bp, idx)) / 3;
      for (const i of idx) varPlane[i] = v;
    }
  }

  // Sobel（亮度平面；边界记 0）
  const edgePlane = new Float64Array(n);
  if (width >= 3 && height >= 3) {
    for (let y = 1; y < height - 1; y += 1) {
      for (let x = 1; x < width - 1; x += 1) {
        const p = y * width + x;
        const p00 = lumPlane[p - width - 1];
        const p01 = lumPlane[p - width];
        const p02 = lumPlane[p - width + 1];
        const p10 = lumPlane[p - 1];
        const p12 = lumPlane[p + 1];
        const p20 = lumPlane[p + width - 1];
        const p21 = lumPlane[p + width];
        const p22 = lumPlane[p + width + 1];
        const gx = p02 + 2 * p12 + p22 - (p00 + 2 * p10 + p20);
        const gy = p20 + 2 * p21 + p22 - (p00 + 2 * p01 + p02);
        edgePlane[p] = Math.sqrt(gx * gx + gy * gy);
      }
    }
  }

  // 亮度四分位桶归属（按基线亮度升序切 4 等份）
  const order = new Array<number>(n);
  for (let i = 0; i < n; i += 1) order[i] = i;
  order.sort((x, y) => lumPlane[x] - lumPlane[y]);
  const quartileOf = new Uint8Array(n);
  for (let rank = 0; rank < n; rank += 1) {
    let q = Math.floor((rank * QUARTILE_COUNT) / n);
    if (q < 0) q = 0;
    if (q > QUARTILE_COUNT - 1) q = QUARTILE_COUNT - 1;
    quartileOf[order[rank]] = q;
  }

  const cells: RegionCell[] = [];
  for (let gy = 0; gy < GRID_ROWS; gy += 1) {
    for (let gx = 0; gx < GRID_COLS; gx += 1) {
      const x0 = gridBoundary(gx, width);
      const x1 = gridBoundary(gx + 1, width);
      const y0 = gridBoundary(gy, height);
      const y1 = gridBoundary(gy + 1, height);
      const c: RegionCell = {
        meanLum: 0,
        meanA: 0,
        meanB: 0,
        varLocal: 0,
        edgeEnergy: 0,
        count: 0,
      };
      for (let y = y0; y < y1; y += 1) {
        for (let x = x0; x < x1; x += 1) {
          const i = y * width + x;
          c.meanLum += lumPlane[i];
          c.meanA += aPlane[i];
          c.meanB += bPlane[i];
          c.varLocal += varPlane[i];
          c.edgeEnergy += edgePlane[i];
          c.count += 1;
        }
      }
      if (c.count > 0) {
        const inv = 1 / c.count;
        c.meanLum *= inv;
        c.meanA *= inv;
        c.meanB *= inv;
        c.varLocal *= inv;
        c.edgeEnergy *= inv;
      }
      cells.push(c);
    }
  }

  const stats: RegionStats = {
    w: width,
    h: height,
    cells,
    quartiles: [],
    lumPlane,
    aPlane,
    bPlane,
    varPlane,
    edgePlane,
    quartileOf,
    globalMeanLum: planeMean(lumPlane),
    chromaVar: planeVarianceAll(aPlane) + planeVarianceAll(bPlane),
  };
  stats.quartiles = aggregateBuckets(stats, quartileOf);
  return stats;
}

function aggregateBuckets(stats: RegionStats, assign: Uint8Array): RegionBucket[] {
  const out: RegionBucket[] = [];
  for (let q = 0; q < QUARTILE_COUNT; q += 1) {
    out.push({ meanLum: 0, meanA: 0, meanB: 0, varLocal: 0, edgeEnergy: 0, count: 0 });
  }
  const n = stats.w * stats.h;
  for (let i = 0; i < n; i += 1) {
    const q = assign[i];
    if (q < 0 || q >= QUARTILE_COUNT) continue;
    const b = out[q];
    b.meanLum += stats.lumPlane[i];
    b.meanA += stats.aPlane[i];
    b.meanB += stats.bPlane[i];
    b.varLocal += stats.varPlane[i];
    b.edgeEnergy += stats.edgePlane[i];
    b.count += 1;
  }
  for (const b of out) {
    if (b.count <= 0) continue;
    const inv = 1 / b.count;
    b.meanLum *= inv;
    b.meanA *= inv;
    b.meanB *= inv;
    b.varLocal *= inv;
    b.edgeEnergy *= inv;
  }
  return out;
}

/** 差分：base 固定网格与桶划分，cur 复用 base 的桶归属（同一批像素） */
export function compareRegionStats(base: RegionStats, cur: RegionStats): LeafDelta {
  if (base.w !== cur.w || base.h !== cur.h || base.w <= 0 || base.h <= 0) {
    throw new Error('region-stats: size mismatch');
  }
  const n = base.w * base.h;
  if (base.cells.length !== GRID_COLS * GRID_ROWS || cur.cells.length !== base.cells.length) {
    throw new Error('region-stats: cell count mismatch');
  }
  const d: LeafDelta = {
    maxCellLumDelta: 0,
    maxCellChromaDelta: 0,
    maxCellChromaDeltaAbs: 0,
    varRelChange: 0,
    edgeRelChange: 0,
    quartileLumDelta: [0, 0, 0, 0],
    globalMeanDelta: 0,
    globalMaxDelta: 0,
    chromaVarRelChange: 0,
    worstCell: -1,
  };
  for (let i = 0; i < base.cells.length; i += 1) {
    const b = base.cells[i];
    const c = cur.cells[i];
    const dLum = c.meanLum - b.meanLum;
    const dA = c.meanA - b.meanA;
    const dB = c.meanB - b.meanB;
    const absLum = Math.abs(dLum);
    if (absLum > d.maxCellLumDelta || d.worstCell < 0) {
      d.maxCellLumDelta = absLum;
      d.worstCell = i;
    }
    const cb = Math.sqrt(b.meanA * b.meanA + b.meanB * b.meanB);
    const cc = Math.sqrt(c.meanA * c.meanA + c.meanB * c.meanB);
    const dC = cc - cb;
    if (Math.abs(dC) > Math.abs(d.maxCellChromaDelta)) d.maxCellChromaDelta = dC;
    const euclid = Math.sqrt(dA * dA + dB * dB);
    if (euclid > d.maxCellChromaDeltaAbs) d.maxCellChromaDeltaAbs = euclid;
  }
  const varBase = planeMean(base.varPlane);
  const varCur = planeMean(cur.varPlane);
  d.varRelChange = varBase > 1e-9 ? (varCur - varBase) / varBase : 0;
  const edgeBase = planeMean(base.edgePlane);
  const edgeCur = planeMean(cur.edgePlane);
  d.edgeRelChange = edgeBase > 1e-9 ? (edgeCur - edgeBase) / edgeBase : 0;
  const curBuckets = aggregateBuckets(cur, base.quartileOf);
  for (let q = 0; q < QUARTILE_COUNT; q += 1) {
    d.quartileLumDelta[q] = curBuckets[q].meanLum - base.quartiles[q].meanLum;
  }
  d.globalMeanDelta = Math.abs(cur.globalMeanLum - base.globalMeanLum);
  let maxPixel = 0;
  for (let i = 0; i < n; i += 1) {
    const diff = Math.abs(cur.lumPlane[i] - base.lumPlane[i]);
    if (diff > maxPixel) maxPixel = diff;
  }
  d.globalMaxDelta = maxPixel;
  d.chromaVarRelChange = base.chromaVar > 1e-9 ? (cur.chromaVar - base.chromaVar) / base.chromaVar : 0;
  return d;
}

/** 判据指标（与引擎 leaf_effect_table.h 的 Metric 枚举同名同义） */
export type Metric =
  | 'none'
  | 'maxCellLumDelta'
  | 'maxCellChromaDelta'
  | 'maxCellChromaDeltaAbs'
  | 'varRelChange'
  | 'edgeRelChange'
  | 'quartileLumDelta0'
  | 'globalMeanDelta'
  | 'chromaVarRelChange';

export type Direction = 'none' | 'increase' | 'decrease' | 'atMost';

export interface LeafCriterion {
  leaf: string;
  op: string;
  step: number;
  metric1: Metric;
  threshold1: number;
  dir1: Direction;
  metric2: Metric;
  threshold2: number;
  dir2: Direction;
}

/**
 * ⚠ CI 只读：阈值只许收紧，不许下调（S10）。
 * 本表必须与 `engine/tests/support/leaf_effect_table.h` 保持一致；
 * 任何一侧改动都要同步另一侧并说明收紧理由。
 *
 * 与架构 §3.6 的两处差异（均已上报，方向都是「更严」）：
 *   1. `detail.colorNR` 的第二个指标由 `maxCellChromaDelta` 改为 `chromaVarRelChange`：
 *      前者是欧氏幅度（恒 ≥ 0），无法表达「下降方向」；且实测逐通道块方差对零均值色噪
 *      不灵敏（降噪时反而 +14.6%），只有 a-star 与 b-star 的局部方差能真实反映色度降噪。
 *   2. `basic.tint` 用 `maxCellChromaDeltaAbs`：tint 是色相轴旋转，ΔC 符号随像素原色相
 *      变化（红黄区升、绿青区降），带符号值会判反；方向正确性由引擎侧
 *      `testHueAxisDirection` 在近中性灰图上单独断言。
 */
export const LEAF_CRITERIA: readonly LeafCriterion[] = [
  { leaf: 'basic.temperature', op: 'basic.temperature', step: 100, metric1: 'maxCellChromaDelta', threshold1: 0.6, dir1: 'increase', metric2: 'none', threshold2: 0, dir2: 'none' },
  { leaf: 'basic.tint', op: 'basic.tint', step: 100, metric1: 'maxCellChromaDeltaAbs', threshold1: 0.3, dir1: 'increase', metric2: 'none', threshold2: 0, dir2: 'none' },
  { leaf: 'basic.vibrance', op: 'basic.vibrance', step: 10, metric1: 'maxCellChromaDelta', threshold1: 1.5, dir1: 'increase', metric2: 'none', threshold2: 0, dir2: 'none' },
  { leaf: 'basic.saturation', op: 'basic.saturation', step: 10, metric1: 'maxCellChromaDelta', threshold1: 1.5, dir1: 'increase', metric2: 'none', threshold2: 0, dir2: 'none' },
  { leaf: 'calibration.blueSat', op: 'calibration.blue', step: 10, metric1: 'maxCellChromaDelta', threshold1: 1.5, dir1: 'increase', metric2: 'none', threshold2: 0, dir2: 'none' },
  { leaf: 'basic.clarity', op: 'basic.clarity', step: 10, metric1: 'varRelChange', threshold1: 0.03, dir1: 'increase', metric2: 'none', threshold2: 0, dir2: 'none' },
  { leaf: 'basic.texture', op: 'basic.texture', step: 10, metric1: 'varRelChange', threshold1: 0.03, dir1: 'increase', metric2: 'none', threshold2: 0, dir2: 'none' },
  { leaf: 'detail.sharpness', op: 'detail.sharpen', step: 10, metric1: 'edgeRelChange', threshold1: 0.05, dir1: 'increase', metric2: 'none', threshold2: 0, dir2: 'none' },
  { leaf: 'detail.colorNR', op: 'detail.denoise', step: 100, metric1: 'varRelChange', threshold1: -0.1, dir1: 'decrease', metric2: 'chromaVarRelChange', threshold2: -0.1, dir2: 'decrease' },
  { leaf: 'optics.grainAmount', op: 'effects.grain', step: 30, metric1: 'varRelChange', threshold1: 0.02, dir1: 'increase', metric2: 'globalMeanDelta', threshold2: 1, dir2: 'atMost' },
  { leaf: 'basic.dehaze', op: 'basic.dehaze', step: 60, metric1: 'quartileLumDelta0', threshold1: 2, dir1: 'increase', metric2: 'none', threshold2: 0, dir2: 'none' },
];

/** Metric → LeafDelta 取值 */
export function metricValue(d: LeafDelta, m: Metric): number {
  switch (m) {
    case 'maxCellLumDelta':
      return d.maxCellLumDelta;
    case 'maxCellChromaDelta':
      return d.maxCellChromaDelta;
    case 'maxCellChromaDeltaAbs':
      return d.maxCellChromaDeltaAbs;
    case 'varRelChange':
      return d.varRelChange;
    case 'edgeRelChange':
      return d.edgeRelChange;
    case 'quartileLumDelta0':
      return d.quartileLumDelta[0];
    case 'globalMeanDelta':
      return d.globalMeanDelta;
    case 'chromaVarRelChange':
      return d.chromaVarRelChange;
    case 'none':
      return 0;
    default:
      return 0;
  }
}

function conditionHolds(v: number, threshold: number, dir: Direction, noOp: boolean): boolean {
  switch (dir) {
    case 'increase':
      return noOp ? v <= threshold * NO_OP_FACTOR : v >= threshold;
    case 'decrease':
      return noOp ? v >= threshold * NO_OP_FACTOR : v <= threshold;
    case 'atMost':
      return noOp ? v <= threshold * NO_OP_FACTOR : v <= threshold;
    case 'none':
      return true;
    default:
      return true;
  }
}

/** 生效：所有条件按方向成立 */
export function assertEffective(d: LeafDelta, c: LeafCriterion): boolean {
  if (!conditionHolds(metricValue(d, c.metric1), c.threshold1, c.dir1, false)) return false;
  if (c.metric2 !== 'none') {
    if (!conditionHolds(metricValue(d, c.metric2), c.threshold2, c.dir2, false)) return false;
  }
  return true;
}

/** 无操作：所有指标 ≤ 阈值 × 0.5，且必须与 assertEffective 互斥（Q5 硬要求） */
export function assertNoOp(d: LeafDelta, c: LeafCriterion): boolean {
  if (assertEffective(d, c)) return false;
  if (!conditionHolds(metricValue(d, c.metric1), c.threshold1, c.dir1, true)) return false;
  if (c.metric2 !== 'none') {
    if (!conditionHolds(metricValue(d, c.metric2), c.threshold2, c.dir2, true)) return false;
  }
  return true;
}
