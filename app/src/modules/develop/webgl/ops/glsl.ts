/**
 * T1.4 · 93 叶 GPU 管线（GLSL ES 1.0 多 pass，与 ops/oracle93.ts 逐式对应）。
 *
 * 域纪律（与引擎/T1.3 一致）：
 *  - 中间 pass 全部在 log16 码 t 域（float FBO，t = code/65535）；
 *  - S3 toneMap 是唯一域分界；S6 之后再做 sRGB 输出编码；
 *  - 蒙版 = 独立 mask pass（T2.7，tone 后 sharpen 前；t 域与 engine codeAt/65535 同口径）。
 *
 * GLSL ES 1.0 红线（见 shaders.ts 头注）：uniform 数组只允许常量索引；
 * 循环用常量边界 + step 掩码（无 break/continue）；浮点纹理手动双线性
 * （不依赖 OES_texture_float_linear）。
 */

export const PASS_VERT = `
attribute vec2 aPos;
varying vec2 vUv;
void main() {
  vUv = aPos * 0.5 + 0.5;
  gl_Position = vec4(aPos, 0.0, 1.0);
}
`;

/** 共享头：t 域取址（NEAREST + 手动 clamp）与公共数学 */
const COMMON = `
precision highp float;
varying vec2 vUv;
uniform sampler2D uTex;
uniform vec2 uTexSize;   // 输入纹理像素尺寸

vec3 fetchPx(vec2 px) {
  vec2 lo = vec2(0.5) / uTexSize;
  vec2 uv = (floor(px) + 0.5) / uTexSize;
  return texture2D(uTex, clamp(uv, lo, vec2(1.0) - lo)).rgb;
}

float smooth01(float x, float edge0, float edge1) {
  if (edge1 <= edge0) return x >= edge1 ? 1.0 : 0.0;
  float t = clamp((x - edge0) / (edge1 - edge0), 0.0, 1.0);
  return t * t * (3.0 - 2.0 * t);
}

// 与 CPU log16Decode(t*65535)=2^(code/2048-12) 同口径：65535/2048=32.0009765625
// （勿写成 t*32——那隐含 65536 网格，会引入 ~0.05 码级系统偏差）
float log16Decode(float t) { return exp2(t * (65535.0 / 2048.0) - 12.0); }
// 与 CPU log16Encode 同口径：码值四舍五入到整数码栅格（±0.5 码，避免 S4 曲线
// 索引在 1/255 边界翻转造成 0.125EV 级跳变）
float log16EncodeF(float lin) {
  float x = max(lin, exp2(-12.0));
  float code = clamp((log2(x) + 12.0) * 2048.0, 0.0, 65535.0);
  return floor(code + 0.5) / 65535.0;
}

vec3 rgb2hsl(vec3 c) {
  float mx = max(c.r, max(c.g, c.b));
  float mn = min(c.r, min(c.g, c.b));
  float d = mx - mn;
  float l = (mx + mn) * 0.5;
  if (d <= 1e-6) return vec3(0.0, 0.0, l);
  float s = l > 0.5 ? d / (2.0 - mx - mn) : d / (mx + mn);
  float h;
  if (mx == c.r) h = (c.g - c.b) / d + (c.g < c.b ? 6.0 : 0.0);
  else if (mx == c.g) h = (c.b - c.r) / d + 2.0;
  else h = (c.r - c.g) / d + 4.0;
  return vec3(h / 6.0, s, l);
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
  if (hsl.y <= 1e-12) return vec3(hsl.z);
  float q = hsl.z < 0.5 ? hsl.z * (1.0 + hsl.y) : hsl.z + hsl.y - hsl.z * hsl.y;
  float p = 2.0 * hsl.z - q;
  return vec3(hue2rgb(p, q, hsl.x + 1.0 / 3.0), hue2rgb(p, q, hsl.x), hue2rgb(p, q, hsl.x - 1.0 / 3.0));
}

float linearToSrgb(float lin) {
  if (lin <= 0.0) return 0.0;
  if (lin >= 1.0) return 1.0;
  if (lin <= 0.0031308) return lin * 12.92;
  return 1.055 * pow(lin, 1.0 / 2.4) - 0.055;
}
`;

/** P0：S0 白平衡（temperature/tint，码域 EV 偏移两趟 clamp；engine basic.cpp WbGainOp 镜像）。
 *  uWb = (tTemp, tTint) = (temperature*0.002, tint*0.001)，仅 whiteBalance==='custom' 时执行。 */
export const FRAG_WB = `
${COMMON}
uniform vec2 uWb;

void main() {
  vec3 t = texture2D(uTex, vUv).rgb;
  if (uWb.x != 0.0) {
    float d = uWb.x * 2048.0 / 65535.0;
    t.r = clamp(t.r + d, 0.0, 1.0);
    t.b = clamp(t.b - d, 0.0, 1.0);
  }
  if (uWb.y != 0.0) {
    float d = uWb.y * 2048.0 / 65535.0;
    t.r = clamp(t.r + d * 0.5, 0.0, 1.0);
    t.g = clamp(t.g - d, 0.0, 1.0);
    t.b = clamp(t.b + d * 0.5, 0.0, 1.0);
  }
  gl_FragColor = vec4(t, 1.0);
}
`;

/** P1a：S1 几何（T2.10 合并采样：crop/perspective/aspect 单应 + 手动畸变复合，
 *  一次逆映射采样；engine geometry_chain / oracle93 同式 src(D(H_inv(dx)))） */
