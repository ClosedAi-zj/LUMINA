/**
 * 冒烟场景（仅 LUMINA_SMOKE=1 时由 App 启动）：
 * 依次驱动 素材库（分页/搜索/筛选）→ 修图台（会话/滑块/撤销/预设/快照/同步/主题切换）
 * → 导入向导（扫描）→ 存储健康 → 协议 v1.0.1 新增契约（endSession 四字段 / raw.selfTest / mock.status），
 * 每步经 console 回传主进程 stdout 作为验证证据；最后由主进程截图。
 */

import { api } from './api/engine';
import { rpc } from './api/rpc-client';
import { useDevelop } from './store/developStore';
import { buildBrushField, FIELD_CELL } from './modules/develop/webgl/maskField';

import { useLibrary } from './store/libraryStore';
import { useUi } from './store/uiStore';

const log = (msg: string) => console.log(`[smoke-scenario] ${msg}`);
const sleep = (ms: number) => new Promise((r) => setTimeout(r, ms));

/** 直接订阅协议事件等待 job 完成（不经 store，验证事件链路本身） */
function waitJob(jobId: number, timeoutMs = 15_000): Promise<Record<string, unknown>> {
  return new Promise((resolve, reject) => {
    const off = rpc.onEvent((method, params) => {
      if (method !== 'event.job.completed') return;
      const e = params as { jobId: number; result?: Record<string, unknown> };
      if (e.jobId !== jobId) return;
      off();
      resolve(e.result ?? {});
    });
    setTimeout(() => {
      off();
      reject(new Error(`job ${jobId} timeout`));
    }, timeoutMs);
  });
}

