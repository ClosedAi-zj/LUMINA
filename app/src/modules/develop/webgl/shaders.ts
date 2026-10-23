/**
 * WebGL 参数近似渲染 · 着色器（架构 §1.2：UI 侧逐参数近似管线，切换快速/专业零解码零闪黑）。
 * 单 pass fragment shader；前/后对比通过 uSplit 在片元内混合两组参数。
 *
 * GLSL ES 1.0 兼容性红线（曾导致修图台整体黑屏，2026-10-22 修复）：
 *  1) uniform 数组只允许「常量索引」或「无 break/continue 的可展开 for 循环索引」；
 *     旧代码 `for (...) { if (i >= uMaskCount) break; ... }` 的 break 让循环不可展开，
 *     编译直接失败（ERROR: 0:122: '[]' : Index expression must be constant），
 *     init() 返回 false → canvas 永远停在默认 300x150 空白 → 黑屏。
 *     现改为：循环体不含 break，用 float active 因子屏蔽未启用的蒙版槽位。
 *  2) 函数参数不支持数组（float p[12]），改为 12 个标量参数的核心函数，A/B 两组各包一层。
 */

export const VERT_SRC = `
attribute vec2 aPos;
varying vec2 vUv;
void main() {
  vUv = aPos * 0.5 + 0.5;
  gl_Position = vec4(aPos, 0.0, 1.0);
}
`;

