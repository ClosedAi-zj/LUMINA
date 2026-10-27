/**
 * T1.4 · GPU 93 叶管线执行器（WebGL1 多 pass + float FBO）。
 *
 * 与 ops/oracle93.ts 逐 pass 对应：
 *   geo（单应+畸变复合，单次采样）→ [blur+denoiseApply] → [blur+clarity] → [blur+texture]
 *   → tone（S2 逐像素 + S3 toneMap + S4 曲线/HSL/分级）→ mask（T2.7 蒙版，tone 后 sharpen 前）
 *   → [blur+sharpen] → effects（暗角/颗粒/sRGB 编码）→ composite（split）。
 *
 * 中间缓冲 = RGBA32F（t = log16 码 / 65535）；最终 sRGB 输出 = RGBA8。
 * 蒙版 = 独立 mask pass（32 槽数据纹理 + 4×8 画笔场图集，T2.7）。
 */

import type { EditParams, MaskLayer } from '../../../../api/protocol';
import {
  FRAG_BLUR_H,
  FRAG_BLUR_V,
  FRAG_COMPOSITE,
  FRAG_DENOISE_APPLY,
  FRAG_EFFECTS,
  FRAG_GEO,
  FRAG_LOCAL_CONTRAST,
  FRAG_MASK,
  FRAG_SHARPEN,
  FRAG_TONE,
  FRAG_WB,
  PASS_VERT,
} from './glsl';
import { buildGeomTransform, buildGrainNoise } from './oracle93';
import { hashNoise } from './log16';
import { buildMaskDataTex, maskNeedClarityBlur, MAX_MASK_SLOTS, MASK_DATA_ROWS } from '../maskField';

const MAX_BLUR_RADIUS = 16;

/** 管线 pass 链（代码级顺序断言的权威表；runSide 按此序执行） */
export const PASS_CHAIN = [
  'wb', 'geo', 'denoise', 'localContrast', 'tone', 'mask', 'sharpen', 'effects',
] as const;

/** mask pass 位置代码断言：必须位于 tone 之后、sharpen 之前（架构 §3.1.4 T2.7 验收） */
export function assertMaskPassOrder(seq: readonly string[]): void {
  const iSeq = (n: string): number => {
    const i = seq.indexOf(n);
    if (i < 0) throw new Error(`mask-pass-order: pass '${n}' 未在序列中登记`);
    return i;
  };
  const iChainTone = PASS_CHAIN.indexOf('tone');
  const iChainMask = PASS_CHAIN.indexOf('mask');
  const iChainSharp = PASS_CHAIN.indexOf('sharpen');
  if (!(iChainTone < iChainMask && iChainMask < iChainSharp)) {
    throw new Error('mask-pass-order: PASS_CHAIN 常量表顺序错误');
  }
  const iTone = iSeq('tone');
  const iMask = iSeq('mask');
  const iSharp = seq.indexOf('sharpen');
  if (!(iMask > iTone && (iSharp < 0 || iSharp > iMask))) {
    throw new Error(
      `mask-pass-order: 运行时序列违规 tone=${iTone} mask=${iMask} sharpen=${iSharp}`,
    );
  }
}

interface Fbo {
  tex: WebGLTexture;
  fbo: WebGLFramebuffer;
  w: number;
  h: number;
  float: boolean;
}

interface Prog {
  prog: WebGLProgram;
  u: Record<string, WebGLUniformLocation | null>;
}

function uniformNames(frag: string): string[] {
  const names = new Set<string>();
  const re = /uniform\s+\w+\s+([A-Za-z_]\w*(\[\d+\])?)/g;
  let m: RegExpExecArray | null;
  while ((m = re.exec(frag))) names.add(m[1]);
  return Array.from(names);
}

