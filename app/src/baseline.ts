/**
 * WP-C1 视觉回归基线：13 帧设计稿 × 2 主题的逐帧截图走查。
 *
 * 运行：`LUMINA_BASELINE=1 npx electron .`（主进程传 ?baseline=1，App 再 import 本模块）。
 * 产物：`docs/ui-1.1/baseline/{dark|light}-<帧号>-<页面>.png`（由主进程 capturePage 落盘）。
 *
 * 与设计稿的对应关系见 `docs/ui-1.1/对齐差异清单.md`（00–12 帧）：
 *  00 设计令牌（信息帧，无独立页面 → 不截图）
 *  11 独有功能（信息帧，无对应页面 → 不截图）
 *  09 为浅色主题规范帧，取景与 02 同页（修图台快速），用途是双主题对照
 *  附录 A1 导出 / A2 存储健康：不在 13 帧内，但有独立页面，一并入基线
 */

import { useUi } from './store/uiStore';
import type { AppView } from './store/uiStore';
import { useDevelop } from './store/developStore';
import { useLibrary } from './store/libraryStore';

const log = (msg: string): void => console.log(`[baseline] ${msg}`);
const sleep = (ms: number): Promise<void> => new Promise((r) => setTimeout(r, ms));

type DevMode = 'fast' | 'pro' | null;

interface FrameSpec {
  id: string;
  name: string;
  view: AppView;
  devMode?: DevMode;
}

const FRAMES: FrameSpec[] = [
  { id: '01', name: 'library', view: 'library' },
  { id: '02', name: 'develop-fast', view: 'develop', devMode: 'fast' },
  { id: '03', name: 'portfolio', view: 'portfolio' },
  { id: '04', name: 'gallery', view: 'gallery' },
  { id: '05', name: 'culling', view: 'culling' },
  { id: '06', name: 'ingest', view: 'ingest' },
  { id: '07', name: 'map', view: 'map' },
  { id: '08', name: 'timelapse', view: 'timelapse' },
  { id: '09', name: 'develop-theme', view: 'develop', devMode: 'fast' },
  { id: '10', name: 'develop-pro', view: 'develop', devMode: 'pro' },
  { id: '12', name: 'landscape', view: 'landscape' },
  { id: 'A1', name: 'export', view: 'export' },
  { id: 'A2', name: 'health', view: 'health' },
];

const themeNow = (): string => document.documentElement.getAttribute('data-theme') ?? 'none';

async function setTheme(want: 'Dark' | 'Light'): Promise<void> {
  if (themeNow() === want) return;
  const btn = document.querySelector<HTMLButtonElement>(
    want === 'Light' ? 'button[title="切换到浅色主题"]' : 'button[title="切换到深色主题"]',
  );
  btn?.click();
  await sleep(400);
}

async function ensurePhoto(): Promise<void> {
  if (useDevelop.getState().photoId !== null) return;
  const ids = (useLibrary.getState().pages[0] ?? []).map((p) => p.id);
  const id = ids[0];
  if (id === undefined) return;
  await useDevelop.getState().openPhoto(id);
  await sleep(1200);
}

export async function runBaselineScenario(): Promise<void> {
  try {
    // 等启动：引擎握手 + 库加载 + 首帧合成（首帧未 paint 时 capturePage 会返回 0x0 空帧）
    await sleep(6000);
    for (let i = 0; i < 40; i++) {
      const loaded = (useLibrary.getState().pages[0] ?? []).length > 0;
      if (loaded) break;
      await sleep(500);
    }
    log(`start: photos=${useLibrary.getState().total} firstPage=${(useLibrary.getState().pages[0] ?? []).length}`);

    for (const f of FRAMES) {
      useUi.getState().navigate(f.view);
      await sleep(700);
      if (f.devMode) {
        await ensurePhoto();
        useDevelop.getState().setMode(f.devMode);
        await sleep(900);
        // 渲染探针：直接读画布中心 8x8 的像素均值。修图台画布黑屏（首轮基线 dark-02/09/10
        // 画布纯黑 + 直方图空）必须用数据定位——是渲染没跑，还是截图链路说谎。
        const el = document.querySelector<HTMLCanvasElement>('canvas[data-viewer-canvas]');
        let px = 'none';
        if (el && el.width > 0) {
          const sc = document.createElement('canvas');
          sc.width = 8;
          sc.height = 8;
          const c2 = sc.getContext('2d');
          if (c2) {
            c2.drawImage(el, 0, 0, 8, 8);
            const d = c2.getImageData(0, 0, 8, 8).data;
            let r = 0;
            let g = 0;
            let b = 0;
            for (let i = 0; i < d.length; i += 4) {
              r += d[i];
              g += d[i + 1];
              b += d[i + 2];
            }
            const n = d.length / 4;
            px = `${Math.round(r / n)},${Math.round(g / n)},${Math.round(b / n)}`;
          }
        }
        const st = useDevelop.getState();
        const cards = Array.from(document.querySelectorAll('aside section')).map(
          (s) => `${(s.querySelector('button')?.textContent ?? '').trim().slice(0, 10)}:${Math.round((s as HTMLElement).getBoundingClientRect().height)}`,
        );
        log(
          `probe ${f.id}-${f.devMode}: canvas=${el ? `${el.width}x${el.height}` : 'none'} px=${px} ` +
            `base=${(st.baseImageUrl ?? 'null').split('/').pop()} ready=${st.baseImageReady} loading=${st.loading} photoId=${st.photoId}`,
        );
        log(`cards ${f.id}: ${cards.join(' | ')}`);
      }
      await setTheme('Dark');
      await sleep(700);
      log(`shot ${f.id}-${f.name} dark`);
      await sleep(1600); // 截图窗口：主进程 capturePage 延迟 + 空帧/陈旧帧重试需要时间
      await setTheme('Light');
      await sleep(700);
      log(`shot ${f.id}-${f.name} light`);
      await sleep(1600);
      await setTheme('Dark');
      await sleep(400);
    }
    log('DONE');
  } catch (err) {
    log(`FAILED ${(err as Error).message}`);
    log('DONE');
  }
}