export const FRAG_GEO = `
${COMMON}
uniform mat3 uInv;        // 逆单应（dst 像素 → 源全图像素，已含 aspect x 向适配）
uniform vec2 uOutSize;
uniform vec2 uCropOff;    // cropX, cropY
uniform float uGeomActive;
uniform float uDistK;     // 畸变系数 = amt/100*0.3（0=无；畸变先于裁剪的合成形式）

void main() {
  if (uGeomActive < 0.5) { gl_FragColor = vec4(texture2D(uTex, vUv).rgb, 1.0); return; }
  vec2 dstPx = floor(vUv * uOutSize);
  vec3 s = uInv * vec3(dstPx, 1.0);
  vec2 full = s.xy / s.z + uCropOff;
  // 畸变逆映射复合（r 基于源全图中心；与 engine/oracle 逐式一致）
  if (uDistK != 0.0) {
    vec2 c = (uTexSize - 1.0) * 0.5;
    vec2 d = full - c;
    float r = length(d) / length(c);
    float rf = r * (1.0 + uDistK * r * r);
    full = c + d * (rf / max(1e-9, r));
  }
  // 出界判定带 1e-4 像素容差（与 CPU oracle 同步）：fp32/fp64 在边界 ±1e-5 内的
  // 取向差异会产生整像素黑/图翻转，收紧为双侧一致的包含判定（边缘钳位双线性）。
  if (full.x < -1e-4 || full.y < -1e-4 || full.x > uTexSize.x - 1.0 + 1e-4 || full.y > uTexSize.y - 1.0 + 1e-4) {
    gl_FragColor = vec4(0.0, 0.0, 0.0, 1.0);
    return;
  }
  vec2 f = floor(full);
  vec2 fr = full - f;
  vec3 p00 = fetchPx(f) * 65535.0;
  vec3 p10 = fetchPx(f + vec2(1.0, 0.0)) * 65535.0;
  vec3 p01 = fetchPx(f + vec2(0.0, 1.0)) * 65535.0;
  vec3 p11 = fetchPx(f + vec2(1.0, 1.0)) * 65535.0;
  vec3 top = p00 + (p10 - p00) * fr.x;
  vec3 bot = p01 + (p11 - p01) * fr.x;
  vec3 v = (top + (bot - top) * fr.y) / 65535.0;
  gl_FragColor = vec4(clamp(v, 0.0, 1.0), 1.0);
}
`;

/** 通用可分离盒式模糊 H/V（引擎 boxBlurPlane 镜像：边界按实际窗口数除）。
 *  uMode: 0 = 原码值平面；1 = 色度平面（code - lum）。uRadius ≤ 16。 */
export const FRAG_BLUR_H = `
${COMMON}
uniform float uRadius;
uniform float uMode;

void main() {
  vec2 px = floor(vUv * uTexSize);
  vec3 sum = vec3(0.0);
  for (int i = -16; i <= 16; i++) {
    float o = float(i);
    float act = step(abs(o), uRadius)
              * step(0.0, px.x + o) * step(px.x + o, uTexSize.x - 1.0);
    vec3 s = fetchPx(px + vec2(o, 0.0)) * 65535.0;
    if (uMode > 0.5) {
      float sl = (s.r + s.g + s.b) / 3.0;
      s -= vec3(sl);
    }
    sum += s * act;
  }
  float count = min(uTexSize.x, px.x + uRadius + 1.0) - max(0.0, px.x - uRadius);
  gl_FragColor = vec4(sum / max(count, 1.0) / 65535.0, 1.0);
}
`;

export const FRAG_BLUR_V = `
${COMMON}
uniform float uRadius;
uniform float uMode;

void main() {
  vec2 px = floor(vUv * uTexSize);
  vec3 sum = vec3(0.0);
  for (int i = -16; i <= 16; i++) {
    float o = float(i);
    float act = step(abs(o), uRadius)
              * step(0.0, px.y + o) * step(px.y + o, uTexSize.y - 1.0);
    vec3 s = fetchPx(px + vec2(0.0, o)) * 65535.0;
    if (uMode > 0.5) {
      float sl = (s.r + s.g + s.b) / 3.0;
      s -= vec3(sl);
    }
    sum += s * act;
  }
  float count = min(uTexSize.y, px.y + uRadius + 1.0) - max(0.0, px.y - uRadius);
  gl_FragColor = vec4(sum / max(count, 1.0) / 65535.0, 1.0);
}
`;

/** NR 应用：色度替换 + 亮度平滑（引擎 detail.cpp DenoiseOp 顺序镜像） */
export const FRAG_DENOISE_APPLY = `
${COMMON}
uniform sampler2D uBlurC;   // 模糊后的色度平面
uniform sampler2D uBlurL;   // 模糊后的码值平面
uniform float uHasChroma;   // cnr > 0
uniform float uChromaKeep;  // cnrDetail/100*0.5
uniform float uHasLuma;     // luma > 0
uniform float uLumaAmt;     // luma/100

void main() {
  vec3 c = texture2D(uTex, vUv).rgb * 65535.0;
  float lum = (c.r + c.g + c.b) / 3.0;
  vec3 c2 = c;
  if (uHasChroma > 0.5) {
    vec3 chroma = c - vec3(lum);
    vec3 blurred = texture2D(uBlurC, vUv).rgb * 65535.0;
    c2 = c + (blurred - chroma) * (1.0 - uChromaKeep);
  }
  if (uHasLuma > 0.5) {
    vec3 blurredL = texture2D(uBlurL, vUv).rgb * 65535.0;
    c2 = c2 + (blurredL - c2) * uLumaAmt;
  }
  gl_FragColor = vec4(clamp(c2 / 65535.0, 0.0, 1.0), 1.0);
}
`;