export class PipelineGL {
  private gl: WebGLRenderingContext | null = null;
  private progs: Record<string, Prog> = {};
  private srcTex: WebGLTexture | null = null;
  private curveTex: WebGLTexture | null = null;
  private noiseTex: WebGLTexture | null = null;
  private fieldTex: WebGLTexture | null = null;
  private maskDataTex: WebGLTexture | null = null;
  private fbos = new Map<string, Fbo>();
  private quad = false;
  private srcW = 0;
  private srcH = 0;
  private noiseKey = '';
  private shaderWarnings = '';
  /** 调试：最近一次 runSide 的 tone 输出 FBO（A 侧） */
  private lastToneFbo: Fbo | null = null;
  /** 四条曲线恒等跳过标志（x=RGB复合 y=R z=G w=B） */
  private curveSkip: number[] = [0, 0, 0, 0];
  /** 当前 mask pass 蒙版数（setMaskData 写入；0 = mask pass 透传） */
  private maskCount = 0;
  /** 最近一次 runSide 实际执行的 pass 序列（mask 位置断言用） */
  lastPassSeq: string[] = [];

  get warnings(): string {
    return this.shaderWarnings;
  }

  get ready(): boolean {
    return this.gl !== null && this.srcTex !== null;
  }

  init(gl: WebGLRenderingContext): boolean {
    this.gl = gl;
    // float 纹理/渲染目标扩展（中间 pass t 域必需；不可用则整管失败 → CPU 保底）
    if (!gl.getExtension('OES_texture_float')) return false;
    gl.getExtension('WEBGL_color_buffer_float');
    const passFrags: Record<string, string> = {
      wb: FRAG_WB,
      geo: FRAG_GEO,
      blurH: FRAG_BLUR_H,
      blurV: FRAG_BLUR_V,
      denoiseApply: FRAG_DENOISE_APPLY,
      localContrast: FRAG_LOCAL_CONTRAST,
      tone: FRAG_TONE,
      mask: FRAG_MASK,
      sharpen: FRAG_SHARPEN,
      effects: FRAG_EFFECTS,
      composite: FRAG_COMPOSITE,
    };
    // 代码级断言（T2.7 验收）：mask pass 必须位于 tone 之后、sharpen 之前
    assertMaskPassOrder(PASS_CHAIN);
    const vs = this.compile(gl.VERTEX_SHADER, PASS_VERT);
    if (!vs) return false;
    for (const [name, frag] of Object.entries(passFrags)) {
      const fs = this.compile(gl.FRAGMENT_SHADER, frag);
      if (!fs) return false;
      const prog = gl.createProgram();
      if (!prog) return false;
      gl.attachShader(prog, vs);
      gl.attachShader(prog, fs);
      gl.linkProgram(prog);
      if (!gl.getProgramParameter(prog, gl.LINK_STATUS)) {
        console.warn(`[develop] program ${name} link 失败:`, gl.getProgramInfoLog(prog));
        return false;
      }
      const u: Record<string, WebGLUniformLocation | null> = {};
      for (const un of uniformNames(frag)) {
        u[un] = gl.getUniformLocation(prog, un);
      }
      this.progs[name] = { prog, u };
    }
    if (!this.quad) {
      const buf = gl.createBuffer();
      gl.bindBuffer(gl.ARRAY_BUFFER, buf);
      gl.bufferData(gl.ARRAY_BUFFER, new Float32Array([-1, -1, 1, -1, -1, 1, 1, 1]), gl.STATIC_DRAW);
      this.quad = true;
    }
    return true;
  }

  private compile(type: number, src: string): WebGLShader | null {
    const gl = this.gl!;
    const sh = gl.createShader(type);
    if (!sh) return null;
    gl.shaderSource(sh, src);
    gl.compileShader(sh);
    const log = (gl.getShaderInfoLog(sh) ?? '').trim();
    if (log) this.shaderWarnings = (this.shaderWarnings ? this.shaderWarnings + ' | ' : '') + log;
    if (!gl.getShaderParameter(sh, gl.COMPILE_STATUS)) {
      console.warn('[develop] shader 编译失败:', log);
      return null;
    }
    return sh;
  }

  /** 上传源图（log16 码 t 域 float，交错 RGB→RGBA） */
  setSourceCodes(t: Float32Array, w: number, h: number): boolean {
    const gl = this.gl;
    if (!gl) return false;
    this.srcW = w;
    this.srcH = h;
    if (!this.srcTex) this.srcTex = gl.createTexture();
    gl.bindTexture(gl.TEXTURE_2D, this.srcTex);
    this.texParams();
    const rgba = new Float32Array(w * h * 4);
    for (let i = 0, j = 0; i < w * h; ++i, j += 3) {
      rgba[i * 4] = t[j];
      rgba[i * 4 + 1] = t[j + 1];
      rgba[i * 4 + 2] = t[j + 2];
      rgba[i * 4 + 3] = 1;
    }
    gl.texImage2D(gl.TEXTURE_2D, 0, gl.RGBA, w, h, 0, gl.RGBA, gl.FLOAT, rgba);
    return true;
  }

