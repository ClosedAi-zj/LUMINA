/**
 * 水印所见即所得预览（WP-B8）。
 *
 * 与引擎 exporter.cpp 的文字水印渲染**同参复刻**，两条路径分别对应引擎的两个分支：
 *  - 含非 ASCII（中文等）：引擎走 GDI 系统字体光栅化（export/text_raster.cpp），
 *    字号 = max(12, 图高×5/88)、锚点用实测文本宽/字体盒高；预览同步走 canvas
 *    系统字体（fillText + fontBoundingBox 度量），观感即导出结果；
 *  - 纯 ASCII：引擎内置 3x5 点阵（字高 ≈5% 图高、margin = 图高/50、scale 相对
 *    「图宽 12%」基准、5 个锚点、blendPixel 白色叠加），预览逐位复刻点阵。
 *
 * 仍存在的引擎边界（协议缺口清单已列给主理人）：
 *  - 点阵仅收录 ASCII（大小写折叠），纯 ASCII 文本中的未收录字符（如 @ #）仍渲染为 '?'；
 *  - 图片水印走 RGB 合成，PNG 透明通道不参与（会是一整块矩形）。
 */

/** 与 engine/src/export/exporter.cpp glyphBits 完全一致的 3x5 点阵 */
export const GLYPHS: Record<string, string> = {
  A: '010101111101101', B: '110101110101110', C: '011100100100011', D: '110101101101110',
  E: '111100110100111', F: '111100110100100', G: '011100101101011', H: '101101111101101',
  I: '111010010010111', J: '001001001101010', K: '101101110101101', L: '100100100100111',
  M: '101111111101101', N: '110111101101101', O: '010101101101010', P: '110101110100100',
  Q: '010101101111011', R: '110101110101101', S: '011100010001110', T: '111010010010010',
  U: '101101101101011', V: '101101101101010', W: '101101111111101', X: '101101010101101',
  Y: '101101010010010', Z: '111001010100111',
  '0': '111101101101111', '1': '010110010010111', '2': '111001111100111', '3': '111001111001111',
  '4': '101101111001001', '5': '111100111001111', '6': '111100111101111', '7': '111001001001001',
  '8': '111101111101111', '9': '111101111001111',
  ' ': '000000000000000', '-': '000000111000000', '.': '000000000000010', '_': '000000000000111',
  ':': '000010000010000', '/': '001001010100100', '(': '001010010010001', ')': '100010010010100',
  '+': '000010111010000', ',': '000000000010100', "'": '010010000000000', '?': '111001010000010',
};

export interface WatermarkPreviewSpec {
  enabled: boolean;
  position: string;
  scale: number;
  opacity: number;
  text?: string;
}

/** 引擎锚点算法（anchorOrigin）的镜像 */
function anchorOrigin(position: string, width: number, height: number, markW: number, markH: number, margin: number): [number, number] {
  let x = margin;
  let y = margin;
  if (position === 'bottom-right') {
    x = width - markW - margin;
    y = height - markH - margin;
  } else if (position === 'bottom-left') {
    x = margin;
    y = height - markH - margin;
  } else if (position === 'top-right') {
    x = width - markW - margin;
    y = margin;
  } else if (position === 'top-left') {
    x = margin;
    y = margin;
  } else {
    x = Math.floor((width - markW) / 2);
    y = Math.floor((height - markH) / 2);
  }
  return [Math.max(0, Math.min(width - 1, x)), Math.max(0, Math.min(height - 1, y))];
}

/** 非 ASCII 检测（与引擎 textraster::hasNonAscii 同口径：码位 > 0x7F） */
export function hasNonAscii(text: string): boolean {
  for (const ch of text) {
    if ((ch.codePointAt(0) ?? 0) > 0x7f) return true;
  }
  return false;
}

/** 纯 ASCII 文本中引擎点阵未收录的字符数（导出时显示为 ?；非 ASCII 已走系统字体不受此限） */
export function countUnsupportedChars(text: string): number {
  let n = 0;
  for (const ch of text) {
    const upper = ch >= 'a' && ch <= 'z' ? ch.toUpperCase() : ch;
    if (!(upper in GLYPHS)) n++;
  }
  return n;
}

/** 文字水印渲染（ctx 上先画好底图再调本函数）；返回点阵未收录字符数 */
export function renderTextWatermark(ctx: CanvasRenderingContext2D, width: number, height: number, wm: WatermarkPreviewSpec): number {
  const text = wm.text ?? '';
  const alpha = Math.max(0, Math.min(1, wm.opacity / 100));
  const margin = Math.max(4, Math.floor(height / 50));
  ctx.fillStyle = `rgba(255, 255, 255, ${alpha})`;

  // ---- 非 ASCII（中文等）：引擎系统字体路径的同参复刻 ----
  // 引擎：rasterizeText(max(12, height*5/88)) → anchorOrigin(实测宽, 字体盒高) → 白字叠加。
  // 预览：fillText + fontBoundingBox 度量镜像同一几何；混合文本整体走本路径（与引擎一致）。
  if (hasNonAscii(text)) {
    const fontSize = Math.max(12, Math.floor((height * 5) / 88));
    ctx.font = `${fontSize}px "Microsoft YaHei", "PingFang SC", "Noto Sans SC", system-ui, sans-serif`;
    ctx.textAlign = 'left';
    ctx.textBaseline = 'alphabetic';
    const m = ctx.measureText(text);
    const ascent = m.fontBoundingBoxAscent ?? fontSize * 0.88;
    const descent = m.fontBoundingBoxDescent ?? fontSize * 0.12;
    const markW = Math.max(1, Math.ceil(m.width));
    const markH = Math.max(1, Math.ceil(ascent + descent));
    const [x0, y0] = anchorOrigin(wm.position, width, height, markW, markH, margin);
    // 基线落在锚点 + ascent 处，使字体盒顶与 GDI DT_CALCRECT 盒顶对齐
    ctx.fillText(text, x0, y0 + ascent);
    return 0;
  }

  // ---- 纯 ASCII：引擎内置 3x5 点阵路径 ----
  const block = Math.max(1, Math.floor(height / 100));
  const glyphW = 3 * block;
  const glyphH = 5 * block;
  const advance = 4 * block;
  const textW = text.length * advance - (advance - glyphW);
  const [x0, y0] = anchorOrigin(wm.position, width, height, textW > 0 ? textW : glyphW, glyphH, margin);

  let unsupported = 0;
  let penX = x0;
  for (const raw of text) {
    const upper = raw >= 'a' && raw <= 'z' ? raw.toUpperCase() : raw;
    let bits = GLYPHS[upper];
    if (!bits) {
      unsupported++;
      bits = GLYPHS['?'];
    }
    for (let gy = 0; gy < 5; gy++) {
      for (let gx = 0; gx < 3; gx++) {
        if (bits[gy * 3 + gx] !== '1') continue;
        ctx.fillRect(penX + gx * block, y0 + gy * block, block, block);
      }
    }
    penX += advance;
  }
  return unsupported;
}

/**
 * 把导出参数换算到预览画布：引擎 scale 的基准 = 成品图宽 12%；
 * 预览画布宽 W 对应"成品长边"，水印字块按同比例缩放，所见即导出比例。
 */
export function previewScalePx(width: number, scale: number): number {
  return Math.max(1, Math.floor(width * 0.12 * scale));
}