/** 局部对比应用：out = base + k*(base - blur)（clarity r10 g0.6 / texture r2 g0.8） */
export const FRAG_LOCAL_CONTRAST = `
${COMMON}
uniform sampler2D uBlur;
uniform float uGain;   // amt/100 * gainScale

void main() {
  vec3 c = texture2D(uTex, vUv).rgb * 65535.0;
  vec3 b = texture2D(uBlur, vUv).rgb * 65535.0;
  gl_FragColor = vec4(clamp((c + uGain * (c - b)) / 65535.0, 0.0, 1.0), 1.0);
}
`;

/** P3：S2 逐像素色调 + S3 toneMap + S4 曲线/HSL/分级（oracle applyToneColor 镜像） */
export const FRAG_TONE = `
${COMMON}
uniform sampler2D uCurve;    // 1024x1 RGBA NEAREST：r=RGB复合 g=R b=G a=B
uniform vec4 uTone;          // exposure contrast highlights shadows
uniform vec4 uTone2;         // whites blacks dehaze 0
uniform vec4 uSatV;          // vibrance saturation 0 0
uniform vec4 uCalib0;        // shadowTint redHue redSat greenHue
uniform vec4 uCalib1;        // greenSat blueHue blueSat 0
uniform vec4 uParam;         // parametric hi li da sh
uniform vec4 uSplits;        // shadowSplit lightSplit midSplit 0（255 域）
uniform mat4 uHsl0;          // 列=band(red orange yellow green)：(hue,sat,lum,0)
uniform mat4 uHsl1;          // aqua blue purple magenta
uniform vec4 uGrade0;        // shadowHue shadowSat midtoneHue midtoneSat
uniform vec4 uGrade1;        // highlightHue highlightSat blend balance
uniform vec4 uCurveSkip;     // 1=该曲线为恒等（引擎跳过）：x=RGB复合 y=R z=G w=B

float bandW(float h, float center) {
  float d = abs(h - center);
  if (d > 0.5) d = 1.0 - d;
  return max(0.0, 1.0 - d * 12.0);
}

// 曲线 LUT 查找（1024x1，分段线性插值；与 CPU applyOne/engine 逐式同口径：
// pos = t*(N-1) 后相邻 texel 手动 lerp——NEAREST 纹理，不依赖 float-linear 扩展）
vec4 curveSampleLut(float t) {
  float pos = clamp(t, 0.0, 1.0) * 1023.0;
  float k = floor(pos);
  float f = pos - k;
  float u0 = (k + 0.5) / 1024.0;
  float u1 = (min(k + 1.0, 1023.0) + 0.5) / 1024.0;
  return mix(texture2D(uCurve, vec2(u0, 0.5)), texture2D(uCurve, vec2(u1, 0.5)), f);
}

// S3 toneMap（Reinhard，唯一域分界）——数值稳定型：
//   log2(lin/(1+lin)) = A - log2(1+2^A)，A≥0 时 = -log2(1+2^-A)。
//   所有 exp2/log2 的结果落在 [0.5,2)，fp32 ULP 误差 ~6e-8 相对 →
//   整数码误差 ~2e-4，远低于曲线 idx 边界间距（0.5 码），消除 0.125EV 量化悬崖。
//   返回整数码 0..65535（与 CPU log16Encode 的 Math.round 同口径）。
float toneMapCode(float t) {
  float A = t * (65535.0 / 2048.0) - 12.0;
  float l2rat;
  if (A >= 0.0) {
    l2rat = -log2(1.0 + exp2(-A));
  } else {
    l2rat = A - log2(1.0 + exp2(A));
  }
  float code = (l2rat + 12.0) * 2048.0;
  return floor(clamp(code, 0.0, 65535.0) + 0.5);
}

// 引擎 uint16 存储口径：t 量化到整数码栅格（CPU oracle 在 HSL/分级后同式取整）
float q65535(float t) {
  return floor(clamp(t, 0.0, 1.0) * 65535.0 + 0.5) / 65535.0;
}
vec3 q65535(vec3 v) {
  return vec3(q65535(v.r), q65535(v.g), q65535(v.b));
}

void main() {
  vec3 t = texture2D(uTex, vUv).rgb;
  // 曝光（码域整数口径在 float 下同式）
  if (uTone.x != 0.0) {
    float d = uTone.x * 2048.0 / 65535.0;
    t = clamp(t + vec3(d), 0.0, 1.0);
  }
  // 对比
  if (uTone.y != 0.0) {
    float k = 1.0 + uTone.y / 100.0 * 0.3;
    float mid = 65535.0 * 0.45;
    t = clamp((mid + (t * 65535.0 - mid) * k) / 65535.0, 0.0, 1.0);
  }
  // 高光/阴影/白/黑（三分量亮度分区）
  // T1.5b：log16 t 域锚点（engine basic.cpp:26-29 定稿镜像，3945259）
  // 显示白 t=0.3750；highlights 0.300 起坡(sRGB≈120)、whites 0.3456 起坡(75%)、
  // shadows 0.306 归零(50%)、blacks 0.1689 归零(10%)
  float l = (t.r + t.g + t.b) / 3.0;
  if (uTone.z != 0.0) t = clamp(t + vec3(uTone.z / 100.0 * 3000.0 * smooth01(l, 0.3000, 0.3750) / 65535.0), 0.0, 1.0);
  l = (t.r + t.g + t.b) / 3.0;
  if (uTone.w != 0.0) t = clamp(t + vec3(uTone.w / 100.0 * 3000.0 * (1.0 - smooth01(l, 0.2410, 0.3060)) / 65535.0), 0.0, 1.0);
  l = (t.r + t.g + t.b) / 3.0;
  if (uTone2.x != 0.0) t = clamp(t + vec3(uTone2.x / 100.0 * 3000.0 * smooth01(l, 0.3456, 0.3750) / 65535.0), 0.0, 1.0);
  l = (t.r + t.g + t.b) / 3.0;
  if (uTone2.y != 0.0) t = clamp(t + vec3(uTone2.y / 100.0 * 3000.0 * (1.0 - smooth01(l, 0.0, 0.1689)) / 65535.0), 0.0, 1.0);
  // 去雾
  if (uTone2.z != 0.0) {
    float dz = uTone2.z / 100.0;
    l = (t.r + t.g + t.b) / 3.0;
    float mean = l * 65535.0;
    vec3 v = t * 65535.0;
    v += vec3(dz * 1500.0 * (1.0 - smooth01(l, 0.2410, 0.3060)) - dz * 500.0 * (1.0 - smooth01(l, 0.0, 0.1689)));
    v = vec3(mean) + (v - vec3(mean)) * (1.0 + dz * 0.25);
    t = clamp(v / 65535.0, 0.0, 1.0);
  }
  // 校准 shadowTint
  if (uCalib0.x != 0.0) {
    l = (t.r + t.g + t.b) / 3.0;
    float w = 1.0 - smooth01(l, 0.0, 0.4);
    float dR = uCalib0.x / 100.0 * 400.0 * 0.5 * w / 65535.0;
    float dG = -uCalib0.x / 100.0 * 400.0 * w / 65535.0;
    t = clamp(t + vec3(dR, dG, dR), 0.0, 1.0);
  }
  // 三原色（red/green/blue：交叉混合 + sat；三通道手动展开，规避动态索引）
  vec3 calibHue = vec3(uCalib0.y, uCalib0.w, uCalib1.y);
  vec3 calibSat = vec3(uCalib0.z, uCalib1.x, uCalib1.z);
  for (int ch = 0; ch < 3; ch++) {
    float hue = calibHue[ch];
    float sat = calibSat[ch];
    if (hue == 0.0 && sat == 0.0) continue;
    vec3 pxv = t * 65535.0;
    float mean = (pxv.r + pxv.g + pxv.b) / 3.0;
    if (ch == 0) {
      pxv.r += (pxv.g - pxv.b) * (hue / 100.0 * 0.25);
      pxv.r = mean + (pxv.r - mean) * (1.0 + sat / 100.0);
    } else if (ch == 1) {
      pxv.g += (pxv.b - pxv.r) * (hue / 100.0 * 0.25);
      pxv.g = mean + (pxv.g - mean) * (1.0 + sat / 100.0);
    } else {
      pxv.b += (pxv.r - pxv.g) * (hue / 100.0 * 0.25);
      pxv.b = mean + (pxv.b - mean) * (1.0 + sat / 100.0);
    }
    t = clamp(pxv / 65535.0, 0.0, 1.0);
  }
  // 饱和 / 自然饱和（vibrance 先于 saturation，两趟）
  if (uSatV.x != 0.0) {
    float mean = (t.r + t.g + t.b) / 3.0;
    float mx = max(t.r, max(t.g, t.b)) * 65535.0;
    float mn = min(t.r, min(t.g, t.b)) * 65535.0;
    float sat = (mx - mn) / (mx + mn + 1.0);
    float factor = 1.0 + uSatV.x / 100.0 * (1.0 - sat) * 0.6;
    t = clamp((vec3(mean) + (t - vec3(mean)) * factor), 0.0, 1.0);
  }
  if (uSatV.y != 0.0) {
    float mean = (t.r + t.g + t.b) / 3.0;
    float factor = 1.0 + uSatV.y / 100.0;
    t = clamp((vec3(mean) + (t - vec3(mean)) * factor), 0.0, 1.0);
  }
  // S3 toneMap（Reinhard，唯一域分界；数值稳定型，见 toneMapCode 注）
  t = vec3(q65535(toneMapCode(t.r) / 65535.0),
           q65535(toneMapCode(t.g) / 65535.0),
           q65535(toneMapCode(t.b) / 65535.0));
  // S4 点曲线：引擎注册序 RGB复合（三通道）→ R → G → B；恒等 LUT 跳过（engine
  // curve.cpp 的 if (identity) return true——恒等查表会引入栅格再量化，语义不同）
  if (uCurveSkip.x < 0.5) {
    t.r = clamp(curveSampleLut(t.r).r / 255.0, 0.0, 1.0);
    t.g = clamp(curveSampleLut(t.g).r / 255.0, 0.0, 1.0);
    t.b = clamp(curveSampleLut(t.b).r / 255.0, 0.0, 1.0);
  }
  if (uCurveSkip.y < 0.5) {
    t.r = clamp(curveSampleLut(t.r).g / 255.0, 0.0, 1.0);
  }
  if (uCurveSkip.z < 0.5) {
    t.g = clamp(curveSampleLut(t.g).b / 255.0, 0.0, 1.0);
  }
  if (uCurveSkip.w < 0.5) {
    t.b = clamp(curveSampleLut(t.b).a / 255.0, 0.0, 1.0);
  }
  // S4 参数曲线（三通道手动展开）
  if (uParam.x != 0.0 || uParam.y != 0.0 || uParam.z != 0.0 || uParam.w != 0.0) {
    float uR = t.r * 255.0;
    float t0R = smooth01(uR, 0.0, uSplits.x);
    float t1R = smooth01(uR, uSplits.x, uSplits.y);
    float t2R = smooth01(uR, uSplits.y, uSplits.z);
    float dR = (uParam.x * t2R + uParam.y * (t1R - t2R) + uParam.z * (t0R - t1R) + uParam.w * (1.0 - t0R)) / 100.0 * 60.0;
    t.r = clamp(uR + dR, 0.0, 255.0) / 255.0;
    float uG = t.g * 255.0;
    float t0G = smooth01(uG, 0.0, uSplits.x);
    float t1G = smooth01(uG, uSplits.x, uSplits.y);
    float t2G = smooth01(uG, uSplits.y, uSplits.z);
    float dG = (uParam.x * t2G + uParam.y * (t1G - t2G) + uParam.z * (t0G - t1G) + uParam.w * (1.0 - t0G)) / 100.0 * 60.0;
    t.g = clamp(uG + dG, 0.0, 255.0) / 255.0;
    float uB = t.b * 255.0;
    float t0B = smooth01(uB, 0.0, uSplits.x);
    float t1B = smooth01(uB, uSplits.x, uSplits.y);
    float t2B = smooth01(uB, uSplits.y, uSplits.z);
    float dB = (uParam.x * t2B + uParam.y * (t1B - t2B) + uParam.z * (t0B - t1B) + uParam.w * (1.0 - t0B)) / 100.0 * 60.0;
    t.b = clamp(uB + dB, 0.0, 255.0) / 255.0;
  }
  // S4 HSL 8 带（T1.3 口径：带宽 1/12，hue ±30°）
  vec3 hsl = rgb2hsl(t);
  if (hsl.y > 1e-6) {
    float w0 = bandW(hsl.x, 0.0);           // red
    float w1 = bandW(hsl.x, 1.0 / 12.0);    // orange
    float w2 = bandW(hsl.x, 1.0 / 6.0);     // yellow
    float w3 = bandW(hsl.x, 1.0 / 3.0);     // green
    float w4 = bandW(hsl.x, 0.5);           // aqua
    float w5 = bandW(hsl.x, 2.0 / 3.0);     // blue
    float w6 = bandW(hsl.x, 5.0 / 6.0);     // purple
    float w7 = bandW(hsl.x, 11.0 / 12.0);   // magenta
    float dHue = w0 * uHsl0[0].x / 100.0 * (30.0 / 360.0)
               + w1 * uHsl0[1].x / 100.0 * (30.0 / 360.0)
               + w2 * uHsl0[2].x / 100.0 * (30.0 / 360.0)
               + w3 * uHsl0[3].x / 100.0 * (30.0 / 360.0)
               + w4 * uHsl1[0].x / 100.0 * (30.0 / 360.0)
               + w5 * uHsl1[1].x / 100.0 * (30.0 / 360.0)
               + w6 * uHsl1[2].x / 100.0 * (30.0 / 360.0)
               + w7 * uHsl1[3].x / 100.0 * (30.0 / 360.0);
    float dSat = w0 * uHsl0[0].y / 100.0 + w1 * uHsl0[1].y / 100.0
               + w2 * uHsl0[2].y / 100.0 + w3 * uHsl0[3].y / 100.0
               + w4 * uHsl1[0].y / 100.0 + w5 * uHsl1[1].y / 100.0
               + w6 * uHsl1[2].y / 100.0 + w7 * uHsl1[3].y / 100.0;
    float dLum = w0 * uHsl0[0].z / 100.0 * 0.3 + w1 * uHsl0[1].z / 100.0 * 0.3
               + w2 * uHsl0[2].z / 100.0 * 0.3 + w3 * uHsl0[3].z / 100.0 * 0.3
               + w4 * uHsl1[0].z / 100.0 * 0.3 + w5 * uHsl1[1].z / 100.0 * 0.3
               + w6 * uHsl1[2].z / 100.0 * 0.3 + w7 * uHsl1[3].z / 100.0 * 0.3;
    float h = hsl.x + dHue;
    if (h < 0.0) h += 1.0;
    if (h >= 1.0) h -= 1.0;
    float s = clamp(hsl.y * (1.0 + dSat), 0.0, 1.0);
    float lv = clamp(hsl.z + dLum * 0.3, 0.0, 1.0);
    // 引擎 uint16 存储口径（CPU oracle 同式 Math.round(*65535)/65535）
    t = q65535(clamp(hsl2rgb(vec3(h, s, lv)), 0.0, 1.0));
  }
  // S4 颜色分级
  float gAny = step(abs(uGrade0.x) + abs(uGrade0.y) + abs(uGrade0.z) + abs(uGrade0.w) + abs(uGrade1.x) + abs(uGrade1.y), 0.0);
  if (gAny < 0.5) {
    vec3 hslg = rgb2hsl(t);
    float balance = uGrade1.w / 100.0 * 0.15;
    float spread = 0.25 + uGrade1.z / 100.0 * 0.25;
    float wHi = smooth01(hslg.z, 0.5 - balance - spread * 0.5, 0.5 - balance + spread * 0.5);
    float wSh = 1.0 - smooth01(hslg.z, 0.35 - balance - spread * 0.5, 0.35 - balance + spread * 0.5);
    float wMid = max(0.0, 1.0 - wHi - wSh);
    float dHue = 0.0;
    float dSat = 0.0;
    // shadow / midtone / highlight 三区
    if (wSh > 0.0 && uGrade0.y != 0.0) {
      float d = uGrade0.x / 360.0 - hslg.x;
      if (d > 0.5) d -= 1.0;
      if (d < -0.5) d += 1.0;
      float strength = min(1.0, abs(uGrade0.y) / 100.0) * wSh;
      dHue += d * strength * 0.5;
      dSat += uGrade0.y / 100.0 * wSh * 0.3;
    }
    if (wMid > 0.0 && uGrade0.w != 0.0) {
      float d = uGrade0.z / 360.0 - hslg.x;
      if (d > 0.5) d -= 1.0;
      if (d < -0.5) d += 1.0;
      float strength = min(1.0, abs(uGrade0.w) / 100.0) * wMid;
      dHue += d * strength * 0.5;
      dSat += uGrade0.w / 100.0 * wMid * 0.3;
    }
    if (wHi > 0.0 && uGrade1.y != 0.0) {
      float d = uGrade1.x / 360.0 - hslg.x;
      if (d > 0.5) d -= 1.0;
      if (d < -0.5) d += 1.0;
      float strength = min(1.0, abs(uGrade1.y) / 100.0) * wHi;
      dHue += d * strength * 0.5;
      dSat += uGrade1.y / 100.0 * wHi * 0.3;
    }
    float h = hslg.x + dHue;
    if (h < 0.0) h += 1.0;
    if (h >= 1.0) h -= 1.0;
    float s = clamp(hslg.y * (1.0 + dSat), 0.0, 1.0);
    t = q65535(clamp(hsl2rgb(vec3(h, s, hslg.z)), 0.0, 1.0));
  }
  gl_FragColor = vec4(t, 1.0);
}
`;