  /** 曲线 LUT：1024x1 RGBA float = (RGB复合, R, G, B)，各 0..255（float 纹理，
   *  与 CPU Float64 LUT 逐值一致，规避 8bit 量化引入的 ±0.5 LSB 偏差）。
   *  T2.9：宽度随 buildCurveLutData 输出自适应（lut.length/4，当前 1024）。
   *  skip = 四条曲线是否恒等（引擎跳过恒等查表，见 glsl.ts FRAG_TONE 注）。 */
  setCurves(lut: Float32Array, skip: boolean[]): void {
    const gl = this.gl;
    if (!gl) return;
    this.curveSkip = [
      skip[0] ? 1 : 0, skip[1] ? 1 : 0, skip[2] ? 1 : 0, skip[3] ? 1 : 0,
    ];
    if (!this.curveTex) this.curveTex = gl.createTexture();
    gl.bindTexture(gl.TEXTURE_2D, this.curveTex);
    this.texParams();
    gl.texImage2D(gl.TEXTURE_2D, 0, gl.RGBA, lut.length / 4, 1, 0, gl.RGBA, gl.FLOAT, lut);
  }

  /** 噪声纹理（R=颗粒块噪声（块内复制） G/B=TPDF u1/u2，w×h 与当前缓冲同尺寸）。
   *  float 纹理存精确哈希值，与 CPU oracle 逐位同源（8bit 量化会引入 ~0.5 LSB 级偏差）。 */
  private ensureNoise(w: number, h: number, size: number, grainOn: boolean): void {
    const gl = this.gl!;
    const key = `${w}x${h}x${size}x${grainOn ? 1 : 0}`;
    if (this.noiseTex && key === this.noiseKey) return;
    this.noiseKey = key;
    if (!this.noiseTex) this.noiseTex = gl.createTexture();
    const data = new Float32Array(w * h * 4);
    if (grainOn) {
      const grain = buildGrainNoise(w, h, size);
      for (let y = 0; y < h; ++y) {
        for (let x = 0; x < w; ++x) {
          const bx = Math.min(grain.gw - 1, Math.trunc(x / size));
          const by = Math.min(grain.gh - 1, Math.trunc(y / size));
          data[(y * w + x) * 4] = grain.grain[by * grain.gw + bx];
        }
      }
    }
    for (let y = 0; y < h; ++y) {
      for (let x = 0; x < w; ++x) {
        const i = (y * w + x) * 4;
        data[i + 1] = hashNoise(x, y, 1000);
        data[i + 2] = hashNoise(x + 7919, y + 104729, 2000);
        data[i + 3] = 1;
      }
    }
    gl.bindTexture(gl.TEXTURE_2D, this.noiseTex);
    this.texParams();
    gl.texImage2D(gl.TEXTURE_2D, 0, gl.RGBA, w, h, 0, gl.RGBA, gl.FLOAT, data);
  }