export const FRAG_SRC = `
precision highp float;
varying vec2 vUv;

uniform sampler2D uImage;
uniform sampler2D uCurve;      // 256x1 RGBA LUT（色调曲线，复合曲线作用于三通道）
uniform vec2 uFlip;            // 图片 y 翻转

// 参数向量：0曝光 1对比 2高光 3阴影 4白 5黑 6色温 7色调 8自然饱和 9饱和 10清晰 11去雾
uniform float pA[12];
uniform float pB[12];
uniform float uSplit;          // 前后对比分割（-1 = 不对比，用 A）
uniform float uVignette;       // -100..0
uniform float uGrain;          // 0..100

// 蒙版（最多 6）：type 0 线性 1 径向 2 画笔（WP-B5：逐笔距离场纹理，3x2 图集）
uniform int uMaskCount;
uniform sampler2D uMaskField;  // 768x512 LUMINANCE，每格 256x256 = 一个画笔蒙版的因子场
uniform vec4 uMaskGeoA[6];
uniform vec4 uMaskGeoB[6];
uniform vec4 uMaskLocal0[6];   // exposure contrast highlights shadows
uniform vec4 uMaskLocal1[6];   // temp tint saturation clarity
uniform float uMaskAmount[6];
uniform float uMaskType[6];
uniform float uMaskOpacity[6];

// HSL 8 色带（WP-B4）。与引擎 imaging::applyHslApprox 同口径：
//   权重  色相到带中心 45° 半宽三角衰减（环形），多带可叠加
//   hue   h += Σ(w·hue)·0.15°
//   sat   s *= 1 + Σ(w·sat)/100
//   lum   l += Σ(w·lum)/200
// 带中心（度）：red 0 / orange 30 / yellow 60 / green 120 / aqua 180 / blue 240 / purple 280 / magenta 320。
// GLSL ES 1.0 红线：不用数组形参、不用变量索引 → 8 个带打成两个 mat4，
// 每列 = 一个带 (hue, sat, lum, 0)，列索引一律字面量常量。
uniform mat4 uHslA0;   // 列 0..3 = red orange yellow green
uniform mat4 uHslA1;   // 列 0..3 = aqua blue purple magenta
uniform mat4 uHslB0;
uniform mat4 uHslB1;

const vec3 LUM = vec3(0.2126, 0.7152, 0.0722);

vec3 srgb2lin(vec3 c) { return pow(max(c, vec3(0.0)), vec3(2.2)); }
vec3 lin2srgb(vec3 c) { return pow(max(c, vec3(0.0)), vec3(1.0 / 2.2)); }

vec3 rgb2hsl(vec3 c) {
  float mx = max(c.r, max(c.g, c.b));
  float mn = min(c.r, min(c.g, c.b));
  float l = (mx + mn) * 0.5;
  float h = 0.0;
  float s = 0.0;
  float d = mx - mn;
  if (d > 1e-5) {
    s = l > 0.5 ? d / (2.0 - mx - mn) : d / (mx + mn);
    if (mx == c.r) h = (c.g - c.b) / d + (c.g < c.b ? 6.0 : 0.0);
    else if (mx == c.g) h = (c.b - c.r) / d + 2.0;
    else h = (c.r - c.g) / d + 4.0;
    h /= 6.0;
  }
  return vec3(h, s, l);
}

float hue2rgb(float p, float q, float t) {
  if (t < 0.0) t += 1.0;
  if (t > 1.0) t -= 1.0;
  if (t < 1.0 / 6.0) return p + (q - p) * 6.0 * t;
  if (t < 0.5) return q;
  if (t < 2.0 / 3.0) return p + (q - p) * (2.0 / 3.0 - t) * 6.0;
  return p;
}

vec3 hsl2rgb(vec3 hsl) {
  if (hsl.y <= 1e-5) return vec3(hsl.z);
  float q = hsl.z < 0.5 ? hsl.z * (1.0 + hsl.y) : hsl.z + hsl.y - hsl.z * hsl.y;
  float p = 2.0 * hsl.z - q;
  return vec3(hue2rgb(p, q, hsl.x + 1.0 / 3.0), hue2rgb(p, q, hsl.x), hue2rgb(p, q, hsl.x - 1.0 / 3.0));
}

float smoothstep5(float edge0, float edge1, float x) {
  float t = clamp((x - edge0) / (edge1 - edge0), 0.0, 1.0);
  return t * t * (3.0 - 2.0 * t);
}

// 核心：12 个标量参数（GLSL ES 1.0 不支持数组形参）
vec3 applyParamsCore(vec3 base,
                     float exposure, float contrast, float highlights, float shadows,
                     float whites, float blacks, float temperature, float tint,
                     float vibrance, float saturation, float clarity, float dehaze) {
  // 1) 曝光（线性域）
  vec3 c = base * pow(2.0, exposure);
  c = lin2srgb(c);

  float lum = dot(c, LUM);

  // 2) 高光/阴影/白/黑（亮度域平滑蒙版）
  float hw = smoothstep5(0.45, 1.0, lum);
  c += (highlights / 100.0) * 0.55 * hw * (1.0 - c);
  float sw = 1.0 - smoothstep5(0.0, 0.55, lum);
  c += (shadows / 100.0) * 0.5 * sw * (1.0 - c);
  float ww = smoothstep5(0.6, 1.0, lum);
  c += (whites / 100.0) * 0.5 * ww * (1.0 - c);
  float bw = 1.0 - smoothstep5(0.0, 0.4, lum);
  c += (blacks / 100.0) * 0.45 * bw * c;

  // 3) 对比（pivot 0.5）
  c = (c - 0.5) * (1.0 + contrast / 100.0) + 0.5;

  // 4) 色温 / 色调
  c.r += (temperature / 100.0) * 0.12;
  c.b -= (temperature / 100.0) * 0.12;
  c.g += (tint / 100.0) * 0.08;
  c.r -= (tint / 100.0) * 0.04;
  c.b -= (tint / 100.0) * 0.04;

  // 5) 去雾（黑场拉伸 + 中间调加对比）与清晰度（中间调微对比近似）
  float dz = dehaze / 100.0;
  c = (c - dz * 0.06) * (1.0 + dz * 0.22) + dz * 0.02;
  float mid = 1.0 - abs(lum - 0.5) * 2.0;
  c = (c - 0.5) * (1.0 + (clarity / 100.0) * 0.35 * mid) + 0.5;

  // 6) 色调曲线 LUT
  c.r = texture2D(uCurve, vec2(clamp(c.r, 0.0, 1.0), 0.5)).r;
  c.g = texture2D(uCurve, vec2(clamp(c.g, 0.0, 1.0), 0.5)).g;
  c.b = texture2D(uCurve, vec2(clamp(c.b, 0.0, 1.0), 0.5)).b;

  // 7) 自然饱和 / 饱和（HSL 近似；vibrance 对低饱和加权）
  vec3 hsl = rgb2hsl(clamp(c, 0.0, 1.0));
  float vib = vibrance / 100.0;
  hsl.y += vib * (1.0 - hsl.y) * 0.5;
  hsl.y *= 1.0 + saturation / 100.0;
  c = hsl2rgb(clamp(hsl, 0.0, 1.0));

  return clamp(c, 0.0, 1.0);
}

vec3 applyParamsA(vec3 base) {
  return applyParamsCore(base, pA[0], pA[1], pA[2], pA[3], pA[4], pA[5],
                         pA[6], pA[7], pA[8], pA[9], pA[10], pA[11]);
}

vec3 applyParamsB(vec3 base) {
  return applyParamsCore(base, pB[0], pB[1], pB[2], pB[3], pB[4], pB[5],
                         pB[6], pB[7], pB[8], pB[9], pB[10], pB[11]);
}

// ---- HSL 8 色带（WP-B4） ----
float hueDist(float a, float b) {
  float d = abs(a - b);
  return min(d, 360.0 - d);
}

float bandW(float h, float center) {
  float d = hueDist(h, center);
  return d >= 45.0 ? 0.0 : 1.0 - d / 45.0;
}

vec3 applyHslCore(vec3 c, mat4 b0, mat4 b1) {
  vec3 hsl = rgb2hsl(clamp(c, 0.0, 1.0));
  if (hsl.y <= 1e-4) return c;   // 引擎在 s==0 时直接跳过（灰阶不受色带影响）
  float h = hsl.x * 360.0;
  float w0 = bandW(h, 0.0);
  float w1 = bandW(h, 30.0);
  float w2 = bandW(h, 60.0);
  float w3 = bandW(h, 120.0);
  float w4 = bandW(h, 180.0);
  float w5 = bandW(h, 240.0);
  float w6 = bandW(h, 280.0);
  float w7 = bandW(h, 320.0);
  float wSum = w0 + w1 + w2 + w3 + w4 + w5 + w6 + w7;
  if (wSum <= 1e-5) return c;
  float hueSum = w0 * b0[0].x + w1 * b0[1].x + w2 * b0[2].x + w3 * b0[3].x
               + w4 * b1[0].x + w5 * b1[1].x + w6 * b1[2].x + w7 * b1[3].x;
  float satSum = w0 * b0[0].y + w1 * b0[1].y + w2 * b0[2].y + w3 * b0[3].y
               + w4 * b1[0].y + w5 * b1[1].y + w6 * b1[2].y + w7 * b1[3].y;
  float lumSum = w0 * b0[0].z + w1 * b0[1].z + w2 * b0[2].z + w3 * b0[3].z
               + w4 * b1[0].z + w5 * b1[1].z + w6 * b1[2].z + w7 * b1[3].z;
  h += hueSum * 0.15;
  if (h < 0.0) h += 360.0;
  if (h >= 360.0) h -= 360.0;
  float s = clamp(hsl.y * (1.0 + satSum / 100.0), 0.0, 1.0);
  float l = clamp(hsl.z + lumSum / 200.0, 0.0, 1.0);
  return hsl2rgb(vec3(h / 360.0, s, l));
}

vec3 applyHslA(vec3 c) {
  return applyHslCore(c, uHslA0, uHslA1);
}

vec3 applyHslB(vec3 c) {
  return applyHslCore(c, uHslB0, uHslB1);
}

// 蒙版局部调整（sRGB 域近似）；active 为槽位启用因子（0/1）。
// 关键：GLSL ES 1.0 中「函数形参」不是 constant-index-expression，不能用 i 索引 uniform 数组
// （ERROR: 0:134 '[]' : Index expression must be constant）。故核心函数一律收标量/vec4，
// 由 applyMaskN 六个包装各自用「字面量常量索引」读取 uniform。
vec3 applyMaskCore(vec3 c, vec2 uv, float active,
                   float type, vec4 ga, vec4 gb, vec4 l0, vec4 l1,
                   float amountRaw, float opacityRaw, vec2 cell) {
  if (active < 0.5) return c;
  float factor = 0.0;
  if (type < 0.5) {
    // 线性渐变：沿 (x1,y1)→(x2,y2) 投影 0..1
    vec2 d = gb.xy - ga.xy;
    float len2 = max(dot(d, d), 1e-5);
    float t = dot(uv - ga.xy, d) / len2;
    factor = clamp(t, 0.0, 1.0);
  } else if (type < 1.5) {
    // 径向：中心外衰减
    vec2 d = uv - ga.xy;
    d.y /= max(gb.w, 0.1);
    factor = 1.0 - smoothstep5(gb.z * 0.55, gb.z, length(d));
  } else {
    // 画笔（WP-B5）：逐笔距离场。场纹理行 0 = 画面顶部，上传后落在 v=0，
    // 故 v = (row + (1 - uv.y)) / 2；cell 由 applyMaskN 以字面量常量传入。
    vec2 fuv = vec2((cell.x + uv.x) / 3.0, (cell.y + 1.0 - uv.y) / 2.0);
    factor = texture2D(uMaskField, fuv).r;
  }
  factor *= opacityRaw / 100.0;
  if (factor <= 0.001) return c;

  float amount = amountRaw / 100.0;
  float lum = dot(c, LUM);

  // 主强度（渐变/径向整体曝光）
  c *= pow(2.0, (l0.x + amount * 0.5) * factor * 0.8);
  float hw = smoothstep5(0.45, 1.0, lum);
  float sw = 1.0 - smoothstep5(0.0, 0.55, lum);
  c += (l0.z / 100.0) * 0.4 * hw * factor * (1.0 - c);
  c += (l0.w / 100.0) * 0.4 * sw * factor * (1.0 - c);
  c = (c - 0.5) * (1.0 + (l0.y / 100.0) * 0.5 * factor) + 0.5;
  c.r += (l1.x / 100.0) * 0.1 * factor;
  c.b -= (l1.x / 100.0) * 0.1 * factor;
  vec3 hsl = rgb2hsl(clamp(c, 0.0, 1.0));
  hsl.y *= 1.0 + (l1.z / 100.0) * 0.6 * factor;
  c = hsl2rgb(clamp(hsl, 0.0, 1.0));
  float mid = 1.0 - abs(lum - 0.5) * 2.0;
  c = (c - 0.5) * (1.0 + (l1.w / 100.0) * 0.3 * mid * factor) + 0.5;
  return clamp(c, 0.0, 1.0);
}

vec3 applyMask0(vec3 c, vec2 uv, float a) {
  return applyMaskCore(c, uv, a, uMaskType[0], uMaskGeoA[0], uMaskGeoB[0],
                       uMaskLocal0[0], uMaskLocal1[0], uMaskAmount[0], uMaskOpacity[0], vec2(0.0, 0.0));
}
vec3 applyMask1(vec3 c, vec2 uv, float a) {
  return applyMaskCore(c, uv, a, uMaskType[1], uMaskGeoA[1], uMaskGeoB[1],
                       uMaskLocal0[1], uMaskLocal1[1], uMaskAmount[1], uMaskOpacity[1], vec2(1.0, 0.0));
}
vec3 applyMask2(vec3 c, vec2 uv, float a) {
  return applyMaskCore(c, uv, a, uMaskType[2], uMaskGeoA[2], uMaskGeoB[2],
                       uMaskLocal0[2], uMaskLocal1[2], uMaskAmount[2], uMaskOpacity[2], vec2(2.0, 0.0));
}
vec3 applyMask3(vec3 c, vec2 uv, float a) {
  return applyMaskCore(c, uv, a, uMaskType[3], uMaskGeoA[3], uMaskGeoB[3],
                       uMaskLocal0[3], uMaskLocal1[3], uMaskAmount[3], uMaskOpacity[3], vec2(0.0, 1.0));
}
vec3 applyMask4(vec3 c, vec2 uv, float a) {
  return applyMaskCore(c, uv, a, uMaskType[4], uMaskGeoA[4], uMaskGeoB[4],
                       uMaskLocal0[4], uMaskLocal1[4], uMaskAmount[4], uMaskOpacity[4], vec2(1.0, 1.0));
}
vec3 applyMask5(vec3 c, vec2 uv, float a) {
  return applyMaskCore(c, uv, a, uMaskType[5], uMaskGeoA[5], uMaskGeoB[5],
                       uMaskLocal0[5], uMaskLocal1[5], uMaskAmount[5], uMaskOpacity[5], vec2(2.0, 1.0));
}

float hash(vec2 p) {
  return fract(sin(dot(p, vec2(127.1, 311.7))) * 43758.5453);
}

void main() {
  vec2 uv = vec2(vUv.x, mix(1.0 - vUv.y, vUv.y, uFlip.y));
  vec3 base = texture2D(uImage, uv).rgb;

  vec3 cA = applyParamsA(srgb2lin(base));
  // 8 色带（引擎口径：色调 → HSL 色带 → 蒙版局部）
  cA = applyHslA(cA);
  // 六个槽位常量展开（无循环、无 break、无变量索引）
  float mc = float(uMaskCount);
  cA = applyMask0(cA, vUv, step(0.5, mc));
  cA = applyMask1(cA, vUv, step(1.5, mc));
  cA = applyMask2(cA, vUv, step(2.5, mc));
  cA = applyMask3(cA, vUv, step(3.5, mc));
  cA = applyMask4(cA, vUv, step(4.5, mc));
  cA = applyMask5(cA, vUv, step(5.5, mc));

  vec3 c;
  if (uSplit < 0.0) {
    c = cA;
  } else {
    vec3 cB = applyHslB(applyParamsB(srgb2lin(base)));
    c = vUv.x < uSplit ? cB : cA;
  }

  // 暗角
  if (uVignette < -0.5) {
    float d = length((vUv - 0.5) * vec2(1.25, 1.0));
    float v = smoothstep5(0.35, 0.85, d);
    c *= 1.0 + (uVignette / 100.0) * v * 0.9;
  }

  // 颗粒
  if (uGrain > 0.5) {
    float g = (hash(floor(gl_FragCoord.xy * 0.75)) - 0.5) * (uGrain / 100.0) * 0.22;
    c += g;
  }

  gl_FragColor = vec4(clamp(c, 0.0, 1.0), 1.0);
}
`;
