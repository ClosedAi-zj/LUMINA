/**
 * M4-04 打印排版 → PDF（主进程侧实现）。
 *
 * 抽成独立模块的原因：IPC handler 与冒烟探针要跑**同一份实现**，
 * 否则测试和产品各写一套排版逻辑，验证结论无意义。
 */
import { BrowserWindow, app } from 'electron';
import path from 'node:path';
import fs from 'node:fs';

export interface PrintPdfRequest {
  imagePath: string;
  pageSize?: 'A4' | 'A3' | 'Letter';
  landscape?: boolean;
  marginMm?: number;
  dpi?: number;
  outputPath?: string;
}

export interface PrintPdfResult {
  path: string;
  bytes: number;
  pageSize: string;
  landscape: boolean;
  dpi: number;
  marginMm: number;
}

function toFileUrl(p: string): string {
  const norm = p.replace(/\\/g, '/');
  return `file:///${encodeURI(norm).replace(/^%2F/, '/')}`;
}

export async function renderImageToPdf(req: PrintPdfRequest): Promise<PrintPdfResult> {
  const pageSize = req?.pageSize ?? 'A4';
  const landscape = req?.landscape ?? false;
  const marginMm = Math.max(0, Math.min(40, req?.marginMm ?? 10));
  const dpi = Math.max(72, Math.min(1200, req?.dpi ?? 300));
  if (!req?.imagePath || !fs.existsSync(req.imagePath)) {
    throw new Error(`打印源图不存在: ${req?.imagePath ?? '(空)'}`);
  }

  const html = `<!doctype html><html><head><meta charset="utf-8"/>
<style>
  @page { size: ${pageSize} ${landscape ? 'landscape' : 'portrait'}; margin: ${marginMm}mm; }
  html,body { margin:0; padding:0; background:#fff; }
  .page { width:100%; height:100%; display:flex; align-items:center; justify-content:center; }
  img { max-width:100%; max-height:100%; object-fit:contain; }
</style></head><body><div class="page"><img src="${toFileUrl(req.imagePath)}"/></div></body></html>`;

  const win = new BrowserWindow({ show: false, webPreferences: { offscreen: true } });
  // 注意：不能用 data: URL 承载页面 —— 其 origin 无法加载 file:// 图像（Chromium 直接拒绝，
  // 页面空白）。落一个临时 HTML 用 file:// 打开，与图像同源。
  const tmpHtml = path.join(app.getPath('temp'), `lumina-print-${process.pid}-${Date.now()}.html`);
  try {
    fs.writeFileSync(tmpHtml, html, 'utf8');
    await win.loadURL(toFileUrl(tmpHtml));
    // 等图片解码完成，否则 PDF 里可能是空白页
    const imgOk = await win.webContents.executeJavaScript(
      `new Promise((res) => { const i = document.images[0]; if (!i) return res(false); if (i.complete) return res(i.naturalWidth > 0); i.onload = () => res(true); i.onerror = () => res(false); })`,
    );
    if (!imgOk) throw new Error('打印源图未能解码（页面为空白）');
    const pdf = await win.webContents.printToPDF({
      pageSize,
      landscape,
      printBackground: true,
      margins: {
        marginType: 'custom',
        top: marginMm / 25.4,
        bottom: marginMm / 25.4,
        left: marginMm / 25.4,
        right: marginMm / 25.4,
      },
    });
    const outPath =
      req.outputPath ?? path.join(app.getPath('downloads'), `LUMINA-print-${Date.now()}.pdf`);
    fs.mkdirSync(path.dirname(outPath), { recursive: true });
    fs.writeFileSync(outPath, pdf);
    return { path: outPath, bytes: pdf.length, pageSize, landscape, dpi, marginMm };
  } finally {
    win.destroy();
    try {
      fs.unlinkSync(tmpHtml);
    } catch {
      /* 临时文件清理失败不影响结果 */
    }
  }
}