export async function runSmokeScenario(): Promise<void> {
  try {
    // ---------- ① 素材库 ----------
    const stats = await api.library.getStats();
    const tree = await api.library.getFolderTree();
    const facets = await api.library.getFacets();
    const collections = await api.library.listSmartCollections();
    // 智能收藏集 id 由引擎分配（number），无收藏集时不调用
    const smart =
      collections.length > 0
        ? await api.library.runSmartCollection(collections[0].id, { offset: 0, limit: 20 })
        : null;
    const filtered = await api.library.queryPhotos({ formats: ['RW2'], minRating: 1 }, { field: 'capturedAt', order: 'asc' }, { offset: 0, limit: 50 });
    // 分页（第二页）与搜索（FTS5 路径）
    const page2 = await api.library.queryPhotos(undefined, { field: 'capturedAt', order: 'asc' }, { offset: 200, limit: 50 });
    const searched = await api.library.search('雪山', { offset: 0, limit: 50 });
    log(
      `library: stats=${stats.totalPhotos} missing=${stats.missing} edited=${stats.edited} roots=${tree.length} cameras=${facets.cameras.length} smartColl=${collections.length} smartHits=${smart?.total ?? 0} filteredRW2=${filtered.total} page2=${page2.items.length}/${page2.total} search=${searched.total}`,
    );

    // ---------- ② 修图台 ----------
    const firstId = filtered.items[0]?.id ?? 1;
    useUi.getState().navigate('develop');
    await useDevelop.getState().openPhoto(firstId);
    await sleep(1200); // 等待 WebGL 首帧
    const d1 = useDevelop.getState();
    const canvas = document.querySelector<HTMLCanvasElement>('canvas[data-viewer-canvas]');
    log(
      `develop: photo=${firstId} session=${d1.sessionId} revision=${d1.revision} masks=${d1.params?.masks.length ?? -1} base=${d1.baseImageUrl ? 'ok' : 'null'} canvas=${canvas ? `${canvas.width}x${canvas.height}` : 'none'} xmp=${d1.xmpStatus}`,
    );
    // 渲染降级提示条是否出现在画面上（GPU 模式应为 false；LUMINA_NO_WEBGL=1 时应为 true）
    const noticeText = '软件渲染模式：参数已生效（部分高级项除外）';
    log(`render-notice: softMode=${document.body.innerText.includes(noticeText)}`);
    // 滑块 → setParams → 引擎历史
    await useDevelop.getState().setNumeric('exposure', 0.85, true);
    await useDevelop.getState().setHsl('blue', 'sat', 25, true);
    await useDevelop.getState().addMask('linear');
    await sleep(300);
    const d2 = useDevelop.getState();
    log(`develop-patch: exposure=${d2.params?.exposure} blueSat=${d2.params?.hsl.blue.sat} masks=${d2.params?.masks.length} labels=${d2.historyLabels.length}`);
    // undo / redo
    await useDevelop.getState().undo();
    await sleep(200);
    const d3 = useDevelop.getState();
    log(`develop-undo: masks=${d3.params?.masks.length} exposure=${d3.params?.exposure}`);
    await useDevelop.getState().redo();
    await sleep(200);
    // 前后对比 + 直方图 + XMP 读写往返
    const xmpRead = await api.xmp.read(firstId);
    await api.xmp.write(firstId, { ...xmpRead.params, contrast: 18 });
    const hist = await api.analysis.histogram(firstId, true);
    log(`develop-xmp: sidecar=${xmpRead.sidecarPath} namespaces=${xmpRead.foreignNamespaces.join('|') || 'none'} histR=${hist.r.length} written=ok`);
    // 预设 + 快照 + 同步
    const presets = await api.presets.list();
    await useDevelop.getState().applyPreset(presets[0].params, presets[0].name);
    await useDevelop.getState().saveSnapshot('冒烟快照');
    const sync = await api.edit.syncToSelection(firstId, [firstId + 1, firstId + 2], true);
    log(`develop-preset: presets=${presets.length} applied=${presets[0].name} snapshot=ok sync=${sync.updated}`);

    // ---------- ⑥ WP-B7 工作流键回放（主进程 sendInputEvent 注入真实按键） ----------
    // 这一段的意义：验证的是「键在 UI 里真的生效」，不是「代码里写了 handler」。
    // 每个 keys-* 行是一次握手——主进程收到该行就投对应按键，下一行再读状态。
    const photoBefore = useDevelop.getState().photoId;
    await useDevelop.getState().setNumeric('exposure', 0.42, true);
    await sleep(250);
    const copySrc = useDevelop.getState().params?.exposure ?? -999;
    log('keys-copy'); // → Ctrl+C
    await sleep(500);
    await useDevelop.getState().setNumeric('exposure', 0.91, true);
    await sleep(250);
    const prePaste = useDevelop.getState().params?.exposure ?? -999;
    log('keys-paste'); // → Ctrl+V
    await sleep(800);
    const pasted = useDevelop.getState().params?.exposure ?? -999;
    // 撤销/重做：粘贴应作为一次历史提交，Ctrl+Z 回到粘贴前，Ctrl+Shift+Z 回到粘贴后
    log('keys-undo'); // → Ctrl+Z
    await sleep(700);
    const afterUndo = useDevelop.getState().params?.exposure ?? -999;
    log('keys-redo'); // → Ctrl+Shift+Z
    await sleep(700);
    const afterRedo = useDevelop.getState().params?.exposure ?? -999;
    // 前后对比：'\' 开关（观察区浮层文案随之出现/消失）
    log('keys-compare'); // → '\'
    await sleep(500);
    const compareOn = document.body.innerText.includes('前 · Before');
    log('keys-compare'); // → '\' 关闭
    await sleep(500);
    const compareOff = !document.body.innerText.includes('前 · Before');
    // 长按空格临时看原图：按下 → 浮层出现「原图（松开空格恢复）」，松开 → 消失
    log('keys-space-down'); // → Space 按下（不松开）
    await sleep(500);
    const spaceHoldOn = document.body.innerText.includes('原图（松开空格恢复）');
    log('keys-space-up'); // → Space 松开
    await sleep(500);
    const spaceHoldOff = !document.body.innerText.includes('原图（松开空格恢复）');
    // 上/下一张循环
    log('keys-next'); // → Ctrl+'
    await sleep(1400);
    const photoNext = useDevelop.getState().photoId;
    log('keys-prev'); // → Shift+Ctrl+'
    await sleep(1400);
    const photoPrev = useDevelop.getState().photoId;
    // ? 速查浮层
    log('keys-overlay-open'); // → Shift+/
    await sleep(600);
    const overlayOpen = document.body.innerText.includes('修图台快捷键');
    log('keys-overlay-close'); // → Shift+/ 收起
    await sleep(500);
    const overlayClosed = !document.body.innerText.includes('修图台快捷键');
    log(
      `workflow: copy=${copySrc.toFixed(2)} prePaste=${prePaste.toFixed(2)} pasted=${pasted.toFixed(2)} ` +
        `paramsEqual=${Math.abs(pasted - copySrc) < 0.005} ` +
        `undo=${afterUndo.toFixed(2)} redo=${afterRedo.toFixed(2)} ` +
        `undoRedoOk=${Math.abs(afterUndo - prePaste) < 0.005 && Math.abs(afterRedo - copySrc) < 0.005} ` +
        `compare=${compareOn}/${compareOff} spaceHold=${spaceHoldOn}/${spaceHoldOff} ` +
        `next=${photoBefore}->${photoNext} prev=${photoPrev} ` +
        `overlay=${overlayOpen}/${overlayClosed}`,
    );

    // ---------- ⑦ WP-B4 HSL 8 色带：三带联动实时预览 ----------
    // 证据口径：改带参数后画布平均色必须真的动（不是"代码里接了参数"）。
    /** 把画面缩到 64x64 网格取样（drawImage 把画布压到网格，网格坐标 ≈ 画面归一化 uv） */
    const sampleGrid = (): Uint8ClampedArray => {
      const el = document.querySelector<HTMLCanvasElement>('canvas[data-viewer-canvas]');
      const empty = new Uint8ClampedArray(64 * 64 * 4);
      if (!el || el.width === 0) return empty;
      const sc = document.createElement('canvas');
      sc.width = 64;
      sc.height = 64;
      const c2 = sc.getContext('2d');
      if (!c2) return empty;
      c2.drawImage(el, 0, 0, 64, 64);
      return c2.getImageData(0, 0, 64, 64).data;
    };
    const sampleMean = (): [number, number, number] => {
      const px = sampleGrid();
      let r = 0;
      let g = 0;
      let b = 0;
      // 红带像素（hue 落在 red ±45° 且有明显彩度）单独平均：red.hue 的引擎口径位移很小
      // （hue 值 ×0.15°），整图均值会被非红像素稀释成 0.0，看不出色带是否生效
      let rr = 0;
      let rg = 0;
      let rb = 0;
      let rn = 0;
      for (let i = 0; i < px.length; i += 4) {
        r += px[i];
        g += px[i + 1];
        b += px[i + 2];
        const mx = Math.max(px[i], px[i + 1], px[i + 2]);
        const mn = Math.min(px[i], px[i + 1], px[i + 2]);
        if (mx - mn > 30 && (px[i] >= px[i + 1] || px[i] >= px[i + 2]) && px[i] > (px[i + 1] + px[i + 2]) / 2) {
          rr += px[i];
          rg += px[i + 1];
          rb += px[i + 2];
          rn++;
        }
      }
      const n = px.length / 4;
      redBandStatsRef.mean = rn > 0 ? [rr / rn, rg / rn, rb / rn] : null;
      redBandStatsRef.pixels = rn;
      return [r / n, g / n, b / n];
    };
    const redBandStatsRef: { mean: [number, number, number] | null; pixels: number } = { mean: null, pixels: 0 };
    const dist = (a: [number, number, number], b: [number, number, number]): number =>
      Math.max(Math.abs(a[0] - b[0]), Math.abs(a[1] - b[1]), Math.abs(a[2] - b[2]));
    const fmt = (m: [number, number, number]): string => m.map((v) => v.toFixed(1)).join('/');
    const fmtOpt = (m: [number, number, number] | null): string => (m ? fmt(m) : 'none');
    // 选一张有彩色的照片：引擎 mock 生成的 1.jpg 是灰阶渐变（s=0，HSL 公式按引擎口径跳过灰阶，
    // 对它测色带只会得到假阴性）。用缩略图离线筛彩度，不开编辑会话（每开一个会话要 1.5s+，会拖爆冒烟看门狗）。
    let hslPhoto: number = photoBefore ?? 1;
    {
      const { thumbUrls } = useLibrary.getState();
      const ids = (useLibrary.getState().pages[0] ?? []).map((p) => p.id).slice(0, 24);
      const chromaOfUrl = (url: string): Promise<number> =>
        new Promise((resolve) => {
          const img = new Image();
          img.onload = () => {
            const sc = document.createElement('canvas');
            sc.width = 32;
            sc.height = 32;
            const c2 = sc.getContext('2d');
            if (!c2) return resolve(-1);
            c2.drawImage(img, 0, 0, 32, 32);
            const px = c2.getImageData(0, 0, 32, 32).data;
            let sum = 0;
            for (let i = 0; i < px.length; i += 4) {
              sum += Math.max(px[i], px[i + 1], px[i + 2]) - Math.min(px[i], px[i + 1], px[i + 2]);
            }
            resolve(sum / (px.length / 4));
          };
          img.onerror = () => resolve(-1);
          img.src = url;
        });
      // 确定性选图：按 id 升序取**第一个**彩度达标的照片（不是 max），保证同一份库
      // 每次跑到同一张；达不到阈值就明确报错，不靠"随便哪张"蒙过去。
      const chromas = await Promise.all(ids.map((id) => chromaOfUrl(thumbUrls[id] ?? '').then((c) => ({ id, c }))));
      const hit = chromas.find((x) => x.c >= 20);
      hslPhoto = hit ? hit.id : (photoBefore ?? 1);
      const hitChroma = hit ? hit.c : -1;
      log(`hsl-photo: id=${hslPhoto} chroma=${hitChroma.toFixed(1)} candidates=${chromas.length} firstIds=${ids.slice(0, 5).join(',')}`);
      if (!hit) log('hsl-photo: FAILED no-colorful-photo（库里没有彩度≥20 的照片，色带测试无意义）');
      if (hslPhoto !== useDevelop.getState().photoId) {
        await useDevelop.getState().openPhoto(hslPhoto);
        await sleep(1200);
      }
    }
    // 固定基线：色调参数 + 8 个色带 + 蒙版全部归零，再取基线。
    // 否则基线会被上游状态污染：① 复制粘贴的曝光 0.42、预设等（实测 166.7 vs 196.4 摇摆）；
    // ② 这张测试照片上历史累积的蒙版（曾堆到 52 个，前 6 个参与渲染，base 从 159.2 掉到 130.7）。
    await useDevelop.getState().applyPatch(
      {
        exposure: 0,
        contrast: 0,
        highlights: 0,
        shadows: 0,
        whites: 0,
        blacks: 0,
        temperature: 0,
        tint: 0,
        vibrance: 0,
        saturation: 0,
        clarity: 0,
        dehaze: 0,
        toneCurveRGB: [{ x: 0, y: 0 }, { x: 255, y: 255 }],
        masks: [],
        hsl: {
          red: { hue: 0, sat: 0, lum: 0 },
          orange: { hue: 0, sat: 0, lum: 0 },
          yellow: { hue: 0, sat: 0, lum: 0 },
          green: { hue: 0, sat: 0, lum: 0 },
          aqua: { hue: 0, sat: 0, lum: 0 },
          blue: { hue: 0, sat: 0, lum: 0 },
          purple: { hue: 0, sat: 0, lum: 0 },
          magenta: { hue: 0, sat: 0, lum: 0 },
        },
      },
      true,
      'HSL 测试基线',
    );
    await sleep(600);
    const dSt = useDevelop.getState();
    const dCanvas = document.querySelector<HTMLCanvasElement>('canvas[data-viewer-canvas]');
    log(
      `hsl-diag: id=${dSt.photoId} canvas=${dCanvas ? `${dCanvas.width}x${dCanvas.height}` : 'none'} ` +
        `base=${(dSt.baseImageUrl ?? 'null').split('/').pop()} ready=${dSt.baseImageReady} loading=${dSt.loading} ` +
        `canvases=[${Array.from(document.querySelectorAll('canvas'))
          .map((c) => `${c.width}x${c.height}${c.hasAttribute('data-viewer-canvas') ? '*' : ''}`)
          .join(',')}]`,
    );
    const mBase = sampleMean();
    const redBase = redBandStatsRef.mean;
    await useDevelop.getState().setHsl('red', 'hue', 12, true);
    await sleep(400);
    const mRed = sampleMean();
    const redAfter = redBandStatsRef.mean;
    await useDevelop.getState().setHsl('orange', 'sat', -30, true);
    await sleep(400);
    const mOrange = sampleMean();
    await useDevelop.getState().setHsl('blue', 'lum', 25, true);
    await sleep(400);
    const mBlue = sampleMean();
    // 灵敏度附加项：red.hue=100（引擎口径位移上限 15°），用于证明红带真的作用到了红像素——
    // DoD 的 red.hue=12 按引擎公式位移 ≤1.8°，在 64x64 采样下属于设计上的不可见量
    await useDevelop.getState().setHsl('red', 'hue', 100, true);
    await sleep(400);
    const mRed100 = sampleMean();
    const red100 = redBandStatsRef.mean;
    log(
      `hsl: base=${fmt(mBase)} redHue12=${fmt(mRed)}(d=${dist(mBase, mRed).toFixed(1)}) ` +
        `orangeSat-30=${fmt(mOrange)}(d=${dist(mRed, mOrange).toFixed(1)}) ` +
        `blueLum25=${fmt(mBlue)}(d=${dist(mOrange, mBlue).toFixed(1)}) ` +
        `bands=${Object.keys(useDevelop.getState().params?.hsl ?? {}).length}`,
    );
    // 红带像素专项：redBand 均值在 red.hue=12 前后的变化（引擎口径 hue×0.15°，位移微小是设计值）
    const redD = redBase && redAfter ? dist(redBase, redAfter).toFixed(2) : 'n/a';
    const red100D = redAfter && red100 ? dist(redAfter, red100).toFixed(1) : 'n/a';
    log(
      `hsl-redband: pixels=${redBandStatsRef.pixels} before=${fmtOpt(redBase)} after=${fmtOpt(redAfter)} d=${redD} | ` +
        `redHue100 band=${fmtOpt(red100)} d=${red100D} full=${fmt(mRed100)}`,
    );
    // 还原三带，避免把测试参数写进后续流程
    await useDevelop.getState().setHsl('red', 'hue', 0, true);
    await useDevelop.getState().setHsl('orange', 'sat', 0, true);
    await useDevelop.getState().setHsl('blue', 'lum', 0, true);
    await sleep(300);

    // ---------- ⑧ WP-B5 画笔逐笔 SDF：三笔交叉 + 浮层边界一致 ----------
    useDevelop.getState().setMaskTool('brush');
    await useDevelop.getState().addMask('brush');
    const brushMaskId = useDevelop.getState().selectedMaskId;
    let brushLine = 'skipped';
    // 测试前把蒙版列表收敛成「只有这个画笔蒙版」：着色器只有 6 个槽位，
    // 历史冒烟在同一张照片上累积过 52 个蒙版（首槽之外全部不参与渲染），
    // 画笔蒙版落在槽 7+ 时距离场根本不会上传，会出现"蒙版静默失效"的假象。
    // 收敛同时也把这张测试照片的历史垃圾清掉。
    if (brushMaskId) {
      const only = (useDevelop.getState().params?.masks ?? []).filter((m) => m.id === brushMaskId);
      await useDevelop.getState().applyPatch({ masks: only }, true, '蒙版测试收敛');
      await sleep(400);
    }
    if (brushMaskId) {
      // 三条交叉笔画（水平/垂直/对角，各 4 印记连折线；flow=100 使边缘 factor 恰为 0.5）
      const paths: Array<Array<[number, number]>> = [
        [[0.2, 0.5], [0.4, 0.5], [0.6, 0.5], [0.8, 0.5]],
        [[0.5, 0.2], [0.5, 0.4], [0.5, 0.6], [0.5, 0.8]],
        [[0.25, 0.25], [0.42, 0.42], [0.58, 0.58], [0.75, 0.75]],
      ];
      for (const path of paths) {
        for (const [x, y] of path) {
          await useDevelop.getState().appendStroke(brushMaskId, { x, y, radius: 0.08, flow: 100, density: 100, erase: false }, false);
        }
      }
      await useDevelop.getState().appendStroke(brushMaskId, { x: 0.75, y: 0.75, radius: 0.08, flow: 100, density: 100, erase: false }, true);
      await sleep(600);
      // 边界一致性：在每条笔画末端的 0.75r 处采样场值，应都 ≈0.5（同一因子场 = 浮层边界 = 渲染边界）
      const strokesNow = useDevelop.getState().params?.masks.find((m) => m.id === brushMaskId)?.strokes ?? [];
      const field = buildBrushField(strokesNow);
      const sampleField = (u: number, v: number): number => {
        const px = Math.max(0, Math.min(FIELD_CELL - 1, Math.round(u * FIELD_CELL - 0.5)));
        const py = Math.max(0, Math.min(FIELD_CELL - 1, Math.round(v * FIELD_CELL - 0.5)));
        return field[py * FIELD_CELL + px];
      };
      const ends: Array<[number, number]> = [[0.8, 0.5], [0.5, 0.8], [0.75, 0.75]];
      const edges = ends.map(([x, y]) => Number(sampleField(Math.min(0.999, x + 0.06), y).toFixed(3)));
      const spread = Math.max(...edges) - Math.min(...edges);

      // 渲染参与：确定性口径。不再用"整幅均值差"（它会随图片内容摇摆，亮图能把
      // 蒙版未生效掩盖成正常：实测过 29.7 与 0.2 两个数量级）。改为只统计
      // **因子场 >0.3 的覆盖区**像素，并对覆盖区变化量设下限断言。
      const coveredIdx: number[] = [];
      const outsideIdx: number[] = [];
      for (let gy = 0; gy < 64; gy++) {
        for (let gx = 0; gx < 64; gx++) {
          const f = sampleField((gx + 0.5) / 64, (gy + 0.5) / 64);
          const at = (gy * 64 + gx) * 4;
          if (f > 0.3) coveredIdx.push(at);
          else if (f < 0.05) outsideIdx.push(at);
        }
      }
      const meanOf = (px: Uint8ClampedArray, idxs: number[]): [number, number, number] => {
        if (idxs.length === 0) return [0, 0, 0];
        let r = 0;
        let g = 0;
        let b = 0;
        for (const i of idxs) {
          r += px[i];
          g += px[i + 1];
          b += px[i + 2];
        }
        return [r / idxs.length, g / idxs.length, b / idxs.length];
      };
      const gridBefore = sampleGrid();
      await useDevelop.getState().updateMaskLocal(brushMaskId, 'exposure', 60, true);
      await sleep(600);
      const gridAfter = sampleGrid();
      const dCovered = dist(meanOf(gridBefore, coveredIdx), meanOf(gridAfter, coveredIdx));
      const dOutside = dist(meanOf(gridBefore, outsideIdx), meanOf(gridAfter, outsideIdx));
      brushLine =
        `strokes=${strokesNow.length} edges=${edges.join('/')} spread=${spread.toFixed(3)} ` +
        `coveredPx=${coveredIdx.length}/${64 * 64} dCovered=${dCovered.toFixed(1)} dOutside=${dOutside.toFixed(1)}`;
      log(`brush-sdf: ${brushLine}`);
      if (coveredIdx.length < 32) {
        log('brush-sdf: FAILED no-coverage（因子场覆盖像素过少，蒙版未成形）');
      } else if (dCovered < 5) {
        log(`brush-sdf: FAILED mask-inert d=${dCovered.toFixed(1)}（覆盖区内蒙版未生效）`);
      } else if (spread > 0.08) {
        log(`brush-sdf: FAILED edges-inconsistent spread=${spread.toFixed(3)}`);
      }
      log('brush-shot'); // 主进程截图（红浮层 + 提亮后的笔画区域肉眼可查）
      // 收尾：删掉测试蒙版，不在这张照片上继续累积蒙版（此前已堆到 52 个）
      await useDevelop.getState().removeMask(brushMaskId);
      await sleep(300);
    } else {
      log('brush-sdf: FAILED no-mask');
    }
    useDevelop.getState().setMaskTool(null);

    // ---------- ⑨ 主题切换（viewer 恒 #08090B · 红线 R5；须在观察区挂载时验证） ----------
    // 先归位到深色（localStorage 可能残留浅色），保证 dark/light 两个采样点确定
    const ensureDark = async () => {
      if (document.documentElement.getAttribute('data-theme') !== 'Dark') {
        document.querySelector<HTMLButtonElement>('button[title="切换到深色主题"]')?.click();
        await sleep(300);
      }
    };
    await ensureDark();
    const canvasDark = getComputedStyle(document.documentElement).getPropertyValue('--Lumina-Tokens-canvas').trim();
    log(`theme-pre: dataTheme=${document.documentElement.getAttribute('data-theme')}`); // 主进程据此拍深色截图（仍在修图台）
    await sleep(600); // 深色截图窗口
    const toggleBtn = document.querySelector<HTMLButtonElement>('button[title="切换到浅色主题"], button[title="切换到深色主题"]');
    toggleBtn?.click();
    await sleep(300);
    const canvasLight = getComputedStyle(document.documentElement).getPropertyValue('--Lumina-Tokens-canvas').trim();
    const viewerEl = document.querySelector('[data-viewer-surface="true"]');
    const viewerBg = viewerEl ? getComputedStyle(viewerEl as HTMLElement).backgroundColor : 'none';
    const themeNow = document.documentElement.getAttribute('data-theme');
    log(`theme: dark=${canvasDark} light=${canvasLight} viewerBg=${viewerBg} dataTheme=${themeNow} changed=${canvasDark !== canvasLight}`);
    await sleep(600); // 留出浅色主题截图窗口（主进程在收到本行日志后 capturePage）
    toggleBtn?.click(); // 还原深色
    await sleep(400);
    log(`theme-restored: dataTheme=${document.documentElement.getAttribute('data-theme')}`); // 主进程据此补拍深色截图

    await useDevelop.getState().closePhoto();
    // 协议 v1.0.1：endSession{flush:true} 四字段返回，UI 凭 sidecarMtime 熄灭指示灯
    const flushMtime = useDevelop.getState().lastFlushMtime;
    log(`develop-endSession: sidecarMtime=${flushMtime === null ? 'null' : 'confirmed'}`);

    // ---------- ③ 导入向导 ----------
    useUi.getState().navigate('ingest');
    await sleep(200);
    // 扫描源用本机真实存在的目录：固定写死外接盘符（E:/DCIM）在没有该盘时会让
    // 整条冒烟链断在环境问题上，掩盖真正的产品缺陷。
    // smoke 是本机诊断脚本：优先用应用 home 的引擎自生成库（与 --home 缺省一致）。
    // 曾写死 D:/LUMINA/mocklib，工程整理归档后失效导致冒烟误报。
    const scanRoot = 'C:/Users/zj/.lumina/mocklib';
    const { jobId: scanJobId } = await api.importApi.scanSource({ kind: 'folder', path: scanRoot });
    const scanResult = (await waitJob(scanJobId)) as { totalFiles: number; duplicates: number; unreadable: unknown[] };
    const volumes = await api.system.listVolumes();
    const check = await api.importApi.checkTarget(scanRoot, 2.0 * 1024 ** 3);
    log(
      `ingest: scanDone totalFiles=${scanResult.totalFiles} duplicates=${scanResult.duplicates} unreadable=${scanResult.unreadable.length} volumes=${volumes.length} removable=${volumes.filter((v) => v.isRemovable).length} freeCheck=${check.warning ? 'warn' : 'ok'}`,
    );

    // ---------- ④ 存储健康 ----------
    useUi.getState().navigate('health');
    await sleep(200);
    const status = await api.health.getStatus();
    const issues = await api.health.listIssues('missing', { offset: 0, limit: 100 });
    const corrupt = await api.health.listIssues('corrupt', { offset: 0, limit: 100 });
    log(`health: verified=${status.verified}/${status.total} missing=${status.missing} corrupt=${status.corrupt} md5=${status.md5.percent}% abnormal=${status.abnormal} issues=${issues.total}/${issues.items.length} corruptList=${corrupt.total}`);

    // ---------- ⑤ 地图定位写入 + Phase 2 域接线（真引擎实测；写后即清） ----------
    const mapLocs = await api.map.getLocations(6);
    const unlocated = await api.map.listUnlocated({ offset: 0, limit: 5 });
    let mapWrite = 'no-unlocated-skip';
    if (unlocated.items.length >= 2) {
      const ids = unlocated.items.slice(0, 2).map((p) => p.id);
      const w = await api.map.setPhotoLocation(ids, 28.42, 98.91, 3520, '冒烟测试点');
      mapWrite = `set updated=${w.updated} sidecarFailed=${w.sidecarFailed} warnings=${w.warnings.length}`;
      if (w.updated > 0) {
        const c = await api.map.clearPhotoLocation(ids);
        mapWrite += ` cleared=${c.updated}`;
      }
    }
    const hdrGroups = await api.hdr.detectBracketGroups();
    const seqsRaw: unknown = await api.timelapse.listSequences();
    const seqCount = Array.isArray(seqsRaw) ? seqsRaw.length : (Array.isArray((seqsRaw as { items?: unknown[] })?.items) ? (seqsRaw as { items: unknown[] }).items.length : -1);
    log(`map-write: locs=${mapLocs.length} unlocated=${unlocated.total} ${mapWrite} hdrGroups=${hdrGroups.length} seqs=${seqCount}`);

    // ---------- ⑥ 协议 v1.0.1 新增契约 ----------
    const selfTest = await api.raw.selfTest(32);
    const mockStatus = await api.mock.status();
    const ping = await api.system.ping();
    log(`protocol-v101: selfTest ok=${selfTest.ok} sampled=${selfTest.sampled} succeeded=${selfTest.succeeded} mockAsset=${mockStatus.assetFormat} lastSelfTest=${mockStatus.lastSelfTest ? 'ok' : 'none'} pong=${ping.pong}`);

    useUi.getState().navigate('library');
    log('DONE');
  } catch (err) {
    log(`FAILED: ${(err as Error).message}`);
  }
}


