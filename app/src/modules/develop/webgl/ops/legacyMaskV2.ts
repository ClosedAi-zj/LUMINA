/**
 * T2.12 差异报告专用 · 旧 v2 composite 蒙版解释的 CPU 逐字镜像（不入渲染管线）。
 *
 * 背景（架构 §5 T2.12 / §1.4 语义裁决 #2）：蒙版语义从 v2 composite（commit 054eb2c
 * 之前的末道内联，display 域近似）升级为 v3 独立 mask pass（严格 w=f·opacity·amount，
 * 引擎域 9 叶）。存量含蒙版照片会以新口径重新解释 → 必须量化「旧解释 vs 新解释」的
 * 观感差异进差异报告（{home}/reports/m2-visual-diff.json），无静默改观感。
 *
 * 本文件是 **被移除的旧 shader 的档案镜像**，逐式来源（054eb2c~1）：
 *   - glsl.ts 旧版 applyMaskCore / applyMask0..5 / rgb2hslM / hsl2rgbM / smoothstep5
 *   - renderer.ts 旧版蒙版上传（enabled 过滤 + slice(0,6)，超出 6 槽静默截断）
 *   - pipeline.ts 旧版 pass 位置（composite = 末道，sRGB 编码之后 → display 域）
 * 旧解释的已知语义缺陷（正是本次升级的动因，报告的量化对象）：
 *   ① amount 被混入曝光公式（exposure + amount*0.5），而非强度倍率；
 *   ② amount=0 的蒙版其余 6 叶仍然生效（引擎读端 v3 下 amount=0 → w=0 完全不施加）；
 *   ③ 只有 7 个局部叶参与（tint/dehaze 静默丢弃）；
 *   ④ 仅前 6 个 enabled 蒙版参与，超出静默截断；
 *   ⑤ combine（replace/intersect/subtract）被忽略，一律顺序叠加；
 *   ⑥ display 域近似（在 sRGB 编码后施加），与引擎域逐式公式不同。
 */
import type { MaskLayer } from '../../../../api/protocol';
import { FIELD_CELL, buildBrushField } from '../maskField';

/** 旧 renderer MAX_MASKS = 6（超出静默截断） */
const LEGACY_MAX_MASKS = 6;

function smoothstep5(edge0: number, edge1: number, x: number): number {
  const t = Math.max(0, Math.min(1, (x - edge0) / (edge1 - edge0)));
  return t * t * (3 - 2 * t);
}

function rgb2hslM(r: number, g: number, b: number): [number, number, number] {
  const mx = Math.max(r, g, b);
  const mn = Math.min(r, g, b);
  const l = (mx + mn) * 0.5;
  let h = 0;
  let s = 0;
  const d = mx - mn;
  if (d > 1e-5) {
    s = l > 0.5 ? d / (2 - mx - mn) : d / (mx + mn);
    if (mx === r) h = (g - b) / d + (g < b ? 6 : 0);
    else if (mx === g) h = (b - r) / d + 2;
    else h = (r - g) / d + 4;
    h /= 6;
  }
  return [h, s, l];
}

function hue2rgbM(p: number, q: number, t0: number): number {
  let t = t0;
  if (t < 0) t += 1;
  if (t > 1) t -= 1;
  if (t < 1 / 6) return p + (q - p) * 6 * t;
  if (t < 0.5) return q;
  if (t < 2 / 3) return q + (p - q) * (2 / 3 - t) * 6;
  return p;
}

function hsl2rgbM(hsl: [number, number, number]): [number, number, number] {
  const [h, s, l] = hsl;
  if (s <= 1e-5) return [l, l, l];
  const q = l < 0.5 ? l * (1 + s) : l + s - l * s;
  const p = 2 * l - q;
  return [hue2rgbM(p, q, h + 1 / 3), hue2rgbM(p, q, h), hue2rgbM(p, q, h - 1 / 3)];
}

interface LegacyMask {
  type: 0 | 1 | 2; // 0=linear 1=radial 2=brush（旧 shader type 编码）
  ax: number;
  ay: number; // linear：线起点（图像空间，顶=0）；radial：圆心
  bx: number;
  by: number; // linear：线终点；radial：radius=bx, ratio=by（旧 maskGeo 打包）
  l0: [number, number, number, number]; // exposure contrast highlights shadows
  l1: [number, number, number, number]; // temperature tint saturation clarity
  amount: number; // 0..100（旧口径缺省 100）
  opacity: number; // 0..100
  field: Float32Array | null; // brush 因子场（256×256，顶=0）
}

