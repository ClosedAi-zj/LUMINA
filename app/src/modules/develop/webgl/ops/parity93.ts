/**
 * T1.4 · 全 93 叶 parity harness（CPU oracle vs GPU 管线，架构 §10 门槛）。
 *
 * 用法（smoke 场景内）：new Parity93Runner(canvas).run() → ParityReport。
 * - 测试图：64×48，HSL 彩色域 × 12 级对数亮度 span（覆盖 log16 动态 + 色相/饱和变化）。
 * - 每叶一 case：默认参数 + 该叶代表值；CPU = ops/oracle93（引擎 T1.3 逐式镜像），
 *   GPU = ops/pipeline（WebGL float FBO 多 pass）。
 * - 对比域：S7 输出 8bit（TPDF 双侧同噪源）；门槛 mean<0.5 / max≤3 LSB（8bit LSB，
 *   比 T0.6 的 log16 LSB 更严，不放宽）。
 * - exit code 机制：smoke 场景输出 `parity93: allPass=...`，失败由 CI 判失败。
 */

import type { EditParams, CurvePoint, Hsl, HslBand } from '../../../../api/protocol';
import { log16Encode, tFromLinear } from './log16';
import {
  oracle93,
  buildCurveLutData,
  lutFromPoints,
  type PlaneBuf,
} from './oracle93';
import { PipelineGL } from './pipeline';
import { buildFieldAtlas, FIELD_COLS, FIELD_ROWS } from '../maskField';

const W = 64;
const H = 48;

export interface ParityCase {
  leaf: string;
  mean: number;
  max: number;
  pixels: number;
  pass: boolean;
  /** 诊断：双侧字节和（定位单侧无操作/翻转类系统性偏差） */
  dbg?: string;
}

export interface ParityReport {
  gpuAvailable: boolean;
  thresholds: { meanLsb: number; maxLsb: number };
  cases: ParityCase[];
  allPass: boolean;
  worstMean: number;
  worstMax: number;
}

const HSL_KEYS: HslBand[] = ['red', 'orange', 'yellow', 'green', 'aqua', 'blue', 'purple', 'magenta'];

export function defaultParams(): EditParams {
  const hsl = {} as Record<HslBand, Hsl>;
  for (const k of HSL_KEYS) hsl[k] = { hue: 0, sat: 0, lum: 0 };
  return {
    processVersion: '15.4',
    whiteBalance: 'asShot',
    exposure: 0,
    contrast: 0,
    highlights: 0,
    shadows: 0,
    whites: 0,
    blacks: 0,
    clarity: 0,
    texture: 0,
    dehaze: 0,
    vibrance: 0,
    saturation: 0,
    toneCurveRGB: [],
    hsl,
    colorGrade: { shadowHue: 0, shadowSat: 0, midtoneHue: 0, midtoneSat: 0, highlightHue: 0, highlightSat: 0, blend: 50, balance: 0 },
    calibration: { shadowTint: 0, redHue: 0, redSat: 0, greenHue: 0, greenSat: 0, blueHue: 0, blueSat: 0 },
    detail: { sharpness: 0, sharpenRadius: 1, sharpenDetail: 25, sharpenMasking: 0, luminanceSmoothing: 0, colorNR: 0, colorNRDetail: 50 },
    optics: { lensProfileEnable: false, manualDistortion: 0, chromaticAberration: 0, vignetteAmount: 0, vignetteMidpoint: 50, vignetteFeather: 50, grainAmount: 0, grainFrequency: 0, grainSize: 0 },
    geometry: { hasCrop: false, cropTop: 0, cropBottom: 100, cropLeft: 0, cropRight: 100, cropAngle: 0, perspective: { vertical: 0, horizontal: 0, rotate: 0, scale: 1, aspect: 0, x: 0, y: 0 } },
    masks: [],
  };
}

type CaseMutator = (p: EditParams) => void;

interface LeafCase {
  leaf: string;
  mut: CaseMutator;
}

