/**
 * T1.4 · 93 叶 GPU 管线（GLSL ES 1.0 多 pass，与 ops/oracle93.ts 逐式对应）。
 *
 * 域纪律（与引擎/T1.3 一致）：
 *  - 中间 pass 全部在 log16 码 t 域（float FBO，t = code/65535）；
 *  - S3 toneMap 是唯一域分界；S6 之后再做 sRGB 输出编码；
 *  - 蒙版沿用 app 既有 display 域近似（T2.2 前），只在 A 侧、编码后应用。
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

/** P1a：几何（crop/perspective 单应，逆映射 + 手动双线性，出界填黑） */
export const FRAG_GEO = `
${COMMON}
uniform mat3 uInv;        // 逆单应（dst 像素 → 源全图像素）
uniform vec2 uOutSize;
uniform vec2 uCropOff;    // cropX, cropY
uniform float uGeomActive;

void main() {
  if (uGeomActive < 0.5) { gl_FragColor = vec4(texture2D(uTex, vUv).rgb, 1.0); return; }
  vec2 dstPx = floor(vUv * uOutSize);
  vec3 s = uInv * vec3(dstPx, 1.0);
  vec2 full = s.xy / s.z + uCropOff;
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

/** P1b：lens.manualDistortion（线性域重映射，引擎 lens.cpp 镜像） */
export const FRAG_DISTORT = `
${COMMON}
uniform float uDistK;     // amt/100*0.3

