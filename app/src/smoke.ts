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
import { DevelopRenderer } from './modules/develop/webgl/renderer';
import { fileUrl } from './api/rpc-client';

import { useLibrary } from './store/libraryStore';
import { useUi } from './store/uiStore';
import type { EditParams } from './api/protocol';

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
    // 快照：同名已存在就复用，避免每次冒烟都往真实库里多堆一条（此前已堆到 64 条同名）
    const existedSnaps = await api.edit.getSnapshots(firstId).catch(() => []);
    if (!existedSnaps.some((x) => x.name === '冒烟快照')) {
      await useDevelop.getState().saveSnapshot('冒烟快照');
    }
    const sync = await api.edit.syncToSelection(firstId, [firstId + 1, firstId + 2], true);
    log(`develop-preset: presets=${presets.length} applied=${presets[0].name} snapshot=ok sync=${sync.updated}`);

    // T1.7 验收截图：切专业模式（确认「部分参数尚未参与渲染」提示条已消失 + 8 组面板全接线形态）
    useDevelop.getState().setMode('pro');
    await sleep(600);
    const proHintGone = !document.body.innerText.includes('尚未参与渲染');
    const proPanelCount = document.body.innerText.split('\n').filter((l) => ['基础', '曲线', '色彩', '细节', '镜头', '几何', '效果', '校准'].includes(l.trim())).length;
    log(`t17-pro: hintGone=${proHintGone} panelCards=${proPanelCount}`);
    log('pro-shot'); // 主进程据此拍专业模式 → build/review-t17-develop-pro.png
    await sleep(900);
    useDevelop.getState().setMode('fast');
    await sleep(300);

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

    // ---------- ⑦b T1.7 逐控件像素断言：改值→像素必须动；回默认→identity ----------
    // 口径（架构文档 §11 T1.7）：每个可交互控件 改值 → 等预览刷新 → 断言像素变化；
    // 默认值必须 identity（拖回 0/默认 = 恢复基线）。全部走真实 setNumeric/setPath →
    // EditParams → paramsToGraph → RPC → GPU ops 渲染，与用户操作同链路。
    // 夹具（T1.7 工装升级）：mock 库原片均为 160×112 合成图且无高光分区，强算子在那种
    // 内容上无论实现对错都低于 minD（断言无效度）。改用程序化代表性夹具：1024×683
    // 无损 PNG（被裁切高光 / 深阴影 / 中性灰阶 / 高饱和色块 / 纹理中调），走真实 import
    // 入库（renameTemplate '{name}' + dedupe=skip，重跑幂等）。minD 与全部断言口径原样不动。
    {
      const FW = 1024;
      const FH = 683;
      const fixtureDir = 'C:/Users/zj/.lumina/t17-fixture';
      const fixtureSrcPath = `${fixtureDir}/t17-fixture.png`;
      const fixtureLibPath = 'C:/Users/zj/.lumina/mocklib/t17-fixture.png';
      // 库内已有夹具照片（内容确定性 + dedupe=skip）→ 直接复用，跳过重写/import。
      // 首跑曾出现 import job 与 xmp migration 在引擎 job 队列竞争、恰好撞上 waitJob
      // 超时的竞态（import finished 时刻 ≈ 超时时刻），复用路径从根上消除。
      const preQ = await api.library.queryPhotos({ formats: ['PNG'] }, undefined, { offset: 0, limit: 20 });
      const preExisting = preQ.items[0]?.id != null;
      let pngQ = preQ;
      if (!preExisting) {
        const fc = document.createElement('canvas');
        fc.width = FW;
        fc.height = FH;
        const fx = fc.getContext('2d');
        if (!fx) throw new Error('2d context unavailable');
        const fimg = fx.createImageData(FW, FH);
        // 确定性伪噪声（整数 hash，无随机源）：中调纹理与 colorNR/颗粒的可测素材
        const hash01 = (x: number, y: number): number => {
          let h = (x * 374761393 + y * 668265263) | 0;
          h = Math.imul(h ^ (h >> 13), 1274126177);
          return ((h ^ (h >> 16)) >>> 0) / 4294967295;
        };
        const SAT: [number, number, number][] = [
          [224, 32, 32], [32, 190, 64], [32, 96, 224], [240, 210, 32], [224, 48, 208], [40, 216, 216],
        ];
        const groundY = Math.floor(FH * 0.42);
        for (let y = 0; y < FH; ++y) {
          for (let x = 0; x < FW; ++x) {
            const i = (y * FW + x) * 4;
            let r: number;
            let g: number;
            let b: number;
            if (y < groundY) {
              // 天空：垂直渐变（中高调，含 blueSat/temperature 可测的蓝区）
              const t = y / groundY;
              r = 150 + 70 * t;
              g = 170 + 60 * t;
              b = 205 + 42 * t;
            } else {
              // 地面：中调纹理（正弦条纹 + 噪声）
              const s = Math.sin(x * 0.055) * Math.sin(y * 0.09);
              const n = (hash01(x, y) - 0.5) * 46;
              const v = 118 + s * 34 + n;
              r = v;
              g = v * 0.97 + 6;
              b = v * 0.9 + 10;
            }
            // 右上：被裁切高光块（255 裁切 + 4px 过渡沿）
            {
              const hx0 = FW - 300;
              const hy0 = 24;
              if (x >= hx0 && x < FW - 24 && y >= hy0 && y < hy0 + 200) {
                const edge = Math.min(x - hx0, FW - 25 - x, y - hy0, hy0 + 199 - y);
                const k = edge < 4 ? (0.75 + 0.25 * (edge / 4)) * 254 + 1.02 : 255;
                r = k; g = k; b = Math.min(255, k - 2);
              }
            }
            // 左下：深阴影块（近黑）
            if (x < 300 && y >= FH - 213) {
              r = 7; g = 8; b = 11;
            }
            // 中带：中性灰阶 64/96/128/160/192（5 × 80px）
            if (y >= 380 && y < 460 && x >= 64 && x < 464) {
              const v = 64 + Math.floor((x - 64) / 80) * 32;
              r = v; g = v; b = v;
            }
            // 中带：高饱和色块（6 × 70px）
            if (y >= 380 && y < 460 && x >= 560 && x < 980) {
              const c2 = SAT[Math.floor((x - 560) / 70)];
              r = c2[0]; g = c2[1]; b = c2[2];
            }
            fimg.data[i] = r;
            fimg.data[i + 1] = g;
            fimg.data[i + 2] = b;
            fimg.data[i + 3] = 255;
          }
        }
        fx.putImageData(fimg, 0, 0);
        log(`t17-fixture-write ${fixtureSrcPath}: ${fc.toDataURL('image/png')}`);
        await sleep(400); // 等主进程落盘
        const { jobId: impJob } = await api.importApi.start({
          source: { kind: 'folder', path: fixtureDir },
          target: 'C:/Users/zj/.lumina/mocklib',
          renameTemplate: '{name}',
          pairRawJpg: false,
          verifyMd5: true,
        });
        // 30s 容差：import job 可能排在 xmp migration 等长任务之后
        const impRes = (await waitJob(impJob, 30_000)) as { imported?: number; duplicated?: number; failed?: number };
        pngQ = await api.library.queryPhotos({ formats: ['PNG'] }, undefined, { offset: 0, limit: 20 });
        log(`t17-fixture: import imported=${impRes.imported ?? 0} duplicated=${impRes.duplicated ?? 0} failed=${impRes.failed ?? 0}`);
      } else {
        log(`t17-fixture: reuse photoId=${preQ.items[0].id}（库内已有夹具，跳过重写/import）`);
      }
      const fixtureId = pngQ.items[0]?.id ?? null;
      if (fixtureId === null) {
        log('t17-fixture: FAILED import-missing（重查 queryPhotos 无 PNG 记录）');
      } else {
        log(
          `t17-fixture: photoId=${fixtureId} size=${FW}x${FH} content=clipped-highlight/deep-shadow/neutral-gray/saturated-blocks/texture-midtone`,
        );
        await useDevelop.getState().openPhoto(fixtureId);
        await sleep(1500);
        // 固定基线：色调参数 + 8 色带 + 蒙版全部归零，再取基线（custom+5500K 由 store 契约归一化为 0）
        await useDevelop.getState().applyPatch(
          {
            whiteBalance: 'custom',
            exposure: 0, contrast: 0, highlights: 0, shadows: 0, whites: 0, blacks: 0,
            temperature: 5500, tint: 0, vibrance: 0, saturation: 0, clarity: 0, texture: 0, dehaze: 0,
            toneCurveRGB: [{ x: 0, y: 0 }, { x: 255, y: 255 }],
            masks: [],
            hsl: {
              red: { hue: 0, sat: 0, lum: 0 }, orange: { hue: 0, sat: 0, lum: 0 },
              yellow: { hue: 0, sat: 0, lum: 0 }, green: { hue: 0, sat: 0, lum: 0 },
              aqua: { hue: 0, sat: 0, lum: 0 }, blue: { hue: 0, sat: 0, lum: 0 },
              purple: { hue: 0, sat: 0, lum: 0 }, magenta: { hue: 0, sat: 0, lum: 0 },
            },
            colorGrade: { shadowHue: 0, shadowSat: 0, midtoneHue: 0, midtoneSat: 0, highlightHue: 0, highlightSat: 0, blend: 50, balance: 0 },
            calibration: { shadowTint: 0, redHue: 0, redSat: 0, greenHue: 0, greenSat: 0, blueHue: 0, blueSat: 0 },
            detail: { sharpness: 0, sharpenRadius: 1, sharpenDetail: 25, sharpenMasking: 0, luminanceSmoothing: 0, colorNR: 0, colorNRDetail: 25 },
            optics: { lensProfileEnable: false, manualDistortion: 0, chromaticAberration: 0, vignetteAmount: 0, vignetteMidpoint: 50, vignetteFeather: 50, grainAmount: 0, grainFrequency: 1, grainSize: 1 },
          },
          true,
          'T1.7 逐控件基线',
        );
        await sleep(700);
        const baseParamsSnap = structuredClone(useDevelop.getState().params!);
        // 引擎侧交叉验证：同一夹具（无损 PNG 原片）+ 同一参数快照，走 develop.render
        // （导出同代码路径）→ 64×64 网格均值，与 GPU 预览 d 同口径对照
        const engineMean = async (params: EditParams): Promise<[number, number, number]> => {
          const er = await api.develop.render({ inputPath: fixtureLibPath, params, longEdge: 0 });
          const im = await new Promise<HTMLImageElement>((res, rej) => {
            const q = new Image();
            q.onload = () => res(q);
            q.onerror = () => rej(new Error(`engine render load failed: ${er.path}`));
            q.src = fileUrl(er.path);
          });
          const ec2 = document.createElement('canvas');
          ec2.width = 64;
          ec2.height = 64;
          const ectx = ec2.getContext('2d', { willReadFrequently: true });
          if (!ectx) throw new Error('2d context unavailable');
          ectx.drawImage(im, 0, 0, 64, 64);
          const epx = ectx.getImageData(0, 0, 64, 64).data;
          let er2 = 0;
          let eg = 0;
          let eb = 0;
          for (let i = 0; i < epx.length; i += 4) {
            er2 += epx[i];
            eg += epx[i + 1];
            eb += epx[i + 2];
          }
          const en = epx.length / 4;
          return [er2 / en, eg / en, eb / en];
        };
        const num = (k: keyof EditParams, v: number) => () => useDevelop.getState().setNumeric(k, v, true);
        const path = (p: string, v: number) => () => useDevelop.getState().setPath(p, v, true);
        // minD = 该控件「变了」的最小像素阈值（max 通道差）；id 阈值统一 2.0
        const controls: { name: string; apply: () => Promise<void>; reset: () => Promise<void>; minD: number }[] = [
          { name: 'exposure+0.6', apply: num('exposure', 0.6), reset: num('exposure', 0), minD: 3 },
          { name: 'contrast+35', apply: num('contrast', 35), reset: num('contrast', 0), minD: 2 },
          { name: 'highlights-45', apply: num('highlights', -45), reset: num('highlights', 0), minD: 1.5 },
          { name: 'shadows+45', apply: num('shadows', 45), reset: num('shadows', 0), minD: 1.5 },
          { name: 'whites+35', apply: num('whites', 35), reset: num('whites', 0), minD: 1.5 },
          { name: 'blacks-35', apply: num('blacks', -35), reset: num('blacks', 0), minD: 1.5 },
          { name: 'temperature+2000K', apply: num('temperature', 7500), reset: num('temperature', 5500), minD: 2 },
          { name: 'tint-25', apply: num('tint', -25), reset: num('tint', 0), minD: 1.5 },
          { name: 'vibrance+45', apply: num('vibrance', 45), reset: num('vibrance', 0), minD: 2 },
          { name: 'saturation+35', apply: num('saturation', 35), reset: num('saturation', 0), minD: 2 },
          { name: 'clarity+45', apply: num('clarity', 45), reset: num('clarity', 0), minD: 1.5 },
          { name: 'texture+45', apply: num('texture', 45), reset: num('texture', 0), minD: 1 },
          { name: 'dehaze+35', apply: num('dehaze', 35), reset: num('dehaze', 0), minD: 1.5 },
          { name: 'detail.sharpness+80', apply: path('detail.sharpness', 80), reset: path('detail.sharpness', 0), minD: 1 },
          { name: 'detail.colorNR+50', apply: path('detail.colorNR', 50), reset: path('detail.colorNR', 0), minD: 1.5 },
          { name: 'optics.vignette-60', apply: path('optics.vignetteAmount', -60), reset: path('optics.vignetteAmount', 0), minD: 3 },
          { name: 'optics.grain+50', apply: path('optics.grainAmount', 50), reset: path('optics.grainAmount', 0), minD: 1.5 },
          { name: 'colorGrade.midtoneSat+50', apply: path('colorGrade.midtoneSat', 50), reset: path('colorGrade.midtoneSat', 0), minD: 1.5 },
          { name: 'calibration.blueSat+40', apply: path('calibration.blueSat', 40), reset: path('calibration.blueSat', 0), minD: 1.5 },
        ];
        const t17: string[] = [];
        let t17Failed = 0;
        for (const c of controls) {
          const base = sampleMean();
          await c.apply();
          await sleep(650);
          const after = sampleMean();
          const d = dist(base, after);
          // 引擎侧同参数对拍（apply 后的 store params 即 GPU 实际用的合并结果）
          const appliedSnap = structuredClone(useDevelop.getState().params!);
          let dEng = -1;
          try {
            dEng = dist(await engineMean(baseParamsSnap), await engineMean(appliedSnap));
          } catch (eeng) {
            log(`t17-engine-x: ${c.name} FAILED ${(eeng as Error).message}`);
          }
          await c.reset();
          await sleep(500);
          const back = sampleMean();
          const dId = dist(back, base);
          const ok = d >= c.minD && dId < 2.0;
          if (!ok) t17Failed++;
          t17.push(`${c.name} d=${d.toFixed(1)} eng=${dEng >= 0 ? dEng.toFixed(1) : 'n/a'} id=${dId.toFixed(1)}${ok ? '' : ' FAILED'}`);
        }
        log(`t17-controls: ${t17Failed === 0 ? 'ALL-OK' : `${t17Failed}-FAILED`} n=${controls.length} | ${t17.join(' · ')}`);
      }
    }

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

    // ---------- ②b M-3：浮层打开状态截图（移除确认对话框 = 玻璃 M2 材质验证载体） ----------
    useUi.getState().navigate('library');
    await sleep(400);
    const firstLib = (useLibrary.getState().pages[0] ?? [])[0];
    if (firstLib) {
      useLibrary.getState().select(firstLib.id, 'set');
      await sleep(200);
      window.dispatchEvent(new KeyboardEvent('keydown', { key: 'Delete' }));
      await sleep(300);
      log('overlay-shot'); // 主进程据此拍「浮层打开状态」→ build/review-overlay.png
      await sleep(900);
      // 永久加固：关闭按钮限定在浮层弹窗（.glass-m2）内查找——WindowControls 的窗口关闭
      // 按钮语义同名，裸查全局第一个会被命中导致整窗退出、冒烟断链
      document.querySelector<HTMLButtonElement>('.glass-m2 button[aria-label="关闭"]')?.click();
      await sleep(200);
    }

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

    // ---------- ⑦ T1.4 全 93 叶 parity（CPU oracle vs GPU，门槛 mean<0.5/max≤3 LSB） ----------
    try {
      log('parity93-step: import-begin');
      const { Parity93Runner } = await import('./modules/develop/webgl/ops/parity93');
      log('parity93-step: import-ok');
      const pCanvas = document.createElement('canvas');
      const runner = new Parity93Runner();
      if (runner.init(pCanvas)) {
        log('parity93-step: init-ok run-begin');
        const report = runner.run();
        log('parity93-step: run-done');
        for (const c of report.cases) {
          log(
            `parity93-case: leaf=${c.leaf} mean=${c.mean.toFixed(4)} max=${c.max.toFixed(0)} px=${c.pixels} pass=${c.pass}`,
          );
        }
        // 报表落盘：单行 JSON 由主进程捕获写 build/parity-report.json
        log(`parity93-report: ${JSON.stringify(report)}`);
        log(`parity93: allPass=${report.allPass} worstMean=${report.worstMean.toFixed(4)} worstMax=${report.worstMax.toFixed(0)} gpu=${report.gpuAvailable}`);
      } else {
        log('parity93: gpu-init-failed allPass=false');
      }
    } catch (perr) {
      log(`parity93: error ${(perr as Error).message} allPass=false`);
    }

    // ---------- ⑦c T1.5 导出=预览像素断言（销毁三套实现后的验收，进 CI） ----------
    // ⑦c 口径保持：回到照片 1 的 80×56 底图（t17 夹具照片会让 parity 画布变 1024×683，
    // 属另一口径；不因夹具升级改变 ⑦c 对照基准）。
    // 口径（写进报表 note）：同一输入位图（preview base 经渲染进程解码一次导出的**无损 PNG**，
    // 两端共享同一份文件，消除浏览器/引擎对有损 JPEG 的双解码差）+
    // 同一参数集 P（覆盖非几何非蒙版全部叶）。预览侧 = GPU 管线（DevelopRenderer/
    // PipelineGL 多 pass，imageToCodes sRGB→linear→log16 进域）；导出侧 = develop.render
    // （与 export 完全同一代码路径 displayToPlanBuf → runEditPlan → planBufToDisplay）。
    // 逐像素 8bit RGB LSB 比对，门槛同 §10 parity：mean<0.5 / max≤3。超门槛即冒烟失败。
    {
      // ⑦c 跑在 t17 之后：显式回到照片 1，保证 parity 画布口径仍是 80×56
      await useDevelop.getState().openPhoto(firstId);
      await sleep(1200);
      const stOpen = useDevelop.getState();
      if (!stOpen.baseImageUrl) {
        await stOpen.openPhoto(firstId);
        await sleep(1200);
      }
      const stNow = useDevelop.getState();
      const base = stNow.baseImageUrl;
      if (!base) {
        log('export-parity: FAILED no-base-image');
      } else {
        const P: EditParams = {
          processVersion: '15.4',
          whiteBalance: 'custom',
          temperature: 24,
          tint: -14,
          exposure: 0.55,
          contrast: 28,
          highlights: -38,
          shadows: 32,
          whites: 18,
          blacks: -22,
          clarity: 26,
          texture: 18,
          dehaze: 16,
          vibrance: 32,
          saturation: 12,
          toneCurveRGB: [{ x: 0, y: 8 }, { x: 64, y: 60 }, { x: 192, y: 200 }, { x: 255, y: 248 }],
          parametric: { highlights: 10, lights: -8, darks: 6, shadows: -12, shadowSplit: 25, lightSplit: 50, midSplit: 75 },
          hsl: {
            red: { hue: 10, sat: 14, lum: -6 },
            orange: { hue: -8, sat: 20, lum: 0 },
            yellow: { hue: 0, sat: -18, lum: 6 },
            green: { hue: 12, sat: 10, lum: 0 },
            aqua: { hue: 0, sat: 0, lum: 0 },
            blue: { hue: -14, sat: 24, lum: 10 },
            purple: { hue: 0, sat: 0, lum: 0 },
            magenta: { hue: 6, sat: 0, lum: 0 },
          },
          colorGrade: { shadowHue: 210, shadowSat: 18, midtoneHue: 40, midtoneSat: 10, highlightHue: 55, highlightSat: 14, blend: 50, balance: 0 },
          calibration: { shadowTint: 8, redHue: -6, redSat: 10, greenHue: 4, greenSat: -8, blueHue: -4, blueSat: 12 },
          detail: { sharpness: 45, sharpenRadius: 1.2, sharpenDetail: 30, sharpenMasking: 10, luminanceSmoothing: 0, colorNR: 0, colorNRDetail: 25 },
          optics: { lensProfileEnable: false, manualDistortion: 0, chromaticAberration: 0, vignetteAmount: -35, vignetteMidpoint: 50, vignetteFeather: 50, grainAmount: 28, grainFrequency: 50, grainSize: 1.2 },
          geometry: { hasCrop: false, perspective: { vertical: 0, horizontal: 0, rotate: 0, scale: 1, aspect: 0, x: 0, y: 0 } },
          masks: [],
        };
        const loadImg = (src: string): Promise<HTMLImageElement> =>
          new Promise((res, rej) => {
            const im = new Image();
            im.onload = () => res(im);
            im.onerror = () => rej(new Error(`img load failed: ${src}`));
            im.src = src;
          });
        const grab = (src: HTMLCanvasElement | HTMLImageElement, w: number, h: number): Uint8ClampedArray => {
          const c = document.createElement('canvas');
          c.width = w;
          c.height = h;
          const ctx = c.getContext('2d', { willReadFrequently: true });
          if (!ctx) throw new Error('2d context unavailable');
          ctx.drawImage(src, 0, 0, w, h);
          return ctx.getImageData(0, 0, w, h).data;
        };
        try {
          // 工装修复（⑦c 双解码差）：原口径 JPEG 底图被两端各自解码（浏览器 vs 引擎 stb），
          // 有损 4:2:0 进域前字节即不同（identity 非零差即证据），sharpen/HSL 再放大边缘/色度差。
          // 修复：渲染进程解码一次 → 导出无损 PNG（主进程按行内路径落盘）→ 两端统一喂同一份 PNG。
          const img = await loadImg(fileUrl(base));
          const losslessPath = base.replace(/\.(jpe?g)$/i, '.parity.png');
          const lc = document.createElement('canvas');
          lc.width = img.naturalWidth;
          lc.height = img.naturalHeight;
          const lctx = lc.getContext('2d', { willReadFrequently: true });
          if (!lctx) throw new Error('2d context unavailable');
          lctx.drawImage(img, 0, 0);
          log(`export-parity-base ${losslessPath}: ${lc.toDataURL('image/png')}`);
          await sleep(400); // 等主进程落盘
          const imgL = await loadImg(fileUrl(losslessPath));
          // 预览侧：GPU 管线（与 ViewerCanvas 同一类、同进域函数）
          const glCanvas = document.createElement('canvas');
          const dr = new DevelopRenderer();
          if (!dr.init(glCanvas) || !dr.setImage(imgL)) throw new Error('gpu pipeline init failed');
          // 两级断言：先 identity 参数（域往返/编码对照，定位进域/出域/toneMap 系统差），
          // 再全叶参数集 P（叶公式对照）。
          const P0: EditParams = {
            ...P,
            whiteBalance: 'asShot',
            temperature: 0,
            tint: 0,
            exposure: 0,
            contrast: 0,
            highlights: 0,
            shadows: 0,
            whites: 0,
            blacks: 0,
            clarity: 0,
            texture: 0,
            dehaze: 0,
            vibrance: 0,
            saturation: 0,
            toneCurveRGB: [{ x: 0, y: 0 }, { x: 255, y: 255 }],
            parametric: { highlights: 0, lights: 0, darks: 0, shadows: 0, shadowSplit: 25, lightSplit: 50, midSplit: 75 },
            hsl: {
              red: { hue: 0, sat: 0, lum: 0 }, orange: { hue: 0, sat: 0, lum: 0 },
              yellow: { hue: 0, sat: 0, lum: 0 }, green: { hue: 0, sat: 0, lum: 0 },
              aqua: { hue: 0, sat: 0, lum: 0 }, blue: { hue: 0, sat: 0, lum: 0 },
              purple: { hue: 0, sat: 0, lum: 0 }, magenta: { hue: 0, sat: 0, lum: 0 },
            },
            colorGrade: { shadowHue: 0, shadowSat: 0, midtoneHue: 0, midtoneSat: 0, highlightHue: 0, highlightSat: 0, blend: 50, balance: 0 },
            calibration: { shadowTint: 0, redHue: 0, redSat: 0, greenHue: 0, greenSat: 0, blueHue: 0, blueSat: 0 },
            detail: { sharpness: 0, sharpenRadius: 1, sharpenDetail: 25, sharpenMasking: 0, luminanceSmoothing: 0, colorNR: 0, colorNRDetail: 25 },
            optics: { lensProfileEnable: false, manualDistortion: 0, chromaticAberration: 0, vignetteAmount: 0, vignetteMidpoint: 50, vignetteFeather: 50, grainAmount: 0, grainFrequency: 50, grainSize: 1 },
          };
          const comparePair = async (tag: string, params: EditParams): Promise<void> => {
            if (!dr.render(glCanvas, { params })) throw new Error('gpu render failed');
            const er = await api.develop.render({ inputPath: losslessPath, params, longEdge: 0 });
            const eimg = await loadImg(fileUrl(er.path));
            if (eimg.naturalWidth !== glCanvas.width || eimg.naturalHeight !== glCanvas.height) {
              log(`export-parity: FAILED size-mismatch [${tag}] gpu=${glCanvas.width}x${glCanvas.height} engine=${eimg.naturalWidth}x${eimg.naturalHeight}`);
              return;
            }
            const w = glCanvas.width;
            const h = glCanvas.height;
            const a = grab(glCanvas, w, h);
            const b = grab(eimg, w, h);
            const inp = grab(imgL, w, h);
            if (tag === 'identity') {
              const pts0: [number, number][] = [[0, 0], [Math.floor(w / 4), Math.floor(h / 2)], [Math.floor(w / 2), Math.floor(h / 2)], [w - 1, h - 1]];
              log(`export-parity-dump: [input] ` + pts0.map(([x, y]) => `(${x},${y})=${inp[(y * w + x) * 4]},${inp[(y * w + x) * 4 + 1]},${inp[(y * w + x) * 4 + 2]}`).join(' | '));
            }
            let sum = 0;
            let maxv = 0;
            for (let i = 0; i < w * h * 4; i += 4) {
              for (let c = 0; c < 3; ++c) {
                const d = Math.abs(a[i + c] - b[i + c]);
                sum += d;
                if (d > maxv) maxv = d;
              }
            }
            const mean = sum / (w * h * 3);
            const pass = mean < 0.5 && maxv <= 3;
            const px = (d: Uint8ClampedArray, x: number, y: number): string =>
              `${d[(y * w + x) * 4]},${d[(y * w + x) * 4 + 1]},${d[(y * w + x) * 4 + 2]}`;
            const pts: [number, number][] = [[0, 0], [Math.floor(w / 4), Math.floor(h / 2)], [Math.floor(w / 2), Math.floor(h / 2)], [w - 1, h - 1]];
            log(
              `export-parity-dump: [${tag}] ` +
                pts.map(([x, y]) => `(${x},${y}) gpu=${px(a, x, y)} eng=${px(b, x, y)}`).join(' | '),
            );
            log(`export-parity: [${tag}] size=${w}x${h} mean=${mean.toFixed(4)} max=${maxv} pass=${pass}`);
            if (tag === 'full-leaf-P') {
              log(
                `export-parity: size=${w}x${h} mean=${mean.toFixed(4)} max=${maxv} pass=${pass} ` +
                  `(口径: 无损 PNG 底图（同一次解码导出，消除浏览器/引擎双解码差）+参数集P；预览=GPU PipelineGL；导出=develop.render→Plan；门槛 mean<0.5/max≤3 LSB)`,
              );
              log(
                `export-parity-report: ${JSON.stringify({
                  scenario: 'T1.5 export=preview',
                  leafScope: '非几何非蒙版全部叶（basic14+WB2+curve+parametric+hsl8+colorGrade+calibration+detail+optics）',
                  size: { w, h },
                  mean,
                  max: maxv,
                  pass,
                  threshold: { meanLsb: 0.5, maxLsb: 3 },
                  identity: { note: 'identity 对照见 export-parity [identity] 行' },
                  note: '底图经渲染进程解码一次导出无损 PNG（build 同目录 .parity.png），两端共享同一份无损输入，消除浏览器/引擎对有损 JPEG 的双解码差；预览=GPU PipelineGL 多 pass（imageToCodes 进域）；导出=develop.render（displayToPlanBuf→runEditPlan→planBufToDisplay，与 export 同一代码路径）；逐像素 8bit RGB LSB 比对',
                })}`,
              );
            }
          };
          await comparePair('identity', P0);
          await comparePair('full-leaf-P', P);
        } catch (eperr) {
          log(`export-parity: FAILED ${(eperr as Error).message}`);
        }
      }
    }

    useUi.getState().navigate('library');
    log('DONE');
  } catch (err) {
    log(`FAILED: ${(err as Error).message}`);
  }
}