/** 93 叶逐叶用例（叶路径口径 = docs/param-leaves.json） */
export function buildLeafCases(): LeafCase[] {
  const set = (fn: (p: EditParams) => void): CaseMutator => fn;
  const curve = (pts: [number, number][]): CurvePoint[] => pts.map(([x, y]) => ({ x, y }));
  const cases: LeafCase[] = [];
  const add = (leaf: string, mut: CaseMutator): void => {
    cases.push({ leaf, mut });
  };
  // basic 14
  add('basic.whiteBalance', set((p) => { p.whiteBalance = 'custom'; }));
  add('basic.temperature', set((p) => { p.whiteBalance = 'custom'; p.temperature = 6200; }));
  add('basic.tint', set((p) => { p.whiteBalance = 'custom'; p.tint = 30; }));
  add('basic.exposure', set((p) => { p.exposure = 0.7; }));
  add('basic.contrast', set((p) => { p.contrast = 40; }));
  add('basic.highlights', set((p) => { p.highlights = 50; }));
  add('basic.shadows', set((p) => { p.shadows = 50; }));
  add('basic.whites', set((p) => { p.whites = 50; }));
  add('basic.blacks', set((p) => { p.blacks = -40; }));
  add('basic.clarity', set((p) => { p.clarity = 40; }));
  add('basic.texture', set((p) => { p.texture = 50; }));
  add('basic.dehaze', set((p) => { p.dehaze = 30; }));
  add('basic.vibrance', set((p) => { p.vibrance = 40; }));
  add('basic.saturation', set((p) => { p.saturation = 30; }));
  // curve 11
  add('curve.toneCurveRGB', set((p) => { p.toneCurveRGB = curve([[0, 0], [64, 90], [192, 200], [255, 255]]); }));
  add('curve.toneCurveR', set((p) => { p.toneCurveR = curve([[0, 0], [128, 170], [255, 255]]); }));
  add('curve.toneCurveG', set((p) => { p.toneCurveG = curve([[0, 0], [128, 90], [255, 255]]); }));
  add('curve.toneCurveB', set((p) => { p.toneCurveB = curve([[0, 20], [255, 235]]); }));
  add('parametric.highlights', set((p) => { p.parametric = { highlights: 50, lights: 0, darks: 0, shadows: 0, shadowSplit: 25, lightSplit: 50, midSplit: 75 }; }));
  add('parametric.lights', set((p) => { p.parametric = { highlights: 0, lights: 40, darks: 0, shadows: 0, shadowSplit: 25, lightSplit: 50, midSplit: 75 }; }));
  add('parametric.darks', set((p) => { p.parametric = { highlights: 0, lights: 0, darks: -40, shadows: 0, shadowSplit: 25, lightSplit: 50, midSplit: 75 }; }));
  add('parametric.shadows', set((p) => { p.parametric = { highlights: 0, lights: 0, darks: 0, shadows: 50, shadowSplit: 25, lightSplit: 50, midSplit: 75 }; }));
  add('parametric.shadowSplit', set((p) => { p.parametric = { highlights: 0, lights: 0, darks: 0, shadows: 0, shadowSplit: 40, lightSplit: 50, midSplit: 75 }; }));
  add('parametric.lightSplit', set((p) => { p.parametric = { highlights: 0, lights: 0, darks: 0, shadows: 0, shadowSplit: 25, lightSplit: 60, midSplit: 75 }; }));
  add('parametric.midSplit', set((p) => { p.parametric = { highlights: 0, lights: 0, darks: 0, shadows: 0, shadowSplit: 25, lightSplit: 50, midSplit: 60 }; }));
  // hsl 24
  const bandValues: [number, number, number][] = [
    [40, 0, 0], [0, 50, 0], [0, 0, 40],
    [-30, 0, 0], [0, -40, 0], [0, 0, -30],
  ];
  HSL_KEYS.forEach((band, bi) => {
    const [h, s, l] = bandValues[bi % bandValues.length];
    add(`hsl.${band}.hue`, set((p) => { p.hsl[band].hue = h; }));
    add(`hsl.${band}.sat`, set((p) => { p.hsl[band].sat = s; }));
    add(`hsl.${band}.lum`, set((p) => { p.hsl[band].lum = l; }));
  });
  // colorGrade 8
  add('colorGrade.shadowHue', set((p) => { p.colorGrade.shadowHue = 30; p.colorGrade.shadowSat = 40; }));
  add('colorGrade.shadowSat', set((p) => { p.colorGrade.shadowHue = 200; p.colorGrade.shadowSat = 40; }));
  add('colorGrade.midtoneHue', set((p) => { p.colorGrade.midtoneHue = 120; p.colorGrade.midtoneSat = 40; }));
  add('colorGrade.midtoneSat', set((p) => { p.colorGrade.midtoneHue = 60; p.colorGrade.midtoneSat = 30; }));
  add('colorGrade.highlightHue', set((p) => { p.colorGrade.highlightHue = 220; p.colorGrade.highlightSat = 40; }));
  add('colorGrade.highlightSat', set((p) => { p.colorGrade.highlightHue = 40; p.colorGrade.highlightSat = 30; }));
  add('colorGrade.blend', set((p) => { p.colorGrade.midtoneHue = 120; p.colorGrade.midtoneSat = 50; p.colorGrade.blend = 20; }));
  add('colorGrade.balance', set((p) => { p.colorGrade.midtoneHue = 200; p.colorGrade.midtoneSat = 50; p.colorGrade.balance = -40; }));
  // calibration 7
  add('calibration.shadowTint', set((p) => { p.calibration.shadowTint = 40; }));
  add('calibration.redHue', set((p) => { p.calibration.redHue = 40; }));
  add('calibration.redSat', set((p) => { p.calibration.redSat = 40; }));
  add('calibration.greenHue', set((p) => { p.calibration.greenHue = 40; }));
  add('calibration.greenSat', set((p) => { p.calibration.greenSat = 40; }));
  add('calibration.blueHue', set((p) => { p.calibration.blueHue = 40; }));
  add('calibration.blueSat', set((p) => { p.calibration.blueSat = 40; }));
  // detail 7
  add('detail.colorNR', set((p) => { p.detail.colorNR = 60; p.detail.colorNRDetail = 30; }));
  add('detail.colorNRDetail', set((p) => { p.detail.colorNR = 60; p.detail.colorNRDetail = 90; }));
  add('detail.luminanceSmoothing', set((p) => { p.detail.luminanceSmoothing = 60; }));
  add('detail.sharpness', set((p) => { p.detail.sharpness = 60; }));
  add('detail.sharpenRadius', set((p) => { p.detail.sharpness = 60; p.detail.sharpenRadius = 2; }));
  add('detail.sharpenDetail', set((p) => { p.detail.sharpness = 60; p.detail.sharpenDetail = 80; }));
  add('detail.sharpenMasking', set((p) => { p.detail.sharpness = 80; p.detail.sharpenMasking = 60; }));
  // lens 2
  add('optics.lensProfileEnable', set((p) => { p.optics.lensProfileEnable = true; }));
  add('optics.manualDistortion', set((p) => { p.optics.manualDistortion = 40; }));
  // effects 6
  add('optics.vignetteAmount', set((p) => { p.optics.vignetteAmount = -50; }));
  add('optics.vignetteMidpoint', set((p) => { p.optics.vignetteAmount = -50; p.optics.vignetteMidpoint = 25; }));
  add('optics.vignetteFeather', set((p) => { p.optics.vignetteAmount = -50; p.optics.vignetteFeather = 90; }));
  add('optics.grainAmount', set((p) => { p.optics.grainAmount = 30; p.optics.grainSize = 2; p.optics.grainFrequency = 20; }));
  add('optics.grainFrequency', set((p) => { p.optics.grainAmount = 30; p.optics.grainSize = 2; p.optics.grainFrequency = 80; }));
  add('optics.grainSize', set((p) => { p.optics.grainAmount = 30; p.optics.grainSize = 6; }));
  // geometry 13
  add('geometry.hasCrop', set((p) => { p.geometry.hasCrop = true; p.geometry.cropLeft = 10; p.geometry.cropTop = 10; p.geometry.cropRight = 90; p.geometry.cropBottom = 90; }));
  add('geometry.cropLeft', set((p) => { p.geometry.hasCrop = true; p.geometry.cropLeft = 15; p.geometry.cropRight = 100; p.geometry.cropBottom = 100; }));
  add('geometry.cropTop', set((p) => { p.geometry.hasCrop = true; p.geometry.cropTop = 20; p.geometry.cropRight = 100; p.geometry.cropBottom = 100; }));
  add('geometry.cropRight', set((p) => { p.geometry.hasCrop = true; p.geometry.cropRight = 80; p.geometry.cropBottom = 100; }));
  add('geometry.cropBottom', set((p) => { p.geometry.hasCrop = true; p.geometry.cropBottom = 75; }));
  add('geometry.cropAngle', set((p) => { p.geometry.hasCrop = true; p.geometry.cropAngle = 5; }));
  add('geometry.perspective.vertical', set((p) => { p.geometry.perspective.vertical = 20; }));
  add('geometry.perspective.horizontal', set((p) => { p.geometry.perspective.horizontal = 20; }));
  add('geometry.perspective.rotate', set((p) => { p.geometry.perspective.rotate = 10; }));
  add('geometry.perspective.scale', set((p) => { p.geometry.perspective.scale = 1.3; }));
  add('geometry.perspective.aspect', set((p) => { p.geometry.perspective.scale = 1.2; p.geometry.perspective.aspect = 10; }));
  add('geometry.perspective.x', set((p) => { p.geometry.perspective.x = 15; }));
  add('geometry.perspective.y', set((p) => { p.geometry.perspective.y = 15; }));
  return cases;
}