void main() {
  if (uDistK == 0.0) { gl_FragColor = vec4(texture2D(uTex, vUv).rgb, 1.0); return; }
  vec2 px = floor(vUv * uTexSize);
  vec2 c = (uTexSize - 1.0) * 0.5;
  vec2 d = px - c;
  float r = length(d) / length(c);
  float rf = r * (1.0 + uDistK * r * r);
  vec2 src = c + d * (rf / max(1e-9, r));
  vec2 f = floor(src);
  vec2 fr = clamp(src - f, 0.0, 1.0);
  vec3 p0 = fetchPx(f);
  vec3 p1 = fetchPx(f + vec2(1.0, 0.0));
  vec3 p2 = fetchPx(f + vec2(0.0, 1.0));
  vec3 p3 = fetchPx(f + vec2(1.0, 1.0));
  // 线性域双线性（引擎：decode → 插值 → encode）
  vec3 lin0 = vec3(log16Decode(p0.r), log16Decode(p0.g), log16Decode(p0.b));
  vec3 lin1 = vec3(log16Decode(p1.r), log16Decode(p1.g), log16Decode(p1.b));
  vec3 lin2 = vec3(log16Decode(p2.r), log16Decode(p2.g), log16Decode(p2.b));
  vec3 lin3 = vec3(log16Decode(p3.r), log16Decode(p3.g), log16Decode(p3.b));
  vec3 topv = lin0 + (lin1 - lin0) * fr.x;
  vec3 botv = lin2 + (lin3 - lin2) * fr.x;
  vec3 linv = topv + (botv - topv) * fr.y;
  vec3 outv = vec3(log16EncodeF(linv.r), log16EncodeF(linv.g), log16EncodeF(linv.b));
  gl_FragColor = vec4(clamp(outv, 0.0, 1.0), 1.0);
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
uniform sampler2D uCurve;    // 256x1 RGBA NEAREST：r=RGB复合 g=R b=G a=B
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

// 曲线 LUT 查找（256x1，u=t*255 四舍五入取索引；与 CPU applyOne 同口径）
vec2 curveUv(float t) {
  float u = clamp(t, 0.0, 1.0) * 255.0;
  float idx = floor(u + 0.5);
  return clamp(vec2((idx + 0.5) / 256.0, 0.5), vec2(0.5 / 256.0), vec2(1.0 - 0.5 / 256.0, 0.5));
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
  float l = (t.r + t.g + t.b) / 3.0;
  if (uTone.z != 0.0) t = clamp(t + vec3(uTone.z / 100.0 * 3000.0 * smooth01(l, 0.5, 1.0) / 65535.0), 0.0, 1.0);
  l = (t.r + t.g + t.b) / 3.0;
  if (uTone.w != 0.0) t = clamp(t + vec3(uTone.w / 100.0 * 3000.0 * (1.0 - smooth01(l, 0.0, 0.5)) / 65535.0), 0.0, 1.0);
  l = (t.r + t.g + t.b) / 3.0;
  if (uTone2.x != 0.0) t = clamp(t + vec3(uTone2.x / 100.0 * 3000.0 * smooth01(l, 0.75, 1.0) / 65535.0), 0.0, 1.0);
  l = (t.r + t.g + t.b) / 3.0;
  if (uTone2.y != 0.0) t = clamp(t + vec3(uTone2.y / 100.0 * 3000.0 * (1.0 - smooth01(l, 0.0, 0.25)) / 65535.0), 0.0, 1.0);
  // 去雾
  if (uTone2.z != 0.0) {
    float dz = uTone2.z / 100.0;
    l = (t.r + t.g + t.b) / 3.0;
    float mean = l * 65535.0;
    vec3 v = t * 65535.0;
    v += vec3(dz * 1500.0 * (1.0 - smooth01(l, 0.0, 0.5)) - dz * 500.0 * (1.0 - smooth01(l, 0.0, 0.25)));
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
  // curve.cpp 的 if (identity) return true——恒等查表会引入 257 栅格再量化，语义不同）
  if (uCurveSkip.x < 0.5) {
    t.r = clamp(texture2D(uCurve, curveUv(t.r)).r / 255.0, 0.0, 1.0);
    t.g = clamp(texture2D(uCurve, curveUv(t.g)).r / 255.0, 0.0, 1.0);
    t.b = clamp(texture2D(uCurve, curveUv(t.b)).r / 255.0, 0.0, 1.0);
  }
  if (uCurveSkip.y < 0.5) {
    t.r = clamp(texture2D(uCurve, curveUv(t.r)).g / 255.0, 0.0, 1.0);
  }
  if (uCurveSkip.z < 0.5) {
    t.g = clamp(texture2D(uCurve, curveUv(t.g)).b / 255.0, 0.0, 1.0);
  }
  if (uCurveSkip.w < 0.5) {
    t.b = clamp(texture2D(uCurve, curveUv(t.b)).a / 255.0, 0.0, 1.0);
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

/** P6：合成（split + A 侧蒙版局部调整，蒙版代码自旧 shaders.ts 原样移植） */
export const FRAG_COMPOSITE = `
precision highp float;
varying vec2 vUv;
uniform sampler2D uA;
uniform sampler2D uB;
uniform float uSplit;

// ---- 蒙版（WP-B4/B5 既有实现原样移植；T2.2 前为 display 域近似） ----
uniform int uMaskCount;
uniform sampler2D uMaskField;
uniform vec4 uMaskGeoA[6];
uniform vec4 uMaskGeoB[6];
uniform vec4 uMaskLocal0[6];
uniform vec4 uMaskLocal1[6];
uniform float uMaskAmount[6];
uniform float uMaskType[6];
uniform float uMaskOpacity[6];

const vec3 LUM = vec3(0.2126, 0.7152, 0.0722);

float smoothstep5(float edge0, float edge1, float x) {
  float t = clamp((x - edge0) / (edge1 - edge0), 0.0, 1.0);
  return t * t * (3.0 - 2.0 * t);
}

vec3 rgb2hslM(vec3 c) {
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

float hue2rgbM(float p, float q, float t) {
  if (t < 0.0) t += 1.0;
  if (t > 1.0) t -= 1.0;
  if (t < 1.0 / 6.0) return p + (q - p) * 6.0 * t;
  if (t < 0.5) return q;
  if (t < 2.0 / 3.0) return p + (q - p) * (2.0 / 3.0 - t) * 6.0;
  return p;
}

vec3 hsl2rgbM(vec3 hsl) {
  if (hsl.y <= 1e-5) return vec3(hsl.z);
  float q = hsl.z < 0.5 ? hsl.z * (1.0 + hsl.y) : hsl.z + hsl.y - hsl.z * hsl.y;
  float p = 2.0 * hsl.z - q;
  return vec3(hue2rgbM(p, q, hsl.x + 1.0 / 3.0), hue2rgbM(p, q, hsl.x), hue2rgbM(p, q, hsl.x - 1.0 / 3.0));
}

vec3 applyMaskCore(vec3 c, vec2 uv, float active,
                   float type, vec4 ga, vec4 gb, vec4 l0, vec4 l1,
                   float amountRaw, float opacityRaw, vec2 cell) {
  if (active < 0.5) return c;
  float factor = 0.0;
  if (type < 0.5) {
    vec2 d = gb.xy - ga.xy;
    float len2 = max(dot(d, d), 1e-5);
    float t = dot(uv - ga.xy, d) / len2;
    factor = clamp(t, 0.0, 1.0);
  } else if (type < 1.5) {
    vec2 d = uv - ga.xy;
    d.y /= max(gb.w, 0.1);
    factor = 1.0 - smoothstep5(gb.z * 0.55, gb.z, length(d));
  } else {
    vec2 fuv = vec2((cell.x + uv.x) / 3.0, (cell.y + 1.0 - uv.y) / 2.0);
    factor = texture2D(uMaskField, fuv).r;
  }
  factor *= opacityRaw / 100.0;
  if (factor <= 0.001) return c;
  float amount = amountRaw / 100.0;
  float lum = dot(c, LUM);
  c *= pow(2.0, (l0.x + amount * 0.5) * factor * 0.8);
  float hw = smoothstep5(0.45, 1.0, lum);
  float sw = 1.0 - smoothstep5(0.0, 0.55, lum);
  c += (l0.z / 100.0) * 0.4 * hw * factor * (1.0 - c);
  c += (l0.w / 100.0) * 0.4 * sw * factor * (1.0 - c);
  c = (c - 0.5) * (1.0 + (l0.y / 100.0) * 0.5 * factor) + 0.5;
  c.r += (l1.x / 100.0) * 0.1 * factor;
  c.b -= (l1.x / 100.0) * 0.1 * factor;
  vec3 hsl = rgb2hslM(clamp(c, 0.0, 1.0));
  hsl.y *= 1.0 + (l1.z / 100.0) * 0.6 * factor;
  c = hsl2rgbM(clamp(hsl, 0.0, 1.0));
  float mid = 1.0 - abs(lum - 0.5) * 2.0;
  c = (c - 0.5) * (1.0 + (l1.w / 100.0) * 0.3 * mid * factor) + 0.5;
  return clamp(c, 0.0, 1.0);
}

vec3 applyMask0(vec3 c, vec2 uv, float a) {
  return applyMaskCore(c, uv, a, uMaskType[0], uMaskGeoA[0], uMaskGeoB[0], uMaskLocal0[0], uMaskLocal1[0], uMaskAmount[0], uMaskOpacity[0], vec2(0.0, 0.0));
}
vec3 applyMask1(vec3 c, vec2 uv, float a) {
  return applyMaskCore(c, uv, a, uMaskType[1], uMaskGeoA[1], uMaskGeoB[1], uMaskLocal0[1], uMaskLocal1[1], uMaskAmount[1], uMaskOpacity[1], vec2(1.0, 0.0));
}
vec3 applyMask2(vec3 c, vec2 uv, float a) {
  return applyMaskCore(c, uv, a, uMaskType[2], uMaskGeoA[2], uMaskGeoB[2], uMaskLocal0[2], uMaskLocal1[2], uMaskAmount[2], uMaskOpacity[2], vec2(2.0, 0.0));
}
vec3 applyMask3(vec3 c, vec2 uv, float a) {
  return applyMaskCore(c, uv, a, uMaskType[3], uMaskGeoA[3], uMaskGeoB[3], uMaskLocal0[3], uMaskLocal1[3], uMaskAmount[3], uMaskOpacity[3], vec2(0.0, 1.0));
}
vec3 applyMask4(vec3 c, vec2 uv, float a) {
  return applyMaskCore(c, uv, a, uMaskType[4], uMaskGeoA[4], uMaskGeoB[4], uMaskLocal0[4], uMaskLocal1[4], uMaskAmount[4], uMaskOpacity[4], vec2(1.0, 1.0));
}
vec3 applyMask5(vec3 c, vec2 uv, float a) {
  return applyMaskCore(c, uv, a, uMaskType[5], uMaskGeoA[5], uMaskGeoB[5], uMaskLocal0[5], uMaskLocal1[5], uMaskAmount[5], uMaskOpacity[5], vec2(2.0, 1.0));
}

void main() {
  vec3 cA = texture2D(uA, vUv).rgb;
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
    vec3 cB = texture2D(uB, vUv).rgb;
    c = vUv.x < uSplit ? cB : cA;
  }
  gl_FragColor = vec4(clamp(c, 0.0, 1.0), 1.0);
}
`;