  private texParams(): void {
    const gl = this.gl!;
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_S, gl.CLAMP_TO_EDGE);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_T, gl.CLAMP_TO_EDGE);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MIN_FILTER, gl.NEAREST);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MAG_FILTER, gl.NEAREST);
  }

  /** 画笔蒙版距离场图集（4×8 = 32 槽；沿用 WP-B5 上传路径） */
  setFieldAtlas(bytes: Uint8Array, cols: number, rows: number): void {
    const gl = this.gl;
    if (!gl) return;
    if (!this.fieldTex) this.fieldTex = gl.createTexture();
    gl.bindTexture(gl.TEXTURE_2D, this.fieldTex);
    this.texParams();
    gl.texImage2D(
      gl.TEXTURE_2D, 0, gl.LUMINANCE, cols * 256, rows * 256, 0, gl.LUMINANCE,
      gl.UNSIGNED_BYTE, bytes,
    );
  }

  /** 蒙版数据纹理（T2.7）：32 槽 × 6 行 RGBA32F（buildMaskDataTex 打包）。
   *  超容量显式失败（A4 容量断言：32 槽硬上限，无静默截断）。 */
  setMaskData(masks: MaskLayer[]): void {
    const gl = this.gl;
    if (!gl) return;
    if (masks.length > MAX_MASK_SLOTS) {
      throw new Error(`mask 槽容量溢出：${masks.length} > ${MAX_MASK_SLOTS}`);
    }
    const { data, count } = buildMaskDataTex(masks);
    this.maskCount = count;
    if (count === 0) return;
    if (!this.maskDataTex) this.maskDataTex = gl.createTexture();
    gl.bindTexture(gl.TEXTURE_2D, this.maskDataTex);
    this.texParams();
    gl.texImage2D(
      gl.TEXTURE_2D, 0, gl.RGBA, MAX_MASK_SLOTS * MASK_DATA_ROWS, 1, 0, gl.RGBA,
      gl.FLOAT, data,
    );
  }

  private fbo(w: number, h: number, slot: number, float: boolean): Fbo {
    const gl = this.gl!;
    const key = `${w}x${h}#${slot}#${float ? 1 : 0}`;
    let f = this.fbos.get(key);
    if (!f) {
      const tex = gl.createTexture()!;
      gl.bindTexture(gl.TEXTURE_2D, tex);
      this.texParams();
      gl.texImage2D(
        gl.TEXTURE_2D, 0, gl.RGBA, w, h, 0, gl.RGBA,
        float ? gl.FLOAT : gl.UNSIGNED_BYTE, null,
      );
      const fb = gl.createFramebuffer()!;
      gl.bindFramebuffer(gl.FRAMEBUFFER, fb);
      gl.framebufferTexture2D(gl.FRAMEBUFFER, gl.COLOR_ATTACHMENT0, gl.TEXTURE_2D, tex, 0);
      gl.bindFramebuffer(gl.FRAMEBUFFER, null);
      f = { tex, fbo: fb, w, h, float };
      this.fbos.set(key, f);
    }
    return f;
  }

  private drawPass(
    name: string,
    input: WebGLTexture,
    inW: number,
    inH: number,
    target: Fbo | null,
    setUniforms: (p: Prog) => void,
  ): void {
    const gl = this.gl!;
    const p = this.progs[name];
    gl.useProgram(p.prog);
    gl.bindFramebuffer(gl.FRAMEBUFFER, target ? target.fbo : null);
    gl.viewport(0, 0, target ? target.w : inW, target ? target.h : inH);
    gl.activeTexture(gl.TEXTURE0);
    gl.bindTexture(gl.TEXTURE_2D, input);
    gl.uniform1i(p.u['uTex'] ?? null, 0);
    gl.uniform2f(p.u['uTexSize'] ?? null, inW, inH);
    setUniforms(p);
    const loc = gl.getAttribLocation(p.prog, 'aPos');
    gl.enableVertexAttribArray(loc);
    gl.vertexAttribPointer(loc, 2, gl.FLOAT, false, 0, 0);
    gl.drawArrays(gl.TRIANGLE_STRIP, 0, 4);
  }

  private blurPair(
    input: WebGLTexture,
    w: number,
    h: number,
    slot: number,
    radius: number,
    mode: number,
  ): WebGLTexture {
    const gl = this.gl!;
    const r = Math.min(MAX_BLUR_RADIUS, Math.max(0, Math.round(radius)));
    const bh = this.fbo(w, h, slot, true);
    const bv = this.fbo(w, h, slot + 1, true);
    this.drawPass('blurH', input, w, h, bh, (p) => {
      gl.uniform1f(p.u['uRadius'] ?? null, r);
      gl.uniform1f(p.u['uMode'] ?? null, mode);
    });
    this.drawPass('blurV', bh.tex, w, h, bv, (p) => {
      gl.uniform1f(p.u['uRadius'] ?? null, r);
      gl.uniform1f(p.u['uMode'] ?? null, mode);
    });
    return bv.tex;
  }

  /** 单侧完整管线。槽位：0=几何后 1=乒乓输出 2/3=模糊对A 4/5=模糊对B
   *  6=tone 输出（亦可作 WB 暂存，先写先读、顺序复用） 7=sRGB输出 */
  private runSide(
    params: EditParams,
    slotBase: number,
  ): { tex: WebGLTexture; w: number; h: number } {
    const gl = this.gl!;
    this.lastPassSeq = [];
    const geom = buildGeomTransform(params, this.srcW, this.srcH);
    const gw = geom.inv ? geom.outW : this.srcW;
    const gh = geom.inv ? geom.outH : this.srcH;

    // P0 S0 白平衡（源尺寸上逐像素，先于几何；与 CPU applyWb 同序两趟）
    const tTemp = params.whiteBalance === 'custom' ? (params.temperature ?? 0) * 0.002 : 0;
    const tTint = params.whiteBalance === 'custom' ? (params.tint ?? 0) * 0.001 : 0;
    let inputTex: WebGLTexture = this.srcTex!;
    if (tTemp !== 0 || tTint !== 0) {
      const wbOut = this.fbo(this.srcW, this.srcH, slotBase + 6, true);
      this.drawPass('wb', inputTex, this.srcW, this.srcH, wbOut, (p) => {
        gl.uniform2f(p.u['uWb'] ?? null, tTemp, tTint);
      });
      inputTex = wbOut.tex;
    }

    // P1a S1 几何（T2.10 合并采样：crop/perspective/aspect 单应 + 畸变复合，一次采样；
    // buildGeomTransform 在 distK≠0 而无几何变换时也返回恒等 inv → 统一走本 pass）
    const slot0 = this.fbo(gw, gh, slotBase, true);
    if (geom.inv) {
      const cm = geom.inv.m;
      const colMajor = new Float32Array([
        cm[0], cm[3], cm[6], cm[1], cm[4], cm[7], cm[2], cm[5], cm[8],
      ]);
      this.drawPass('geo', inputTex, this.srcW, this.srcH, slot0, (p) => {
        gl.uniformMatrix3fv(p.u['uInv'] ?? null, false, colMajor);
        gl.uniform2f(p.u['uOutSize'] ?? null, gw, gh);
        gl.uniform2f(p.u['uCropOff'] ?? null, geom.cropX, geom.cropY);
        gl.uniform1f(p.u['uGeomActive'] ?? null, 1);
        gl.uniform1f(p.u['uDistK'] ?? null, geom.distK);
      });
    } else {
      this.drawPass('geo', inputTex, this.srcW, this.srcH, slot0, (p) => {
        gl.uniform1f(p.u['uGeomActive'] ?? null, 0);
      });
    }
    let curTex: WebGLTexture = slot0.tex;
    const curW = gw;
    const curH = gh;
    // 乒乓输出目标：始终避开当前输入纹理（slot0 几何输出在 S1 之后即死亡，可复用）
    const outTarget = (): Fbo => {
      const s1 = this.fbo(curW, curH, slotBase + 1, true);
      return curTex === s1.tex ? slot0 : s1;
    };

    // S1.5 NR（引擎顺序：先色度后亮度，亮度模糊必须读色度应用后的缓冲）
    const cnr = params.detail?.colorNR ?? 0;
    const cnrDetail = params.detail?.colorNRDetail ?? 50;
    const lumaAmt = params.detail?.luminanceSmoothing ?? 0;
    if (cnr > 0) {
      const chromaR = Math.max(1, Math.round((cnr / 100) * 5));
      const blurC = this.blurPair(curTex, curW, curH, slotBase + 2, chromaR, 1);
      const out = outTarget();
      this.drawPass('denoiseApply', curTex, curW, curH, out, (p) => {
        gl.activeTexture(gl.TEXTURE1);
        gl.bindTexture(gl.TEXTURE_2D, blurC);
        gl.uniform1i(p.u['uBlurC'] ?? null, 1);
        gl.uniform1i(p.u['uBlurL'] ?? null, 2);
        gl.uniform1f(p.u['uHasChroma'] ?? null, 1);
        gl.uniform1f(p.u['uChromaKeep'] ?? null, (cnrDetail / 100) * 0.5);
        gl.uniform1f(p.u['uHasLuma'] ?? null, 0);
        gl.uniform1f(p.u['uLumaAmt'] ?? null, 0);
      });
      curTex = out.tex;
    }
    if (lumaAmt > 0) {
      const lumaR = Math.max(1, Math.round((lumaAmt / 100) * 3));
      const blurL = this.blurPair(curTex, curW, curH, slotBase + 4, lumaR, 0);
      const out = outTarget();
      this.drawPass('denoiseApply', curTex, curW, curH, out, (p) => {
        gl.activeTexture(gl.TEXTURE2);
        gl.bindTexture(gl.TEXTURE_2D, blurL);
        gl.uniform1i(p.u['uBlurC'] ?? null, 1);
        gl.uniform1i(p.u['uBlurL'] ?? null, 2);
        gl.uniform1f(p.u['uHasChroma'] ?? null, 0);
        gl.uniform1f(p.u['uChromaKeep'] ?? null, 0);
        gl.uniform1f(p.u['uHasLuma'] ?? null, 1);
        gl.uniform1f(p.u['uLumaAmt'] ?? null, lumaAmt / 100);
      });
      curTex = out.tex;
    }

    // S2 局部对比：clarity（r10 g0.6）→ texture（r2 g0.8）顺序两步
    const localStep = (amt: number, radius: number, gain: number): void => {
      if (amt === 0) return;
      const blur = this.blurPair(curTex, curW, curH, slotBase + 2, radius, 0);
      const out = outTarget();
      this.drawPass('localContrast', curTex, curW, curH, out, (p) => {
        gl.activeTexture(gl.TEXTURE1);
        gl.bindTexture(gl.TEXTURE_2D, blur);
        gl.uniform1i(p.u['uBlur'] ?? null, 1);
        gl.uniform1f(p.u['uGain'] ?? null, (amt / 100) * gain);
      });
      curTex = out.tex;
    };
    localStep(params.clarity ?? 0, 10, 0.6);
    localStep(params.texture ?? 0, 2, 0.8);

    // P3 tone（S2 逐像素 + S3 toneMap + S4 曲线/HSL/分级）→ 槽 6（勿与输入同槽）
    {
      const out = this.fbo(curW, curH, slotBase + 6, true);
      this.drawPass('tone', curTex, curW, curH, out, (p) => {
        this.setToneUniforms(gl, p, params);
      });
      curTex = out.tex;
      if (slotBase === 0) this.lastToneFbo = out;
    }
    this.lastPassSeq.push('tone');

    // P3.5 mask（T2.7 蒙版阶段：tone 后 sharpen 前；uCount=0 时透传直拷保持序列恒定）
    {
      const needClarityBlur = this.maskCount > 0 &&
        (params.masks ?? []).some(maskNeedClarityBlur);
      if (needClarityBlur) {
        // 共享盒式模糊平面（半径 10，进入蒙版阶段的码域快照；每次渲染只算 1 次）
        this.blurPair(curTex, curW, curH, slotBase + 2, 10, 0);
      }
      const out = outTarget();
      this.drawPass('mask', curTex, curW, curH, out, (p) => {
        gl.activeTexture(gl.TEXTURE1);
        gl.bindTexture(gl.TEXTURE_2D, this.maskDataTex);
        gl.uniform1i(p.u['uMaskData'] ?? null, 1);
        gl.activeTexture(gl.TEXTURE2);
        gl.bindTexture(gl.TEXTURE_2D, this.fieldTex);
        gl.uniform1i(p.u['uMaskField'] ?? null, 2);
        gl.activeTexture(gl.TEXTURE3);
        gl.bindTexture(gl.TEXTURE_2D, needClarityBlur ? this.fbo(curW, curH, slotBase + 3, true).tex : this.srcTex);
        gl.uniform1i(p.u['uBlur'] ?? null, 3);
        gl.uniform1i(p.u['uMaskCount'] ?? null, this.maskCount);
        gl.uniform1f(p.u['uHasBlur'] ?? null, needClarityBlur ? 1 : 0);
      });
      curTex = out.tex;
    }
    this.lastPassSeq.push('mask');

    // S5 锐化（unsharp + masking）
    const sharpAmt = params.detail?.sharpness ?? 0;
    if (sharpAmt !== 0) {
      const radius = Math.max(1, params.detail?.sharpenRadius ?? 1);
      const blur = this.blurPair(curTex, curW, curH, slotBase + 2, radius, 0);
      const out = outTarget();
      this.drawPass('sharpen', curTex, curW, curH, out, (p) => {
        gl.activeTexture(gl.TEXTURE1);
        gl.bindTexture(gl.TEXTURE_2D, blur);
        gl.uniform1i(p.u['uBlur'] ?? null, 1);
        gl.uniform4f(
          p.u['uSharp'] ?? null,
          sharpAmt,
          params.detail?.sharpenRadius ?? 1,
          params.detail?.sharpenDetail ?? 25,
          params.detail?.sharpenMasking ?? 0,
        );
      });
      curTex = out.tex;
      this.lastPassSeq.push('sharpen');
    }

    // P5 effects（暗角 + 颗粒）+ S7 sRGB 编码
    const vignAmt = params.optics?.vignetteAmount ?? 0;
    const grainAmt = params.optics?.grainAmount ?? 0;
    const grainSize = Math.max(1, params.optics?.grainSize ?? 0);
    const freq = params.optics?.grainFrequency ?? 0;
    const amp = grainAmt !== 0 ? ((grainAmt / 100) * 1500 * (1 + (freq / 100) * 0.5)) / 65535 : 0;
    this.ensureNoise(curW, curH, grainSize, grainAmt !== 0);
    const out8 = this.fbo(curW, curH, slotBase + 7, false);
    this.drawPass('effects', curTex, curW, curH, out8, (p) => {
      gl.activeTexture(gl.TEXTURE1);
      gl.bindTexture(gl.TEXTURE_2D, this.noiseTex);
      gl.uniform1i(p.u['uNoise'] ?? null, 1);
      gl.uniform4f(
        p.u['uEff'] ?? null,
        vignAmt,
        (params.optics?.vignetteMidpoint ?? 50) / 100,
        Math.max(1, params.optics?.vignetteFeather ?? 50),
        grainAmt,
      );
      gl.uniform1f(p.u['uGrainAmp'] ?? null, amp);
      gl.uniform2f(p.u['uNoiseSize'] ?? null, curW, curH);
    });
    return { tex: out8.tex, w: curW, h: curH };
  }

  private setToneUniforms(gl: WebGLRenderingContext, p: Prog, params: EditParams): void {
    gl.activeTexture(gl.TEXTURE1);
    gl.bindTexture(gl.TEXTURE_2D, this.curveTex);
    gl.uniform1i(p.u['uCurve'] ?? null, 1);
    const grade = params.colorGrade ?? ({} as EditParams['colorGrade']);
    const calib = params.calibration ?? ({} as EditParams['calibration']);
    const param = params.parametric ?? ({} as NonNullable<EditParams['parametric']>);
    gl.uniform4f(
      p.u['uTone'] ?? null,
      params.exposure ?? 0,
      params.contrast ?? 0,
      params.highlights ?? 0,
      params.shadows ?? 0,
    );
    gl.uniform4f(p.u['uTone2'] ?? null, params.whites ?? 0, params.blacks ?? 0, params.dehaze ?? 0, 0);
    gl.uniform4f(p.u['uSatV'] ?? null, params.vibrance ?? 0, params.saturation ?? 0, 0, 0);
    gl.uniform4f(
      p.u['uCalib0'] ?? null,
      calib.shadowTint ?? 0,
      calib.redHue ?? 0,
      calib.redSat ?? 0,
      calib.greenHue ?? 0,
    );
    gl.uniform4f(
      p.u['uCalib1'] ?? null,
      calib.greenSat ?? 0,
      calib.blueHue ?? 0,
      calib.blueSat ?? 0,
      0,
    );
    gl.uniform4f(
      p.u['uParam'] ?? null,
      param.highlights ?? 0,
      param.lights ?? 0,
      param.darks ?? 0,
      param.shadows ?? 0,
    );
    gl.uniform4f(
      p.u['uSplits'] ?? null,
      ((param.shadowSplit ?? 25) / 100) * 255,
      ((param.lightSplit ?? 50) / 100) * 255,
      ((param.midSplit ?? 75) / 100) * 255,
      0,
    );
    gl.uniformMatrix4fv(p.u['uHsl0'] ?? null, false, this.hslMat(params, 0));
    gl.uniformMatrix4fv(p.u['uHsl1'] ?? null, false, this.hslMat(params, 4));
    gl.uniform4f(
      p.u['uCurveSkip'] ?? null,
      this.curveSkip[0],
      this.curveSkip[1],
      this.curveSkip[2],
      this.curveSkip[3],
    );
    gl.uniform4f(
      p.u['uGrade0'] ?? null,
      grade.shadowHue ?? 0,
      grade.shadowSat ?? 0,
      grade.midtoneHue ?? 0,
      grade.midtoneSat ?? 0,
    );
    gl.uniform4f(
      p.u['uGrade1'] ?? null,
      grade.highlightHue ?? 0,
      grade.highlightSat ?? 0,
      grade.blend ?? 50,
      grade.balance ?? 0,
    );
  }

  private hslMat(params: EditParams, start: number): Float32Array {
    const order = ['red', 'orange', 'yellow', 'green', 'aqua', 'blue', 'purple', 'magenta'];
    const m = new Float32Array(16);
    const bands = params.hsl as
      | Record<string, { hue?: number; sat?: number; lum?: number } | undefined>
      | undefined;
    for (let i = 0; i < 4; ++i) {
      const b = bands?.[order[start + i]];
      m.set([b?.hue ?? 0, b?.sat ?? 0, b?.lum ?? 0, 0], i * 4);
    }
    return m;
  }

  /** 完整渲染（A/B 双侧 + composite 到画布；蒙版 = 各侧 params.masks 经 mask pass） */
  render(
    canvas: HTMLCanvasElement,
    params: EditParams,
    before: EditParams | null,
    split: number | null,
  ): boolean {
    const gl = this.gl;
    if (!gl || !this.srcTex) return false;
    this.setMaskData(params.masks ?? []);
    const sideA = this.runSide(params, 0);
    if (before) {
      this.setMaskData(before.masks ?? []);
    }
    const sideB = before ? this.runSide(before, 8) : sideA;
    const outW = sideA.w;
    const outH = sideA.h;
    if (canvas.width !== outW || canvas.height !== outH) {
      canvas.width = outW;
      canvas.height = outH;
    }
    const pc = this.progs['composite'];
    gl.useProgram(pc.prog);
    gl.bindFramebuffer(gl.FRAMEBUFFER, null);
    gl.viewport(0, 0, outW, outH);
    gl.activeTexture(gl.TEXTURE0);
    gl.bindTexture(gl.TEXTURE_2D, sideA.tex);
    gl.uniform1i(pc.u['uA'] ?? null, 0);
    gl.activeTexture(gl.TEXTURE1);
    gl.bindTexture(gl.TEXTURE_2D, sideB.tex);
    gl.uniform1i(pc.u['uB'] ?? null, 1);
    gl.uniform1f(pc.u['uSplit'] ?? null, split === null ? -1 : split);
    const loc = gl.getAttribLocation(pc.prog, 'aPos');
    gl.enableVertexAttribArray(loc);
    gl.vertexAttribPointer(loc, 2, gl.FLOAT, false, 0, 0);
    gl.drawArrays(gl.TRIANGLE_STRIP, 0, 4);
    return true;
  }

  /** 调试：读取最近 A 侧 tone 输出（t 域 float，RGBA） */
  debugReadTone(): { data: Float32Array; w: number; h: number } | null {
    const gl = this.gl;
    const f = this.lastToneFbo;
    if (!gl || !f) return null;
    gl.bindFramebuffer(gl.FRAMEBUFFER, f.fbo);
    const data = new Float32Array(f.w * f.h * 4);
    gl.readPixels(0, 0, f.w, f.h, gl.RGBA, gl.FLOAT, data);
    gl.bindFramebuffer(gl.FRAMEBUFFER, null);
    return { data, w: f.w, h: f.h };
  }

  dispose(): void {
    const gl = this.gl;
    if (!gl) return;
    for (const f of this.fbos.values()) {
      gl.deleteTexture(f.tex);
      gl.deleteFramebuffer(f.fbo);
    }
    this.fbos.clear();
    if (this.srcTex) gl.deleteTexture(this.srcTex);
    if (this.curveTex) gl.deleteTexture(this.curveTex);
    if (this.noiseTex) gl.deleteTexture(this.noiseTex);
    if (this.fieldTex) gl.deleteTexture(this.fieldTex);
    if (this.maskDataTex) gl.deleteTexture(this.maskDataTex);
    this.srcTex = this.curveTex = this.noiseTex = this.fieldTex = this.maskDataTex = null;
  }
}