/** 测试图：12 级对数亮度 span × 色相横向渐变（彩色 + log16 动态覆盖） */
export function makeTestImage(): PlaneBuf {
  const t = new Float32Array(W * H * 3);
  for (let y = 0; y < H; ++y) {
    for (let x = 0; x < W; ++x) {
      const i = y * W + x;
      const span = i % 12;
      const lum = tFromLinear(Math.pow(2, -9 + (span / 11) * 16)); // 2^-9..2^7
      const hue = (x / W) % 1;
      const sat = 0.35 + 0.45 * (y / H);
      // HSL → RGB（简单色相条带，与 oracle rgbToHsl 互逆即可）
      const [r, g, b] = hslToRgbHost(hue, sat, lum);
      const q = i * 3;
      t[q] = log16Encode(r) / 65535;
      t[q + 1] = log16Encode(g) / 65535;
      t[q + 2] = log16Encode(b) / 65535;
    }
  }
  return { t, w: W, h: H };
}

function hslToRgbHost(h: number, s: number, l: number): [number, number, number] {
  if (s <= 1e-12) return [l, l, l];
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

/** 4 条曲线 → LUT 纹理数据 + 恒等标志（pipeline.setCurves 契约） */
export function buildCurveTexture(p: EditParams): {
  data: Float32Array;
  skip: boolean[];
} {
  return buildCurveLutData(p);
}

/** parity 执行器：复用一个 WebGL 上下文跑全部 case */
export class Parity93Runner {
  pipeline = new PipelineGL();
  private gl: WebGLRenderingContext | null = null;
  src: PlaneBuf = makeTestImage();
  /** 调试开关：失败用例追加 tone FBO t 值 */
  dbgTone = true;
  /** 最近一次 runOne 的 CPU 8bit 输出（runMaskSuite 伪绿防御用） */
  lastCpu8: Uint8Array | null = null;

  get gpu(): boolean {
    return this.gl !== null;
  }

  init(canvas: HTMLCanvasElement): boolean {
    const gl = canvas.getContext('webgl', {
      preserveDrawingBuffer: true,
      antialias: false,
    }) as WebGLRenderingContext | null;
    if (!gl) return false;
    if (!this.pipeline.init(gl)) return false;
    if (!this.pipeline.setSourceCodes(this.src.t, this.src.w, this.src.h)) return false;
    this.gl = gl;
    return true;
  }

  run(): ParityReport {
    const cases: ParityCase[] = [];
    let allPass = true;
    let worstMean = 0;
    let worstMax = 0;
    if (!this.gl) {
      return { gpuAvailable: false, thresholds: { meanLsb: 0.5, maxLsb: 3 }, cases, allPass: false, worstMean: 0, worstMax: 0 };
    }
    for (const { leaf, mut } of buildLeafCases()) {
      const params = defaultParams();
      mut(params);
      const c = this.runOne(leaf, params);
      cases.push(c);
      allPass = allPass && c.pass;
      if (c.mean > worstMean) worstMean = c.mean;
      if (c.max > worstMax) worstMax = c.max;
    }
    return {
      gpuAvailable: true,
      thresholds: { meanLsb: 0.5, maxLsb: 3 },
      cases,
      allPass,
      worstMean,
      worstMax,
    };
  }

  /** 单 case 执行：CPU oracle vs GPU 管线，8bit 输出逐像素对比 */
  runOne(leaf: string, params: EditParams): ParityCase {
    const gl = this.gl!;
    const canvas = gl.canvas as HTMLCanvasElement;
    const t0 = performance.now();
    // CPU
    const cpu = oracle93(params, this.src);
    this.lastCpu8 = cpu.out8;
    const t1 = performance.now();
    // GPU
    const curve = buildCurveTexture(params);
    this.pipeline.setCurves(curve.data, curve.skip);
    const ok = this.pipeline.render(canvas, params, null, null);
    const t2 = performance.now();
    const w = canvas.width;
    const h = canvas.height;
    let mean = -1;
    let max = -1;
    let dbg = '';
    if (ok && w === cpu.w && h === cpu.h) {
      const px = new Uint8Array(w * h * 4);
      gl.readPixels(0, 0, w, h, gl.RGBA, gl.UNSIGNED_BYTE, px);
      let sum = 0;
      let mx = 0;
      let count = 0;
      let sumGpu = 0;
      let sumCpu = 0;
      for (let y = 0; y < h; ++y) {
        for (let x = 0; x < w; ++x) {
          // composite 已翻 uA/uB 采样（画布呈现方向 = 源图方向），readPixels 行 0 =
          // GL 帧缓冲底行 = 图像底行 → 对 CPU 行序做 y 翻转后再比
          const gy = h - 1 - y;
          const gIdx = (gy * w + x) * 4;
          const cIdx = (y * w + x) * 3;
          for (let c = 0; c < 3; ++c) {
            const d = Math.abs(px[gIdx + c] - cpu.out8[cIdx + c]);
            sum += d;
            if (d > mx) mx = d;
            sumGpu += px[gIdx + c];
            sumCpu += cpu.out8[cIdx + c];
            ++count;
          }
        }
      }
      mean = sum / count;
      max = mx;
      // 诊断：取差异最大的前 5 像素
      const worst: { x: number; y: number; g: number; c: number }[] = [];
      for (let y = 0; y < h; ++y) {
        for (let x = 0; x < w; ++x) {
          const gy = h - 1 - y;  // 与上方主对比同口径（composite 翻转后的行序对齐）
          const gIdx = (gy * w + x) * 4;
          const cIdx = (y * w + x) * 3;
          for (let c = 0; c < 3; ++c) {
            const d = Math.abs(px[gIdx + c] - cpu.out8[cIdx + c]);
            if (d > 3) {
              worst.push({ x, y, g: px[gIdx + c], c: cpu.out8[cIdx + c] });
              if (worst.length > 40) worst.shift();
            }
          }
        }
      }
      dbg =
        `gpuSum=${sumGpu} cpuSum=${sumCpu} worst=` +
        worst.slice(0, 5).map((p) => `(${p.x},${p.y}):${p.g}/${p.c}`).join(' ');
      // 诊断：tone FBO 的 t（GPU）与 8bit 对比域之外的真值
      if (worst.length > 0 && this.dbgTone) {
        const tone = this.pipeline.debugReadTone();
        if (tone && tone.w === w && tone.h === h) {
          const p0 = worst[0];
          const ti = (p0.y * w + p0.x) * 4;
          dbg += ` toneT(${p0.x},${p0.y})=[${[0, 1, 2].map((k) => tone.data[ti + k].toFixed(6)).join(',')}];srcT=[${[0, 1, 2].map((k) => this.src.t[(p0.y * this.src.w + p0.x) * 3 + k].toFixed(6)).join(',')}]`;
        }
      }
    }
    const pass = mean >= 0 && mean < 0.5 && max <= 3;
    if (window.lumina?.isSmoke) {
      console.log(
        `[smoke-scenario] parity93-run: ${leaf} mean=${mean.toFixed(4)} max=${max.toFixed(0)} pass=${pass} ` +
          `cpuMs=${(t1 - t0).toFixed(0)} gpuMs=${(t2 - t1).toFixed(0)} cmpMs=${(performance.now() - t2).toFixed(0)}`,
      );
    }
    return { leaf, mean, max, pixels: w * h, pass, dbg: dbg || undefined };
  }
}

// ---- T2.7 蒙版 parity（A3/A4 验收；CPU = oracle applyMasksOracle vs GPU = mask pass）----

export interface MaskParityReport {
  gpuAvailable: boolean;
  thresholds: { meanLsb: number; maxLsb: number };
  cases: ParityCase[];
  allPass: boolean;
  worstMean: number;
  worstMax: number;
  /** 伪绿防御：各 case CPU 侧相对无蒙版基线的变化像素数（蒙版必须实际作用） */
  changed: number[];
}

const LOCAL_FULL = {
  exposure: 25,
  contrast: 20,
  highlights: 30,
  shadows: -25,
  temperature: 15,
  tint: -10,
  saturation: 20,
  clarity: 30,
  dehaze: 10,
};

/** 画笔测试笔画：两笔交叉 + 一笔擦除（覆盖 buildBrushField 的 max/乘法衰减两分支） */
const BRUSH_STROKES = [
  { x: 0.25, y: 0.35, radius: 0.13, flow: 90, density: 100, erase: false },
  { x: 0.45, y: 0.45, radius: 0.13, flow: 90, density: 100, erase: false },
  { x: 0.65, y: 0.55, radius: 0.13, flow: 90, density: 100, erase: false },
  { x: 0.55, y: 0.5, radius: 0.1, flow: 60, density: 100, erase: true },
];

/** A3 用例矩阵：3 蒙版类型 × 3 槽数 × 3 combine = 27 组；外加 range 2 组 + 20 槽 1 组 */
export function buildMaskCases(): { name: string; masks: EditParams['masks'] }[] {
  const types = ['linear', 'radial', 'brush'] as const;
  const combines = ['replace', 'intersect', 'subtract'] as const;
  const slotCounts = [1, 2, 3];
  const cases: { name: string; masks: EditParams['masks'] }[] = [];
  const mkMask = (
    type: (typeof types)[number],
    i: number,
    combine: (typeof combines)[number],
  ): EditParams['masks'][number] => ({
    id: `m${i}`,
    type,
    enabled: true,
    opacity: 80,
    amount: 100,
    combine,
    geometry: type === 'linear'
      ? { x1: 0.05 + i * 0.1, y1: 0.1, x2: 0.75 + i * 0.08, y2: 0.9 }
      : { cx: 0.3 + i * 0.18, cy: 0.4 + i * 0.1, radius: 0.34 + i * 0.05, ratio: 1 },
    strokes: type === 'brush' ? BRUSH_STROKES : undefined,
    local: { ...LOCAL_FULL },
  });
  for (const type of types) {
    for (const n of slotCounts) {
      for (const combine of combines) {
        const masks = [];
        for (let i = 0; i < n; ++i) {
          // 首条固定 replace（fold 语义：combine 对首条无意义）；后续用组 combine
          masks.push(mkMask(type, i, i === 0 ? 'replace' : combine));
        }
        cases.push({ name: `mask.${type}.n${n}.${combine}`, masks });
      }
    }
  }
  // 范围蒙版（T2.7 新增类型的 parity 覆盖；不占 27 组名额）
  cases.push({
    name: 'mask.luminanceRange',
    masks: [{
      id: 'lr', type: 'luminanceRange', enabled: true, opacity: 100, amount: 100,
      combine: 'replace',
      range: { low: 0.35, high: 0.75, samples: [], smooth: 0.12 },
      local: { ...LOCAL_FULL },
    } as EditParams['masks'][number]],
  });
  cases.push({
    name: 'mask.colorRange',
    masks: [{
      id: 'cr', type: 'colorRange', enabled: true, opacity: 100, amount: 100,
      combine: 'replace',
      range: { hueCenter: 0.45, hueWidth: 0.5, satLow: 0.05, satHigh: 1.0, smooth: 0.15 } as unknown as EditParams['masks'][number]['range'],
      local: { ...LOCAL_FULL },
    } as EditParams['masks'][number]],
  });
  // A4：20 槽（GPU 32 槽容量内全链路 fold）
  {
    const masks = [];
    for (let i = 0; i < 20; ++i) {
      masks.push({
        id: `s${i}`,
        type: 'linear',
        enabled: true,
        opacity: 70,
        amount: 100,
        combine: i % 3 === 1 ? 'intersect' : i % 3 === 2 ? 'subtract' : 'replace',
        geometry: { x1: i * 0.024, y1: 0.05, x2: 0.35 + i * 0.03, y2: 0.95 },
        local: { ...LOCAL_FULL, exposure: 8 + i * 1.5 },
      } as EditParams['masks'][number]);
    }
    cases.push({ name: 'mask.slots20', masks });
  }
  return cases;
}

/** 蒙版 parity 执行器（复用 Parity93Runner 的上下文；runMaskSuite 前须 init 成功） */
export function runMaskSuite(runner: Parity93Runner): MaskParityReport {
  const cases: ParityCase[] = [];
  const changed: number[] = [];
  let allPass = true;
  let worstMean = 0;
  let worstMax = 0;
  if (!runner.gpu) {
    return { gpuAvailable: false, thresholds: { meanLsb: 0.5, maxLsb: 3 }, cases, allPass: false, worstMean: 0, worstMax: 0, changed };
  }
  // 无蒙版基线（伪绿防御参照：蒙版必须相对基线产生实际变化）
  const base = oracle93(defaultParams(), runner.src);
  for (const { name, masks } of buildMaskCases()) {
    const params = defaultParams();
    params.masks = masks;
    // 画笔图集（GPU brush 因子数据；与 renderer 同一 builder）
    const atlas = buildFieldAtlas(masks);
    runner.pipeline.setFieldAtlas(atlas.bytes, FIELD_COLS, FIELD_ROWS);
    const c = runner.runOne(name, params);
    // CPU 侧实际作用断言：相对无蒙版基线的变化像素数（8bit 域）
    let ch = 0;
    const cpu8 = runner.lastCpu8;
    if (cpu8) {
      for (let i = 0; i < cpu8.length; ++i) {
        if (cpu8[i] !== base.out8[i]) ++ch;
      }
    }
    changed.push(ch);
    const pass = c.pass && ch >= 300;  // 蒙版未生效（changed≈0）= 假绿，判失败
    c.pass = pass;
    cases.push(c);
    allPass = allPass && pass;
    if (c.mean > worstMean) worstMean = c.mean;
    if (c.max > worstMax) worstMax = c.max;
    if (window.lumina?.isSmoke) {
      console.log(`[smoke-scenario] mask-parity-run: ${name} changed=${ch} pass=${pass}`);
    }
  }
  return { gpuAvailable: true, thresholds: { meanLsb: 0.5, maxLsb: 3 }, cases, allPass, worstMean, worstMax, changed };
}