/** P4 锐化应用（blur 由通用 H/V 对给出；unsharp + masking 阈值） */
export const FRAG_SHARPEN = `
${COMMON}
uniform sampler2D uBlur;
uniform vec4 uSharp;   // sharpness radius detail masking

void main() {
  vec3 c = texture2D(uTex, vUv).rgb * 65535.0;
  vec3 b = texture2D(uBlur, vUv).rgb * 65535.0;
  float k = uSharp.x / 100.0 * 0.9;
  float dBoost = 1.0 + uSharp.z / 100.0 * 0.5;
  float threshold = uSharp.w / 100.0 * 30.0;
  vec3 resid = (c - b) * dBoost;
  vec3 delta = vec3(step(threshold, abs(resid.r)) * resid.r,
                    step(threshold, abs(resid.g)) * resid.g,
                    step(threshold, abs(resid.b)) * resid.b) * k;
  gl_FragColor = vec4(clamp((c + delta) / 65535.0, 0.0, 1.0), 1.0);
}
`;

/** P5：S6 暗角 + 颗粒 + sRGB 编码（确定性 TPDF 噪声纹理） */
export const FRAG_EFFECTS = `
${COMMON}
uniform vec4 uEff;          // vignetteAmount vignetteMidpoint(/100) feather grainAmount
uniform float uGrainAmp;    // amp/65535
uniform sampler2D uNoise;   // R=块噪声 G/B=TPDF u1/u2
uniform vec2 uNoiseSize;

void main() {
  vec2 px = floor(vUv * uTexSize);
  vec3 t = texture2D(uTex, vUv).rgb;
  // 噪声纹理坐标（与 ensureNoise 的 w×h 逐像素布局一致；NEAREST 采样取整 texel）
  vec2 puv = (px + 0.5) / uNoiseSize;
  vec2 nuv = clamp(puv, vec2(0.5 / uNoiseSize.x, 0.5 / uNoiseSize.y), vec2(1.0) - vec2(0.5 / uNoiseSize.x, 0.5 / uNoiseSize.y));
  // 暗角
  if (uEff.x != 0.0) {
    vec2 c = (uTexSize - 1.0) * 0.5;
    float maxR = length(c);
    float r = length(px - c) / maxR;
    float tt = smooth01(r, uEff.y, uEff.y + uEff.z / 100.0);
    float f = clamp(1.0 + uEff.x / 100.0 * tt, 0.0, 2.0);
    t = clamp(t * f, 0.0, 1.0);
  }
  // 颗粒（噪声纹理 R = 块展开图：逐像素值即所在块 hashNoise 值，与 CPU oracle 同源）
  if (uEff.w != 0.0 && uGrainAmp != 0.0) {
    float n = texture2D(uNoise, nuv).r;
    float d = (n * 2.0 - 1.0) * uGrainAmp;
    t = clamp(t + vec3(d), 0.0, 1.0);
  }
  // S7 输出编码（sRGB + 确定性 TPDF）
  vec2 dith = texture2D(uNoise, nuv).gb;
  float d = (dith.x + dith.y) * 0.5 - 0.5;
  vec3 enc = vec3(linearToSrgb(log16Decode(t.r)),
                  linearToSrgb(log16Decode(t.g)),
                  linearToSrgb(log16Decode(t.b)));
  vec3 q = floor(enc * 255.0 + 0.5 + d) / 255.0;
  gl_FragColor = vec4(clamp(q, 0.0, 1.0), 1.0);
}
`;