/** 旧 renderer.ts maskGeo + 蒙版上传语义（enabled 过滤 + slice(0,6) + amount 缺省 100） */
export function legacyMaskSetV2(masks: MaskLayer[]): {
  used: LegacyMask[];
  truncated: number; // 被 6 槽静默截断的 enabled 蒙版数
} {
  const enabled = masks.filter((m) => m.enabled);
  const usedMasks = enabled.slice(0, LEGACY_MAX_MASKS);
  const used: LegacyMask[] = usedMasks.map((m) => {
    // 旧 renderer type 编码：linear=0 / radial=1 / 其余（brush、aiSky、luminanceRange
    // 等 v3 新类型）一律 2 = 图集采样分支。旧图集只为 brush 槽位填充场，其余槽位
    // 字节恒 0 → factor=0 → v3 新类型在旧解释下完全惰性（逐字镜像，非疏漏）。
    if (m.type === 'linear' && m.geometry && 'x1' in m.geometry) {
      return {
        type: 0 as const,
        ax: m.geometry.x1,
        ay: m.geometry.y1,
        bx: m.geometry.x2,
        by: m.geometry.y2,
        l0: [m.local.exposure, m.local.contrast, m.local.highlights, m.local.shadows],
        l1: [m.local.temperature, m.local.tint, m.local.saturation, m.local.clarity],
        amount: m.amount ?? 100,
        opacity: m.opacity,
        field: null,
      };
    }
    if (m.type === 'radial' && m.geometry && 'cx' in m.geometry) {
      return {
        type: 1 as const,
        ax: m.geometry.cx,
        ay: m.geometry.cy,
        bx: m.geometry.radius,
        by: m.geometry.ratio,
        l0: [m.local.exposure, m.local.contrast, m.local.highlights, m.local.shadows],
        l1: [m.local.temperature, m.local.tint, m.local.saturation, m.local.clarity],
        amount: m.amount ?? 100,
        opacity: m.opacity,
        field: null,
      };
    }
    return {
      type: 2 as const,
      ax: 0,
      ay: 0,
      bx: 0,
      by: 0,
      l0: [m.local.exposure, m.local.contrast, m.local.highlights, m.local.shadows],
      l1: [m.local.temperature, m.local.tint, m.local.saturation, m.local.clarity],
      amount: m.amount ?? 100,
      opacity: m.opacity,
      field: m.type === 'brush' ? buildBrushField(m.strokes ?? []) : null, // null → factor 0
    };
  });
  return { used, truncated: enabled.length - usedMasks.length };
}

