/**
 * M4-04 冒烟探针（Electron 直接运行，不启 UI）：
 *   electron build/app/electron/print-smoke.cjs <imagePath> <outputPath> [A4|A3|Letter] [landscape]
 * 与产品 IPC 走**同一份** renderImageToPdf 实现，最后把结果 JSON 打到 stdout。
 */
import { app } from 'electron';
import { renderImageToPdf } from './print-pdf';

async function main() {
  const [imagePath, outputPath, pageSize, orientation] = process.argv.slice(2);
  if (!imagePath || !outputPath) {
    console.log(JSON.stringify({ ok: false, error: 'usage: print-smoke <image> <out.pdf> [A4|A3|Letter] [landscape]' }));
    app.exit(2);
    return;
  }
  try {
    const r = await renderImageToPdf({
      imagePath,
      outputPath,
      pageSize: (pageSize as 'A4' | 'A3' | 'Letter') ?? 'A4',
      landscape: orientation === 'landscape',
      marginMm: 10,
      dpi: 300,
    });
    console.log(JSON.stringify({ ok: true, ...r }));
    app.exit(0);
  } catch (e) {
    console.log(JSON.stringify({ ok: false, error: e instanceof Error ? e.message : String(e) }));
    app.exit(1);
  }
}

app.whenReady().then(main);