/** P6：合成（split 对比；T2.7 起蒙版在独立 mask pass（tone 后 sharpen 前），
 *  本 shader 不再含蒙版数学） */
export const FRAG_COMPOSITE = `
precision highp float;
varying vec2 vUv;
uniform sampler2D uA;
uniform sampler2D uB;
uniform float uSplit;

void main() {
  // 图像采样翻转：GL 帧缓冲行 0 = 显示底部，而源纹理数据行 0 = 图像顶部（imageToCodes
  // 顶行在前），不翻转则画布呈现为上下镜像（合成图是渐变长期未被发现；parity93 的
  // readPixels 对比恰好同为 GL 行序而互相抵消）。
  vec2 iUv = vec2(vUv.x, 1.0 - vUv.y);
  vec3 cA = texture2D(uA, iUv).rgb;
  vec3 c;
  if (uSplit < 0.0) {
    c = cA;
  } else {
    vec3 cB = texture2D(uB, iUv).rgb;
    c = vUv.x < uSplit ? cB : cA;
  }
  gl_FragColor = vec4(clamp(c, 0.0, 1.0), 1.0);
}
`;

/** P3.5 独立蒙版 pass（T2.7，架构 §3.1.3/§3.1.4；engine mask_field.cpp + mask.cpp 逐式镜像）。
 *  位置 = tone 之后、sharpen 之前（S4 末蒙版阶段）；输入/输出均为 t 域（u = t = code/65535，
 *  与 engine codeAt/65535 同口径）。5 类因子 + 布尔 fold + 9 局部叶 + clarity 共享模糊。 */
