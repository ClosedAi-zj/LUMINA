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
    optics: { lensProfileEnable: false, manualDistortion: 0, vignetteAmount: 0, vignetteMidpoint: 50, vignetteFeather: 50, grainAmount: 0, grainFrequency: 0, grainSize: 0 },
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
  private pipeline = new PipelineGL();
  private gl: WebGLRenderingContext | null = null;
  private src: PlaneBuf = makeTestImage();
  /** 调试开关：失败用例追加 tone FBO t 值 */
  dbgTone = true;

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
    const canvas = this.gl.canvas as HTMLCanvasElement;
    for (const { leaf, mut } of buildLeafCases()) {
      const params = defaultParams();
      mut(params);
      const t0 = performance.now();
      // CPU
      const cpu = oracle93(params, this.src);
      const t1 = performance.now();
      // GPU
      const curve = buildCurveTexture(params);
      this.pipeline.setCurves(curve.data, curve.skip);
      const masks = {
        count: 0,
        geoA: new Float32Array(24),
        geoB: new Float32Array(24),
        local0: new Float32Array(24),
        local1: new Float32Array(24),
        amount: new Float32Array(6),
        type: new Float32Array(6),
        opacity: new Float32Array(6),
      };
      const ok = this.pipeline.render(canvas, params, null, null, masks);
      const t2 = performance.now();
      const w = canvas.width;
      const h = canvas.height;
      let mean = -1;
      let max = -1;
      let dbg = '';
      if (ok && w === cpu.w && h === cpu.h) {
        const px = new Uint8Array(w * h * 4);
        this.gl.readPixels(0, 0, w, h, this.gl.RGBA, this.gl.UNSIGNED_BYTE, px);
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
      allPass = allPass && pass;
      if (mean > worstMean) worstMean = mean;
      if (max > worstMax) worstMax = max;
      cases.push({ leaf, mean, max, pixels: w * h, pass, dbg: dbg || undefined });
      if (window.lumina?.isSmoke) {
        console.log(
          `[smoke-scenario] parity93-run: ${leaf} mean=${mean.toFixed(4)} max=${max.toFixed(0)} pass=${pass} ` +
            `cpuMs=${(t1 - t0).toFixed(0)} gpuMs=${(t2 - t1).toFixed(0)} cmpMs=${(performance.now() - t2).toFixed(0)}`,
        );
      }
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
}