/** 旧 applyMaskCore：display 域（sRGB 0..1）上逐像素施加，uv 为图像空间（顶=0） */
function applyMaskCore(
  c0: [number, number, number],
  u: number,
  v: number,
  m: LegacyMask,
): [number, number, number] {
  let factor = 0;
  if (m.type === 0) {
    const dx = m.bx - m.ax;
    const dy = m.by - m.ay;
    const len2 = Math.max(dx * dx + dy * dy, 1e-5);
    const t = ((u - m.ax) * dx + (v - m.ay) * dy) / len2;
    factor = Math.max(0, Math.min(1, t));
  } else if (m.type === 1) {
    const dxu = u - m.ax;
    const dyv = (v - m.ay) / Math.max(m.by, 0.1);
    factor = 1 - smoothstep5(m.bx * 0.55, m.bx, Math.hypot(dxu, dyv));
  } else if (m.field) {
    // 旧 shader 经 3×2 图集中转（fuv = ((cell.x+uv.x)/3, (cell.y+1-uv.y)/2)），
    // 净效果 = 图像空间直接双线性采样（cell 偏移与 v 翻转互相抵消，见设计注释）
    const fx = Math.max(0, Math.min(FIELD_CELL - 1, u * FIELD_CELL - 0.5));
    const fy = Math.max(0, Math.min(FIELD_CELL - 1, v * FIELD_CELL - 0.5));
    const x0 = Math.floor(fx);
    const y0 = Math.floor(fy);
    const x1 = Math.min(FIELD_CELL - 1, x0 + 1);
    const y1 = Math.min(FIELD_CELL - 1, y0 + 1);
    const tx = fx - x0;
    const ty = fy - y0;
    const f = m.field;
    const s00 = f[y0 * FIELD_CELL + x0];
    const s10 = f[y0 * FIELD_CELL + x1];
    const s01 = f[y1 * FIELD_CELL + x0];
    const s11 = f[y1 * FIELD_CELL + x1];
    factor =
      s00 * (1 - tx) * (1 - ty) + s10 * tx * (1 - ty) + s01 * (1 - tx) * ty + s11 * tx * ty;
  }
  factor *= m.opacity / 100;
  if (factor <= 0.001) return c0;
  const amount = m.amount / 100;
  let [r, g, b] = c0;
  const lum = 0.2126 * r + 0.7152 * g + 0.0722 * b;
  // 旧口径核心 quirk：amount 混入曝光公式（exposure + amount*0.5）
  const ev = Math.pow(2, (m.l0[0] + amount * 0.5) * factor * 0.8);
  r *= ev;
  g *= ev;
  b *= ev;
  const hw = smoothstep5(0.45, 1.0, lum);
  const sw = 1 - smoothstep5(0.0, 0.55, lum);
  r += (m.l0[2] / 100) * 0.4 * hw * factor * (1 - r);
  g += (m.l0[2] / 100) * 0.4 * hw * factor * (1 - g);
  b += (m.l0[2] / 100) * 0.4 * hw * factor * (1 - b);
  r += (m.l0[3] / 100) * 0.4 * sw * factor * (1 - r);
  g += (m.l0[3] / 100) * 0.4 * sw * factor * (1 - g);
  b += (m.l0[3] / 100) * 0.4 * sw * factor * (1 - b);
  r = (r - 0.5) * (1 + (m.l0[1] / 100) * 0.5 * factor) + 0.5;
  g = (g - 0.5) * (1 + (m.l0[1] / 100) * 0.5 * factor) + 0.5;
  b = (b - 0.5) * (1 + (m.l0[1] / 100) * 0.5 * factor) + 0.5;
  // temperature（旧 shader：r+/b- 对推，tint 键被读进 l1[1] 但从未使用 → 静默丢弃）
  r += (m.l1[0] / 100) * 0.1 * factor;
  b -= (m.l1[0] / 100) * 0.1 * factor;
  r = Math.max(0, Math.min(1, r));
  g = Math.max(0, Math.min(1, g));
  b = Math.max(0, Math.min(1, b));
  // saturation（tint/dehaze 无对应分支 → 静默丢弃）
  const hsl = rgb2hslM(r, g, b);
  hsl[1] *= 1 + (m.l1[2] / 100) * 0.6 * factor;
  [r, g, b] = hsl2rgbM(hsl);
  r = Math.max(0, Math.min(1, r));
  g = Math.max(0, Math.min(1, g));
  b = Math.max(0, Math.min(1, b));
  // clarity（中调对比近似）
  const mid = 1 - Math.abs(lum - 0.5) * 2;
  r = (r - 0.5) * (1 + (m.l1[3] / 100) * 0.3 * mid * factor) + 0.5;
  g = (g - 0.5) * (1 + (m.l1[3] / 100) * 0.3 * mid * factor) + 0.5;
  b = (b - 0.5) * (1 + (m.l1[3] / 100) * 0.3 * mid * factor) + 0.5;
  return [Math.max(0, Math.min(1, r)), Math.max(0, Math.min(1, g)), Math.max(0, Math.min(1, b))];
}

/**
 * 旧 v2 composite 逐字镜像：在「无蒙版渲染结果」的 display 域像素上
 * 按旧管线顺序（末道、sRGB 域、前 6 个 enabled、顺序叠加）施加蒙版。
 * px: RGBA（就地修改）；w/h 为像素尺寸。
 */
export function applyLegacyCompositeV2(
  px: Uint8ClampedArray,
  w: number,
  h: number,
  masks: MaskLayer[],
): void {
  const { used } = legacyMaskSetV2(masks);
  for (let py = 0; py < h; py++) {
    const v = (py + 0.5) / h;
    for (let pxi = 0; pxi < w; pxi++) {
      const u = (pxi + 0.5) / w;
      let c: [number, number, number] = [
        px[(py * w + pxi) * 4] / 255,
        px[(py * w + pxi) * 4 + 1] / 255,
        px[(py * w + pxi) * 4 + 2] / 255,
      ];
      for (const m of used) {
        c = applyMaskCore(c, u, v, m);
      }
      const o = (py * w + pxi) * 4;
      px[o] = Math.round(c[0] * 255);
      px[o + 1] = Math.round(c[1] * 255);
      px[o + 2] = Math.round(c[2] * 255);
    }
  }
}