export const FRAG_MASK = `
${COMMON}
uniform sampler2D uMaskData;   // 192x1 RGBA32F：32 槽 × 6 行参数（maskField.ts buildMaskDataTex）
uniform sampler2D uMaskField;  // 1024x2048 LUMINANCE：画笔距离场图集（4 列 × 8 行）
uniform sampler2D uBlur;       // 共享盒式模糊平面（r10，t 域；仅 clarity 消费）
uniform int uMaskCount;
uniform float uHasBlur;        // 1 = uBlur 有效（存在 enabled 且 clarity≠0 的蒙版）

const vec3 LUM3 = vec3(0.2126, 0.7152, 0.0722);
const float kRowsPer = 6.0;
const float kDataW = 32.0 * kRowsPer;   // 192

vec4 fetchRow(float r) {
  return texture2D(uMaskData, vec2((r + 0.5) / kDataW, 0.5));
}

// 画笔图集采样（NEAREST texel fetch + 手动双线性；镜像 maskField.ts sampleAtlas /
// engine sampleBrushField256：fx/fy 截 [0, CELL-1]，4 texel lerp）
float sampleBrushAtlas(float slot, float u, float v) {
  float cell = 256.0;
  float cols = 4.0;
  float rows = 8.0;
  float fx = clamp(u * cell - 0.5, 0.0, cell - 1.0);
  float fy = clamp(v * cell - 0.5, 0.0, cell - 1.0);
  float x0 = floor(fx);
  float y0 = floor(fy);
  float tx = fx - x0;
  float ty = fy - y0;
  float x1 = min(x0 + 1.0, cell - 1.0);
  float y1 = min(y0 + 1.0, cell - 1.0);
  float col = mod(slot, cols);
  float row = floor(slot / cols);
  float ax = (col * cell + x0 + 0.5) / (cols * cell);
  float bx = (col * cell + x1 + 0.5) / (cols * cell);
  float ay = (row * cell + y0 + 0.5) / (rows * cell);
  float by = (row * cell + y1 + 0.5) / (rows * cell);
  float a = texture2D(uMaskField, vec2(ax, ay)).r;
  float b = texture2D(uMaskField, vec2(bx, ay)).r;
  float c = texture2D(uMaskField, vec2(ax, by)).r;
  float d = texture2D(uMaskField, vec2(bx, by)).r;
  return (a * (1.0 - tx) + b * tx) * (1.0 - ty) + (c * (1.0 - tx) + d * tx) * ty;
}

void main() {
  vec3 u3 = texture2D(uTex, vUv).rgb;
  if (uMaskCount <= 0) { gl_FragColor = vec4(u3, 1.0); return; }
  vec2 px = floor(vUv * uTexSize);
  float mu = (px.x + 0.5) / uTexSize.x;
  float mv = (px.y + 0.5) / uTexSize.y;   // FBO 数据行 0 = 图像顶行（v 向下，engine uvV 同口径）
  float fold = 0.0;
  float havePrev = 0.0;
  for (int i = 0; i < 32; i++) {
    float fi = float(i);
    if (fi < float(uMaskCount)) {
      float base = fi * kRowsPer;
      vec4 r0 = fetchRow(base + 0.0);
      vec4 r1 = fetchRow(base + 1.0);
      vec4 r2 = fetchRow(base + 2.0);
      vec4 r3 = fetchRow(base + 3.0);
      vec4 r4 = fetchRow(base + 4.0);
      vec4 r5 = fetchRow(base + 5.0);
      float type = r0.x;
      float combine = r0.y;
      float opa = r0.w;
      float amt = r1.x;
      // ---- raw 因子（5 类；mask_field.cpp 同式）----
      float raw = 0.0;
      if (type < 0.5) {
        // linear：r1.yzw = x1,y1,x2；r2.x = y2。梯度投影 clamp01；零长度梯度 = 0
        vec2 pa = r1.yz;
        vec2 pb = vec2(r1.w, r2.x);
        vec2 d = pb - pa;
        float len2 = dot(d, d);
        raw = len2 <= 1e-12 ? 0.0
          : clamp(dot(vec2(mu, mv) - pa, d) / len2, 0.0, 1.0);
      } else if (type < 1.5) {
        // radial：r1.yzw = cx,cy,radius；r2.x = ratio；r2.y = feather
        float ratio = r2.x;
        if (ratio <= 1e-9) ratio = 1.0;
        float feather = clamp(r2.y, 0.0, 1.0);
        vec2 d = vec2(mu, mv) - r1.yz;
        d.y /= ratio;
        raw = 1.0 - smooth01(length(d), r1.w * (1.0 - feather), r1.w);
      } else if (type < 2.5) {
        raw = sampleBrushAtlas(fi, mu, mv);
      } else if (type < 3.5) {
        // luminanceRange：r1.yzw = low,high,smooth（基于 tone 输出像素 L）
        float L = dot(u3, LUM3);
        raw = smooth01(L, r1.y - r1.w, r1.y) * (1.0 - smooth01(L, r1.z, r1.z + r1.w));
      } else {
        // colorRange：r1.yzw = hueCenter,hueWidth,satLow；r2.xy = satHigh,smooth
        vec3 hsl = rgb2hsl(clamp(u3, 0.0, 1.0));
        float dh = hsl.x - r1.y;
        dh -= floor(dh + 0.5);
        float halfW = max(0.0, r1.z) * 0.5;
        float fHue = 1.0 - smooth01(abs(dh), halfW * (1.0 - r2.y), halfW);
        float fSat = smooth01(hsl.y, r1.w, r1.w + r2.y) *
                     (1.0 - smooth01(hsl.y, r2.x, r2.x + r2.y));
        raw = fHue * fSat;
      }
      // ---- fold（§3.1.3：首条有效按 replace；disabled 项在数据纹理里 enabled=0 跳过）----
      float f = raw;
      if (havePrev > 0.5) {
        if (combine < 0.5) { f = raw; }
        else if (combine < 1.5) { f = fold * raw; }
        else { f = fold * (1.0 - raw); }
      }
      fold = f;
      havePrev = 1.0;
      // ---- 施加（subtract 只减面积不施加 local；全零 local 与 w==0 跳过）----
      float appliesLocal = combine < 1.5 ? 1.0 : 0.0;
      float w = clamp(f * opa * amt, -1.0, 1.0);
      float zeroLocal = step(abs(r3.x) + abs(r3.y) + abs(r3.z) + abs(r3.w) +
                             abs(r4.x) + abs(r4.y) + abs(r4.z) + abs(r4.w) +
                             abs(r5.x), 1e-9);
      if (appliesLocal > 0.5 && w != 0.0 && zeroLocal < 0.5) {
        // 1 exposure：u *= 2^(E*w)
        if (r3.x != 0.0) { u3 *= exp2(r3.x * w); }
        // 2 contrast：0.5 为轴
        if (r3.y != 0.0) { u3 = (u3 - 0.5) * (1.0 + r3.y / 100.0 * 0.5 * w) + 0.5; }
        // 3 highlights：hw = smooth01(L, 0.45, 1)（L 按当时像素状态现算）
        if (r3.z != 0.0) {
          float L = dot(u3, LUM3);
          u3 += (r3.z / 100.0) * 0.4 * w * smooth01(L, 0.45, 1.0) * (1.0 - u3);
        }
        // 4 shadows：sw = 1 - smooth01(L, 0, 0.55)
        if (r3.w != 0.0) {
          float L = dot(u3, LUM3);
          u3 += (r3.w / 100.0) * 0.4 * w * (1.0 - smooth01(L, 0.0, 0.55)) * (1.0 - u3);
        }
        // 5 temperature
        if (r4.x != 0.0) {
          float g = r4.x / 100.0 * 0.1 * w;
          u3.r *= exp2(g);
          u3.b *= exp2(-g);
        }
        // 6 tint
        if (r4.y != 0.0) { u3.g *= exp2(r4.y / 100.0 * 0.05 * w); }
        // 7 saturation：HSL 往返（前截断与 CPU 同口径）
        if (r4.z != 0.0) {
          vec3 hsl = rgb2hsl(clamp(u3, 0.0, 1.0));
          hsl.y *= 1.0 + r4.z / 100.0 * 0.6 * w;
          hsl.y = clamp(hsl.y, 0.0, 1.0);
          u3 = hsl2rgb(hsl);
        }
        // 8 clarity：u += Cl/100*0.6*w*(u - B)；B = 共享盒式模糊平面（半径 10）
        if (r4.w != 0.0 && uHasBlur > 0.5) {
          u3 += (r4.w / 100.0) * 0.6 * w * (u3 - texture2D(uBlur, vUv).r);
        }
        // 9 dehaze：d≥0 黑场下压；d<0 加雾
        if (r5.x != 0.0) {
          float d = r5.x / 100.0 * w;
          u3 = d >= 0.0 ? (u3 - 0.15 * d) / (1.0 - 0.15 * d)
                        : u3 * (1.0 + 0.3 * d) + 0.10 * (-d);
        }
      }
    }
  }
  gl_FragColor = vec4(u3, 1.0);
}
`;
