/**
 * MockEngine —— 引擎不可用时的内置替身（PRD §8.9：替身必须真实可跑、状态机真实、字段与协议逐字一致）。
 * 实现ipc-protocol.md 全部 95 方法 / 7 事件 / 24 错误码的合理假数据行为。
 * 长任务走 job + event.job.progress/completed；缩略图落盘真实文件（SVG）并传路径。
 */

import type { EngineNotification } from '../engine-bridge';
import {
  generateMockLibrary,
  runQuery,
  buildStats,
  buildTimeline,
  buildFacets,
  buildFolderTree,
  type MockFolder,
  type MockLibraryData,
} from './data';
import { MockAssetStore } from './thumbs';
import type {
  EditParams,
  Filter,
  HealthIssueItem,
  JobInfo,
  PhotoDetail,
  RampKeyframe,
} from '../../src/api/protocol';

interface JsonRpcResponse {
  jsonrpc: '2.0';
  id: number;
  result?: unknown;
  error?: { code: number; message: string; data?: unknown };
}

class RpcShapeError extends Error {
  constructor(
    readonly code: number,
    message: string,
    readonly data?: unknown,
  ) {
    super(message);
  }
}

const now = (): number => Date.now();
const DAY = 86_400_000;

/** 未编辑参数基线 */
function defaultParams(): EditParams {
  return {
    processVersion: '15.4',
    whiteBalance: 'asShot',
    temperature: 5500,
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
    toneCurveRGB: [
      { x: 0, y: 0 },
      { x: 255, y: 255 },
    ],
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
    colorGrade: { shadowHue: 0, shadowSat: 0, midtoneHue: 0, midtoneSat: 0, highlightHue: 0, highlightSat: 0, blend: 50, balance: 0 },
    calibration: { shadowTint: 0, redHue: 0, redSat: 0, greenHue: 0, greenSat: 0, blueHue: 0, blueSat: 0 },
    detail: { sharpness: 25, sharpenRadius: 1, sharpenDetail: 25, sharpenMasking: 0, luminanceSmoothing: 0, colorNR: 0, colorNRDetail: 50 },
    optics: {
      lensProfileEnable: false,
      chromaticAberration: 0,
      manualDistortion: 0,
      vignetteAmount: 0,
      vignetteMidpoint: 50,
      vignetteFeather: 50,
      grainAmount: 0,
      grainFrequency: 50,
      grainSize: 25,
    },
    geometry: { hasCrop: false, perspective: { vertical: 0, horizontal: 0, rotate: 0, scale: 100, aspect: 100, x: 0, y: 0 } },
    masks: [],
  };
}

/** 深合并 patch（数组整体替换，对象递归合并） */
function deepMerge<T>(base: T, patch: Partial<T> | Record<string, unknown> | undefined): T {
  if (patch === undefined || patch === null) return base;
  if (Array.isArray(base) || Array.isArray(patch)) return patch as T;
  if (typeof base === 'object' && base !== null && typeof patch === 'object') {
    const out: Record<string, unknown> = { ...(base as Record<string, unknown>) };
    for (const [k, v] of Object.entries(patch as Record<string, unknown>)) {
      const bv = out[k];
      if (bv !== null && typeof bv === 'object' && !Array.isArray(bv) && v !== null && typeof v === 'object' && !Array.isArray(v)) {
        out[k] = deepMerge(bv, v as Record<string, unknown>);
      } else {
        out[k] = v;
      }
    }
    return out as T;
  }
  return (patch as T) ?? base;
}

interface MockSession {
  sessionId: number;
  photoId: number;
  params: EditParams;
  revision: number;
  undoStack: EditParams[];
  redoStack: EditParams[];
  flushTimer: ReturnType<typeof setTimeout> | null;
}

interface MockJob extends JobInfo {
  timer: ReturnType<typeof setInterval> | null;
  startedAt: number;
  durationMs: number;
  onDone?: () => void;
}

export class MockEngine {
  readonly data: MockLibraryData;
  private assetStore: MockAssetStore;
  private deleted = new Set<number>();
  private sessions = new Map<number, MockSession>();
  private sessionSeq = 1;
  private jobs = new Map<number, MockJob>();
  private jobSeq = 1;
  private conflictIds = new Set<number>();
  private corruptIssues = new Map<number, HealthIssueItem>();
  private issueSeq = 1;
  private snapshots = new Map<number, { snapshotId: number; name: string; createdAt: number; params: EditParams }[]>();
  private vcopies = new Map<number, { vcopyId: number; name: string; createdAt: number }[]>();
  private vcopySeq = 1;
  private snapshotSeq = 1;
  private albumItems = new Map<number, number[]>();
  private albumSeq = 10;
  private publishedGalleries: { albumId: number; token: string; previewUrl: string; expiresAt: number; password: string; favorites: number }[] = [];
  private exportPresets: { id: number; name: string; spec: Record<string, unknown> }[] = [
    { id: 1, name: 'Web 质量 JPG', spec: { format: 'jpg', longEdge: 2048, colorSpace: 'sRGB', quality: 88, psdKeepLayers: false, watermark: { enabled: true, position: 'bottom-right', scale: 0.88, opacity: 0.8, text: '© LUMINA' }, namingTemplate: 'YYYY-MM-DD_地点_序号', outputDir: 'D:/LUMINA/build/runtime/exports' } },
    { id: 2, name: '印刷质量 TIFF', spec: { format: 'tiff', longEdge: 4096, colorSpace: 'AdobeRGB', quality: 95, psdKeepLayers: false, watermark: { enabled: false, position: 'bottom-right', scale: 0.88, opacity: 0.8 }, namingTemplate: 'YYYY-MM-DD_地点_序号', outputDir: 'D:/LUMINA/build/runtime/exports' } },
    { id: 3, name: 'PSD · 带图层', spec: { format: 'psd', longEdge: 4096, colorSpace: 'AdobeRGB', quality: 100, psdKeepLayers: true, watermark: { enabled: false, position: 'bottom-right', scale: 0.88, opacity: 0.8 }, namingTemplate: 'YYYY-MM-DD_地点_序号', outputDir: 'D:/LUMINA/build/runtime/exports' } },
  ];
  private exportPresetSeq = 4;
  private settings: Record<string, unknown> = {
    theme: 'dark',
    homeDir: '',
    libraryRoots: ['F:/2025', 'F:/照片', 'X:/2026', 'Y:/2026/2026', 'F:/ljf.it'],
    backupTarget: 'N:/NAS-Photos/Lumina',
    thumbCacheQuotaMB: 5120,
  };
  private recentSources = ['E:/DCIM/100_PANA', 'D:/拍摄素材/2026-04', 'F:/ljf.it/外包', 'X:/2026/客户片', 'C:/Users/zj/Desktop/样片'];
  private rampBySeq = new Map<number, RampKeyframe[]>();
  private deflickerBySeq = new Map<number, { analyzed: boolean; brightnessGainPct?: number }>();
  private locationFavorites = new Set<string>();
  private extraFolders = new Map<number, MockFolder>();
  private folderSeq = 5000;
  private notificationListeners = new Set<(n: EngineNotification) => void>();
  private handlers = new Map<string, (params: Record<string, unknown>) => unknown | Promise<unknown>>();

  constructor(homeDir: string) {
    this.data = generateMockLibrary();
    this.assetStore = new MockAssetStore(homeDir);
    this.settings.homeDir = homeDir.replace(/\\/g, '/');
    this.initRamps();
    this.registerHandlers();
  }

  onNotification(cb: (n: EngineNotification) => void): () => void {
    this.notificationListeners.add(cb);
    return () => this.notificationListeners.delete(cb);
  }

  private notify(method: string, params: unknown): void {
    const n: EngineNotification = { jsonrpc: '2.0', method, params };
    for (const cb of this.notificationListeners) {
      try {
        cb(n);
      } catch {
        /* 监听器异常不传播 */
      }
    }
  }

  private initRamps(): void {
    this.rampBySeq.set(1, [
      { frameNo: 1, ev: 1.0, temperatureK: 5200 },
      { frameNo: 120, ev: 0.5, temperatureK: 5100 },
      { frameNo: 240, ev: -1.0, temperatureK: 4800 },
      { frameNo: 360, ev: -4.5, temperatureK: 4100 },
      { frameNo: 480, ev: -5.0, temperatureK: 3200 },
    ]);
    this.rampBySeq.set(2, [
      { frameNo: 1, ev: -1.0, temperatureK: 6500 },
      { frameNo: 150, ev: 0.0, temperatureK: 5600 },
      { frameNo: 300, ev: 1.5, temperatureK: 5000 },
    ]);
    this.rampBySeq.set(3, [
      { frameNo: 1, ev: 0.0, temperatureK: 5600 },
      { frameNo: 75, ev: -0.5, temperatureK: 5400 },
      { frameNo: 150, ev: -1.2, temperatureK: 5200 },
    ]);
  }

  // ============ 调度入口 ============

  async dispatch(req: { id: number; method: string; params: unknown }): Promise<JsonRpcResponse> {
    const handler = this.handlers.get(req.method);
    if (!handler) {
      // 协议 §2：未识别方法返回 -32601，不得崩溃
      return { jsonrpc: '2.0', id: req.id, error: { code: -32601, message: `方法未注册: ${req.method}`, data: null } };
    }
    try {
      const result = await handler((req.params as Record<string, unknown>) ?? {});
      return { jsonrpc: '2.0', id: req.id, result };
    } catch (err) {
      if (err instanceof RpcShapeError) {
        return { jsonrpc: '2.0', id: req.id, error: { code: err.code, message: err.message, data: err.data ?? null } };
      }
      return { jsonrpc: '2.0', id: req.id, error: { code: -32603, message: err instanceof Error ? err.message : String(err), data: null } };
    }
  }

  // ============ 帮助函数 ============

  private photoById(id: number): PhotoDetail {
    const p = this.data.photos.find((x) => x.id === id);
    if (!p) throw new RpcShapeError(1001, `文件不存在: photoId=${id}`);
    return p;
  }

  private livePhotos(): PhotoDetail[] {
    return this.data.photos.filter((p) => !this.deleted.has(p.id));
  }

  private folderById(id: number): MockFolder | undefined {
    return this.data.folders.find((f) => f.id === id) ?? this.extraFolders.get(id);
  }

  private folderOf(p: PhotoDetail): MockFolder | undefined {
    return this.folderById(p.folderId);
  }

  /** 模拟 job：定时发 progress，结束发 completed 并记录 */
  private startJob(kind: string, durationMs: number, buildResult: () => Record<string, unknown>, opts?: { fileResultTick?: (i: number, total: number) => void }): number {    const jobId = this.jobSeq++;
    const total = Math.max(1, Math.round(durationMs / 250));
    let i = 0;
    const job: MockJob = {
      jobId,
      kind,
      state: 'running',
      progress: 0,
      detail: '准备中…',
      etaMs: durationMs,
      timer: null,
      startedAt: now(),
      durationMs,
    };
    job.timer = setInterval(() => {
      i++;
      const progress = Math.min(100, Math.round((i / total) * 100));
      job.progress = progress;
      job.detail = `${kind} · ${progress}%`;
      job.etaMs = Math.max(0, durationMs - i * 250);
      this.notify('event.job.progress', { jobId, kind, state: 'running', progress, detail: job.detail, etaMs: job.etaMs });
      opts?.fileResultTick?.(i, total);
      if (progress >= 100) {
        if (job.timer) clearInterval(job.timer);
        job.timer = null;
        job.state = 'done';
        const result = buildResult();
        this.notify('event.job.completed', { jobId, kind, state: 'done', result });
      }
    }, 250);
    this.jobs.set(jobId, job);
    return jobId;
  }

  /**
   * 协议约定（§7.2）：所有"转 job"的方法（import.* / health.scanFull|restoreFromBackup|verifyPhotos /
   * export.createJob / timelapse.* / hdr.composite / denoise.apply / mock.generate）
   * 必须返回 `{ jobId }` 形状——立即返回 jobId 不占 10s 超时。startJob 返回裸 id，
   * 统一经此包装为协议形状，避免各 handler 漏包装导致 UI 解构出 undefined。
   */
  private startJobRpc(
    kind: string,
    durationMs: number,
    buildResult: () => Record<string, unknown>,
    opts?: { fileResultTick?: (i: number, total: number) => void },
  ): { jobId: number } {
    return { jobId: this.startJob(kind, durationMs, buildResult, opts) };
  }

  private cancelJob(jobId: number): void {
    const job = this.jobs.get(jobId);
    if (!job) throw new RpcShapeError(3001, '任务不存在');
    if (job.timer) clearInterval(job.timer);
    job.timer = null;
    job.state = 'cancelled';
    this.notify('event.job.completed', { jobId, kind: job.kind, state: 'cancelled' });
  }

  private getParams(photoId: number): EditParams {
    return this.data.paramsById.get(photoId) ?? defaultParams();
  }

  private ensureFolderForImport(): MockFolder {
    const ymd = new Date().toISOString().slice(0, 10);
    for (const f of this.extraFolders.values()) {
      if (f.name === `导入_${ymd}`) return f;
    }
    const folder: MockFolder = {
      id: this.folderSeq++,
      rootId: 1,
      path: `F:/2025/导入_${ymd}`,
      name: `导入_${ymd}`,
      locationName: '松赞林寺',
      missing: 0,
      photoCount: 0,
    };
    this.extraFolders.set(folder.id, folder);
    return folder;
  }

  private appendImportedPhoto(idx: number, folder: MockFolder): PhotoDetail {
    const id = this.data.photos.length + 1;
    const capturedAt = now() - idx * 60_000;
    const fileName = `${folder.name}_${String(idx).padStart(4, '0')}.RW2`;
    const photo: PhotoDetail = {
      id,
      fileName,
      filePath: `${folder.path}/${fileName}`,
      format: 'RW2',
      width: 6720,
      height: 4480,
      capturedAt,
      importedAt: now(),
      camera: 'DC-S5M2',
      lens: 'LUMIX S 24-60mm',
      iso: 200,
      aperture: 'f/8',
      shutter: '1/250s',
      focalLength: '35mm',
      gpsLat: null,
      gpsLng: null,
      rating: 0,
      pick: 'none',
      colorLabel: null,
      hasEdits: false,
      hasGps: false,
      isMissing: false,
      isVideo: false,
      pairKind: idx % 9 === 0 ? 'raw+jpg' : 'single',
      thumbState: 'pending',
      thumbPath: null,
      seqId: null,
      md5: null,
      fileSize: 28_000_000,
      sidecarPath: `${folder.path}/${fileName}.xmp`,
      sidecarMtime: now(),
      whiteBalance: 'asShot',
      folderId: folder.id,
      rootId: folder.rootId,
      keywords: [],
      description: '',
      seqInfo: null,
      health: 'ok',
      backup: { backedUp: true, backupPath: `N:/NAS-Photos/Lumina/${fileName}`, verifiedAt: now() },
    };
    this.data.photos.push(photo);
    folder.photoCount++;
    return photo;
  }

  // ============ 方法注册 ============

  private registerHandlers(): void {
    const H = (name: string, fn: (params: Record<string, unknown>) => unknown | Promise<unknown>) => this.handlers.set(name, fn.bind(this));

    // ---- 5.0 系统与握手 ----
    H('system.initialize', () => ({
      protocolVersion: '1.0',
      engineVersion: 'mock-1.1.1',
      capabilities: { libraw: false, fts5: true, codecs: ['h264', 'h265'], mock: true },
      homeDir: this.settings.homeDir,
      dbPath: `${this.settings.homeDir}/lumina.db`,
    }));
    H('system.shutdown', () => {
      this.notify('event.engine.log', { level: 'info', module: 'core', message: 'mock engine shutdown' });
      return {};
    });
    H('system.ping', () => ({ pong: true, uptimeMs: process.uptime() * 1000 }));
    H('system.getSettings', () => ({ settings: this.settings }));
    H('system.setSettings', (p) => {
      this.settings = deepMerge(this.settings, p.patch as Record<string, unknown>);
      return { settings: this.settings };
    });
    H('system.listVolumes', () => [
      { path: 'E:/', label: 'SD 卡 · DC-S5M2', totalBytes: 64.2 * 1024 ** 3, freeBytes: 21.6 * 1024 ** 3, isRemovable: true },
      { path: 'D:/', label: '系统盘（NVMe）', totalBytes: 1024 * 1024 ** 3, freeBytes: 412 * 1024 ** 3, isRemovable: false },
      { path: 'N:/', label: 'NAS-Photos', totalBytes: 8 * 1024 ** 3, freeBytes: 3.2 * 1024 ** 3, isRemovable: false },
    ]);

    // ---- 5.1 图库查询 ----
    H('library.getStats', () => buildStats(this.data.photos, this.deleted));
    H('library.queryPhotos', (p) =>
      runQuery(this.data.photos, this.deleted, p.filter as Filter | undefined, p.sort as never, (p.page as { offset: number; limit: number }) ?? { offset: 0, limit: 200 }, this.allFolders()),
    );
    H('library.search', (p) => {
      const text = String(p.text ?? '').trim().toLowerCase();
      const page = (p.page as { offset: number; limit: number }) ?? { offset: 0, limit: 200 };
      if (!text) return { total: 0, items: [] };
      // FTS5 替身：文件名 + 地点 + 日期字符串匹配
      const hits = this.livePhotos().filter((ph) => {
        const folder = this.folderOf(ph);
        const ym = new Date(ph.capturedAt).toISOString().slice(0, 7);
        return ph.fileName.toLowerCase().includes(text) || (folder?.locationName ?? '').includes(text) || ym.includes(text) || folder?.name.toLowerCase().includes(text);
      });
      return { total: hits.length, items: hits.slice(page.offset, page.offset + page.limit).map((ph) => this.summary(ph)) };
    });
    H('library.getFacets', (p) => buildFacets(this.data.photos, this.deleted, p.filter as Filter | undefined, this.allFolders()));
    H('library.getTimeline', () => buildTimeline(this.data.photos, this.deleted));
    H('library.getFolderTree', () => buildFolderTree(this.data.photos, this.deleted, this.allFolders()));
    H('library.getPhotoDetail', (p) => this.detail(this.photoById(Number(p.id))));
    H('library.updatePhotos', (p) => {
      const ids = (p.ids as number[]) ?? [];
      const patch = (p.patch ?? {}) as { rating?: number; pick?: string; colorLabel?: string | null; keywords?: string[]; description?: string };
      let updated = 0;
      for (const id of ids) {
        const ph = this.photoById(id);
        if (patch.rating !== undefined) ph.rating = patch.rating as PhotoDetail['rating'];
        if (patch.pick !== undefined) ph.pick = patch.pick as PhotoDetail['pick'];
        if (patch.colorLabel !== undefined) ph.colorLabel = patch.colorLabel;
        if (patch.keywords !== undefined) ph.keywords = patch.keywords;
        if (patch.description !== undefined) ph.description = patch.description;
        updated++;
      }
      return { updated };
    });
    H('library.removePhotos', (p) => {
      const ids = (p.ids as number[]) ?? [];
      for (const id of ids) this.deleted.add(id);
      return { removed: ids.length };
    });
    H('library.restorePhotos', (p) => {
      const ids = (p.ids as number[]) ?? [];
      for (const id of ids) this.deleted.delete(id);
      return { restored: ids.length };
    });
    H('library.listSmartCollections', () => [
      { id: 1, name: '快速收藏', filter: { minRating: 2 } },
      { id: 2, name: '上个月', filter: { dateFrom: now() - 30 * DAY, dateTo: now() } },
      { id: 3, name: '五星球', filter: { minRating: 5 } },
      { id: 4, name: 'RAW 优先', filter: { formats: ['RW2'] } },
      { id: 5, name: '已编辑', filter: { edited: true } },
      { id: 6, name: 'GPS 已定位', filter: { located: true } },
      { id: 7, name: '缺失待处理', filter: { missing: true } },
    ]);
    H('library.runSmartCollection', (p) => {
      const table: Record<number, Filter> = {
        1: { minRating: 2 },
        2: { dateFrom: now() - 30 * DAY, dateTo: now() },
        3: { minRating: 5 },
        4: { formats: ['RW2'] },
        5: { edited: true },
        6: { located: true },
        7: { missing: true },
      };
      const filter = table[Number(p.id)] ?? {};
      return runQuery(this.data.photos, this.deleted, filter, undefined, (p.page as { offset: number; limit: number }) ?? { offset: 0, limit: 200 }, this.allFolders());
    });

    // ---- 5.2 相册 ----
    H('albums.list', () => {
      if (this.albumItems.size === 0) {
        const live = this.livePhotos();
        this.albumItems.set(1, live.slice(100, 142).map((p) => p.id));
        this.albumItems.set(2, live.filter((p) => p.seqId === 1).slice(0, 18).map((p) => p.id));
        this.albumItems.set(3, live.slice(5000, 5074).map((p) => p.id));
      }
      const defs = [
        { id: 1, name: '川西 · 秋色', kind: 'collection', coverPhotoId: (this.albumItems.get(1) ?? [])[0] ?? 1 },
        { id: 2, name: '松赞林寺 · 延时', kind: 'portfolio', coverPhotoId: (this.albumItems.get(2) ?? [])[0] ?? 1 },
        { id: 3, name: '建筑几何', kind: 'portfolio', coverPhotoId: (this.albumItems.get(3) ?? [])[0] ?? 1 },
      ];
      return defs.map((d) => ({
        ...d,
        itemCount: (this.albumItems.get(d.id) ?? []).length,
        published: this.publishedGalleries.some((g) => g.albumId === d.id),
      }));
    });
    H('albums.create', (p) => {
      const id = ++this.albumSeq;
      this.albumItems.set(id, []);
      return { id };
    });
    H('albums.addPhotos', (p) => {
      const albumId = Number(p.albumId);
      const ids = (p.ids as number[]) ?? [];
      const cur = this.albumItems.get(albumId) ?? [];
      const merged = [...new Set([...cur, ...ids])];
      this.albumItems.set(albumId, merged);
      return { added: merged.length - cur.length };
    });
    H('albums.removePhotos', (p) => {
      const albumId = Number(p.albumId);
      const ids = new Set((p.ids as number[]) ?? []);
      const cur = this.albumItems.get(albumId) ?? [];
      const next = cur.filter((id) => !ids.has(id));
      this.albumItems.set(albumId, next);
      return { removed: cur.length - next.length };
    });
    H('albums.reorder', (p) => {
      const albumId = Number(p.albumId);
      const orderedIds = (p.orderedIds as number[]) ?? [];
      this.albumItems.set(albumId, orderedIds);
      return {};
    });

    // ---- 5.3 缩略图与预览 ----
    H('thumb.ensureBatch', (p) => {
      const ids = (p.ids as number[]) ?? [];
      const size = (p.size as 320 | 640) ?? 320;
      const items: { id: number; state: string; path?: string }[] = [];
      const readyIds: number[] = [];
      for (const id of ids) {
        try {
          const ph = this.photoById(id);
          const path = this.assetStore.thumb(ph.id, size, ph.capturedAt, ph.format === 'JPG' || ph.format === 'PNG');
          ph.thumbState = 'ready';
          ph.thumbPath = path;
          items.push({ id, state: 'ready', path });
          readyIds.push(id);
        } catch {
          items.push({ id, state: 'failed' });
        }
      }
      // 就绪后逐张发 event.thumb.ready（允许晚于响应到达）
      readyIds.forEach((id, i) => {
        const ph = this.photoById(id);
        setTimeout(() => this.notify('event.thumb.ready', { photoId: id, size, path: ph.thumbPath }), 5 + i * 2);
      });
      return { items };
    });
    H('thumb.getStatus', (p) => {
      const ids = (p.ids as number[]) ?? [];
      const size = (p.size as 320 | 640) ?? 320;
      return {
        items: ids.map((id) => {
          const ph = this.photoById(id);
          return ph.thumbState === 'ready' ? { id, state: 'ready', path: ph.thumbPath } : { id, state: ph.thumbState };
        }),
      };
    });
    H('preview.getBase', (p) => {
      const ph = this.photoById(Number(p.id));
      if (ph.isMissing) throw new RpcShapeError(1001, '原片离线，无法解码');
      const path = this.assetStore.preview(ph.id, ph.capturedAt, ph.format === 'JPG' || ph.format === 'PNG');
      return { path, width: 1920, height: 1280, source: 'native' };
    });
    H('cache.clear', (p) => ({ freedBytes: p.scope === 'thumbs' ? 512 * 1024 * 1024 : 1024 * 1024 * 1024 }));

    // ---- 5.4 XMP ----
    H('xmp.read', (p) => {
      const ph = this.photoById(Number(p.id));
      const params = this.getParams(ph.id);
      const warnings: string[] = [];
      const foreignNamespaces: string[] = [];
      if (ph.seqId !== null) {
        foreignNamespaces.push('lrt:');
        warnings.push('检测到 LRTimelapse lrt: 命名空间，非本次编辑字段将逐字节保留');
      }
      if (ph.format === 'DNG') warnings.push('ProcessVersion 由 DNG 内嵌参数升级而来');
      return {
        params,
        schemaVersion: 1,
        processVersion: '15.4',
        warnings,
        sidecarPath: ph.sidecarPath ?? `${ph.filePath}.xmp`,
        sidecarMtime: ph.sidecarMtime ?? now(),
        foreignNamespaces,
      };
    });
    H('xmp.write', (p) => {
      const ph = this.photoById(Number(p.id));
      if (this.conflictIds.has(ph.id)) {
        const knownMtime = ph.sidecarMtime ?? now();
        throw new RpcShapeError(1005, 'sidecar 已被外部修改', { sidecarMtime: knownMtime + 60_000, knownMtime });
      }
      const params = p.params as EditParams;
      this.data.paramsById.set(ph.id, params);
      ph.hasEdits = true;
      ph.sidecarPath = ph.sidecarPath ?? `${ph.filePath}.xmp`;
      ph.sidecarMtime = now();
      this.conflictIds.delete(ph.id);
      this.notify('event.engine.log', { level: 'info', module: 'xmp', message: `write ok photo=${ph.id} mtime=${ph.sidecarMtime}` });
      return { sidecarMtime: ph.sidecarMtime };
    });
    H('xmp.checkExternal', (p) => {
      const ph = this.photoById(Number(p.id));
      return { changed: this.conflictIds.has(ph.id), sidecarMtime: ph.sidecarMtime ?? now() };
    });
    H('xmp.listNamespaces', (p) => {
      const ph = this.photoById(Number(p.id));
      const list = [
        { prefix: 'crs', uri: 'http://ns.adobe.com/camera-raw-settings/1.0/', fieldCount: 42 },
        { prefix: 'lumina', uri: 'http://lumina.photo/ns/1.0/', fieldCount: ph.hasEdits ? 8 : 2 },
        { prefix: 'exif', uri: 'http://ns.adobe.com/exif/1.0/', fieldCount: ph.hasGps ? 6 : 3 },
      ];
      if (ph.seqId !== null) list.push({ prefix: 'lrt', uri: 'http://www.lrtimelapse.com/ns/1.0/', fieldCount: 5 });
      return list;
    });
    H('xmp.migrationStatus', () => ({ current: 1, distribution: { '0': 58, '1': 18134 }, migrated: 58 }));

    // ---- 5.5 编辑会话 ----
    H('edit.beginSession', (p) => {
      const photoId = Number(p.id);
      this.photoById(photoId);
      const sessionId = this.sessionSeq++;
      const session: MockSession = {
        sessionId,
        photoId,
        params: structuredClone(this.getParams(photoId)),
        revision: 1,
        undoStack: [],
        redoStack: [],
        flushTimer: null,
      };
      this.sessions.set(sessionId, session);
      return { sessionId, params: session.params, revision: session.revision, historyLength: 0 };
    });
    H('edit.setParams', (p) => {
      const session = this.sessions.get(Number(p.sessionId));
      if (!session) throw new RpcShapeError(3001, '会话不存在');
      const commitHistory = p.commitHistory !== false;
      if (commitHistory) {
        session.undoStack.push(structuredClone(session.params));
        if (session.undoStack.length > 200) session.undoStack.shift();
        session.redoStack = [];
      }
      session.params = deepMerge(session.params, p.patch as Partial<EditParams>);
      session.revision++;
      // 引擎侧 500ms 防抖落盘 sidecar（协议 §5.5）
      if (session.flushTimer) clearTimeout(session.flushTimer);
      session.flushTimer = setTimeout(() => {
        const ph = this.data.photos.find((x) => x.id === session.photoId);
        if (ph && !this.conflictIds.has(ph.id)) {
          this.data.paramsById.set(ph.id, structuredClone(session.params));
          ph.hasEdits = true;
          ph.sidecarMtime = now();
          this.notify('event.engine.log', { level: 'info', module: 'xmp', message: `debounce-write ok photo=${ph.id}` });
        }
      }, 500);
      return { params: session.params, revision: session.revision };
    });
    H('edit.undo', (p) => {
      const session = this.sessions.get(Number(p.sessionId));
      if (!session) throw new RpcShapeError(3001, '会话不存在');
      const prev = session.undoStack.pop();
      if (!prev) throw new RpcShapeError(3004, '无可撤销步骤', { reason: 'history-empty' });
      session.redoStack.push(structuredClone(session.params));
      session.params = prev;
      session.revision++;
      return { params: session.params, revision: session.revision, historyIndex: session.undoStack.length };
    });
    H('edit.redo', (p) => {
      const session = this.sessions.get(Number(p.sessionId));
      if (!session) throw new RpcShapeError(3001, '会话不存在');
      const next = session.redoStack.pop();
      if (!next) throw new RpcShapeError(3004, '无可重做步骤', { reason: 'history-end' });
      session.undoStack.push(structuredClone(session.params));
      session.params = next;
      session.revision++;
      return { params: session.params, revision: session.revision, historyIndex: session.undoStack.length };
    });
    H('edit.endSession', (p) => {
      const session = this.sessions.get(Number(p.sessionId));
      if (!session) return {};
      // 协议 v1.0.1：flush=true（默认）跳过防抖强制落盘；UI 凭 sidecarMtime 熄灭指示灯，mock 不替 UI 判断
      const flush = p.flush !== false;
      const ph = this.data.photos.find((x) => x.id === session.photoId);
      let flushed = false;
      if (flush && ph && !this.conflictIds.has(ph.id)) {
        this.data.paramsById.set(ph.id, structuredClone(session.params));
        ph.hasEdits = true;
        ph.sidecarMtime = now();
        flushed = true;
      }
      if (session.flushTimer) clearTimeout(session.flushTimer);
      this.sessions.delete(session.sessionId);
      return { flushed, params: session.params, revision: session.revision, sidecarMtime: ph?.sidecarMtime ?? null };
    });
    H('edit.getSnapshots', (p) => this.snapshots.get(Number(p.id)) ?? []);
    H('edit.createSnapshot', (p) => {
      const id = Number(p.id);
      const snapshotId = this.snapshotSeq++;
      const list = this.snapshots.get(id) ?? [];
      list.push({ snapshotId, name: String(p.name ?? `快照 ${list.length + 1}`), createdAt: now(), params: structuredClone(this.getParams(id)) });
      this.snapshots.set(id, list);
      return { snapshotId };
    });
    H('edit.restoreSnapshot', (p) => {
      const id = Number(p.id);
      const snap = (this.snapshots.get(id) ?? []).find((s) => s.snapshotId === Number(p.snapshotId));
      if (!snap) throw new RpcShapeError(3001, '快照不存在');
      this.data.paramsById.set(id, structuredClone(snap.params));
      return { params: this.getParams(id) };
    });
    H('vcopy.create', (p) => {
      const id = Number(p.id);
      const vcopyId = this.vcopySeq++;
      const list = this.vcopies.get(id) ?? [];
      list.push({ vcopyId, name: String(p.name ?? `副本 ${list.length + 1}`), createdAt: now() });
      this.vcopies.set(id, list);
      return { vcopyId };
    });
    H('vcopy.list', (p) => this.vcopies.get(Number(p.id)) ?? []);
    H('presets.list', () => [
      { id: 1, name: '余晖斜刺', params: this.presetParams(0.45, 18, -70, 35, 30, 22) },
      { id: 2, name: '苍穹 · 山岚', params: this.presetParams(-0.15, 8, -40, 20, 18, -8) },
      { id: 3, name: '深夜深色', params: this.presetParams(-0.8, 22, -85, 12, 35, -18) },
      { id: 4, name: '建筑 · 透视', params: this.presetParams(0.1, 25, -55, 15, 12, 0) },
      { id: 5, name: '日出金顶', params: this.presetParams(0.35, 12, -65, 30, 22, 15) },
      { id: 6, name: '风光 · 基础', params: this.presetParams(0.2, 15, -50, 25, 20, 8) },
    ]);
    H('presets.apply', (p) => {
      const presetId = Number(p.presetId);
      const targets = (p.targetIds as number[]) ?? [];
      const presets: Record<number, EditParams> = {
        1: this.presetParams(0.45, 18, -70, 35, 30, 22),
        2: this.presetParams(-0.15, 8, -40, 20, 18, -8),
        3: this.presetParams(-0.8, 22, -85, 12, 35, -18),
        4: this.presetParams(0.1, 25, -55, 15, 12, 0),
        5: this.presetParams(0.35, 12, -65, 30, 22, 15),
        6: this.presetParams(0.2, 15, -50, 25, 20, 8),
      };
      const params = presets[presetId];
      if (!params) throw new RpcShapeError(3001, '预设不存在');
      for (const id of targets) {
        this.data.paramsById.set(id, structuredClone(params));
        const ph = this.photoById(id);
        ph.hasEdits = true;
        ph.sidecarMtime = now();
      }
      return { applied: targets.length };
    });

    // ---- 5.6 参数同步 ----
    H('edit.syncToSelection', (p) => this.syncParams(Number(p.sourceId), (p.targetIds as number[]) ?? []));
    H('edit.syncToSequence', (p) => {
      const seqId = Number(p.seqId);
      const targets = this.livePhotos()
        .filter((ph) => ph.seqId === seqId && ph.id !== Number(p.sourceId))
        .map((ph) => ph.id);
      return this.syncParams(Number(p.sourceId), targets);
    });

    // ---- 5.7 RAW 渲染 ----
    H('raw.decode', (p) => {
      const ph = this.photoById(Number(p.id));
      if (ph.isMissing) throw new RpcShapeError(1001, '原片离线，无法解码');
      const path = this.assetStore.preview(ph.id, ph.capturedAt, false, 4096, 2731);
      return { path, width: 6720, height: 4480 };
    });
    H('raw.getProxy', (p) => {
      const ph = this.photoById(Number(p.id));
      const path = this.assetStore.preview(ph.id, ph.capturedAt, false, 1920, 1280);
      return { path, width: 1920, height: 1280 };
    });
    // 协议 v1.0.1 §5.7：raw.selfTest（L1 判定：抽样解码 100% 成功且输出均值 ∈ [16,240]）。
    // mock 资产为结构合法 linear DNG，抽样范围内全部可解码；缺失照片计入 failures。
    H('raw.selfTest', (p) => {
      const sample = Math.max(1, Math.min(512, Number(p.sample ?? 32)));
      const ids = Array.isArray(p.ids) ? (p.ids as number[]).map(Number) : null;
      const picked = ids ?? this.livePhotos().slice(0, sample).map((x) => x.id);
      const failures = picked
        .map((id) => this.photoById(id))
        .filter((ph) => ph.isMissing)
        .map((ph) => ({ photoId: ph.id, error: 'file not found' }));
      const succeeded = picked.length - failures.length;
      return {
        ok: picked.length > 0 && failures.length === 0,
        sampled: picked.length,
        succeeded,
        decodeMsP50: 18,
        decodeMsP95: 42,
        failures,
      };
    });
    H('analysis.histogram', (p) => this.histogram(Number(p.id), p.edited === true));
    H('analysis.getExposedRange', (p) => {
      const params = this.getParams(Number(p.id));
      const over = Math.max(0, Math.min(12, params.exposure * 4 + params.whites * 0.05));
      const under = Math.max(0, Math.min(12, -params.exposure * 4 - params.blacks * 0.05));
      return { underexposedPct: Math.round(under * 10) / 10, overexposedPct: Math.round(over * 10) / 10 };
    });

    // ---- 5.8 导入 ----
    H('import.scanSource', (p) => {
      const sourcePath = String((p.source as { path?: string })?.path ?? 'E:/DCIM/100_PANA');
      return this.startJobRpc(
        'import.scan',
        2200,
        () => ({
          totalFiles: 1214,
          totalBytes: 42.6 * 1024 ** 3,
          byFormat: { RW2: 1198, JPG: 16 },
          duplicates: 3,
          rawWithJpg: 16,
          jpgOnly: 0,
          unreadable: [{ path: `${sourcePath}/105_PANA`, error: '源不可读（目录缺失或权限不足）' }],
          sampleFiles: ['P1060421.RW2', 'P1060422.RW2', 'P1060423.RW2', 'P1060424.RW2', 'P1060425.RW2'].map((fileName) => ({ fileName, bytes: 27.4 * 1024 * 1024, format: 'RW2' as const })),
        }),
      );
    });
    H('import.checkTarget', (p) => {
      const requiredBytes = Number(p.requiredBytes ?? 40 * 1024 ** 3);
      const freeBytes = 412 * 1024 ** 3;
      const ratio = requiredBytes / freeBytes;
      return { freeBytes, ratio: Math.round(ratio * 1000) / 1000, warning: requiredBytes * 1.1 > freeBytes };
    });
    H('import.start', (p) => {
      const folder = this.ensureFolderForImport();
      const total = 50;
      const target = String(p.target ?? 'D:/Lumina/Photos');
      void target;
      let jobIdRef = 0;
      const jobId = this.startJob(
        'import.copy',
        5000,
        () => ({ added: total - 3, skipped: 3, failed: 1, total }),
        {
          fileResultTick: (i, t) => {
            if (i > t - 1) return;
            const perTick = 3;
            for (let k = 0; k < perTick; k++) {
              const idx = (i - 1) * perTick + k + 1;
              if (idx > total) break;
              if (idx === 23) {
                this.notify('event.import.fileResult', { jobId: jobIdRef, path: `${folder.path}/dup_${idx}.RW2`, status: 'duplicate' });
              } else if (idx === 37) {
                this.notify('event.import.fileResult', { jobId: jobIdRef, path: `${folder.path}/bad_${idx}.RW2`, status: 'failed', error: 'MD5 校验失败' });
              } else {
                const ph = this.appendImportedPhoto(idx, folder);
                this.notify('event.import.fileResult', { jobId: jobIdRef, path: ph.filePath, status: 'ok', photoId: ph.id });
              }
            }
          },
        },
      );
      jobIdRef = jobId;
      return { jobId };
    });
    H('import.cancel', (p) => {
      this.cancelJob(Number(p.jobId));
      return {};
    });
    H('import.resolveMissingFolder', (p) => {
      const action = String(p.action);
      if (action === 'relocate') {
        const newPath = String(p.newPath ?? '');
        for (const ph of this.data.photos) {
          if (ph.filePath.startsWith(String(p.path))) ph.filePath = ph.filePath.replace(String(p.path), newPath);
        }
      }
      return { affected: action === 'relocate' ? 214 : action === 'backfill' ? 96 : 0 };
    });
    H('import.listRecentSources', () => this.recentSources);

    // ---- 5.9 存储健康 ----
    H('health.getStatus', () => {
      const alive = this.livePhotos();
      const missing = alive.filter((p) => p.isMissing).length;
      const corrupt = this.corruptIssues.size;
      return {
        verified: alive.length - missing,
        total: alive.length,
        missing,
        corrupt,
        backup: { state: 'ok', lastSuccessAt: now() - 5 * 3600_000, target: 'N:/NAS-Photos/Lumina' },
        md5: { percent: 97.7, running: [...this.jobs.values()].some((j) => j.kind.startsWith('health') && j.state === 'running') },
        abnormal: missing > 0 || corrupt > 0,
      };
    });
    H('health.scanFull', () =>
      this.startJobRpc('health.scan', 3000, () => ({ scanned: this.livePhotos().length, missing: this.livePhotos().filter((p) => p.isMissing).length })),
    );
    H('health.listIssues', (p) => {
      const kind = String(p.kind ?? 'missing');
      const page = (p.page as { offset: number; limit: number }) ?? { offset: 0, limit: 100 };
      let items: HealthIssueItem[];
      if (kind === 'missing') {
        items = this.livePhotos()
          .filter((ph) => ph.isMissing)
          .map((ph, i) => ({
            issueId: 10_000 + ph.id,
            photoId: ph.id,
            path: ph.filePath,
            lastKnown: ph.filePath,
            kind: 'missing' as const,
            detectedAt: ph.importedAt + 5 * DAY,
          }))
          .slice(0, 421);
      } else {
        items = [...this.corruptIssues.values()];
      }
      return { total: items.length, items: items.slice(page.offset, page.offset + page.limit) };
    });
    H('health.resolveIssue', (p) => {
      const issueId = Number(p.issueId);
      const action = String(p.action);
      if (issueId >= 10_000) {
        const ph = this.photoById(issueId - 10_000);
        if (action === 'relocate' && p.newPath) {
          ph.filePath = String(p.newPath);
          ph.isMissing = false;
          ph.health = 'ok';
        } else if (action === 'backfill') {
          ph.isMissing = false;
          ph.health = 'ok';
          ph.fileName = ph.fileName.replace(/\.RW2$/, '-已增强-降噪.dng');
        }
        // orphan：保留缺失标记，仅改状态
      } else {
        this.corruptIssues.delete(issueId);
      }
      return { resolved: 1 };
    });
    H('health.restoreFromBackup', (p) => {
      const photoId = Number(p.photoId);
      return this.startJobRpc('health.restore', 1500, () => {
        const ph = this.photoById(photoId);
        ph.isMissing = false;
        ph.health = 'ok';
        for (const [k, v] of this.corruptIssues) {
          if (v.photoId === photoId) this.corruptIssues.delete(k);
        }
        return { photoId };
      });
    });
    H('health.verifyPhotos', (p) => {
      const ids = (p.ids as number[] | undefined) ?? undefined;
      return this.startJobRpc('health.verify', 2500, () => ({ verified: ids ? ids.length : this.livePhotos().length, failed: 0 }));
    });
    H('health.getBackupConfig', () => ({ enabled: true, targetPath: 'N:/NAS-Photos/Lumina', timing: 'nightly' as const, lastRunAt: now() - 5 * 3600_000 }));

    // ---- 5.10 导出 ----
    H('export.createJob', (p) => {
      const photoIds = (p.photoIds as number[]) ?? [];
      const spec = (p.spec ?? {}) as { format?: string; namingTemplate?: string };
      const format = spec.format ?? 'jpg';
      const outputs: { photoId: number; path: string }[] = [];
      return this.startJobRpc('export.render', Math.min(8000, 600 + photoIds.length * 300), () => ({ outputs }), {
        fileResultTick: (i, t) => {
          const perTick = 2;
          for (let k = 0; k < perTick; k++) {
            const idx = (i - 1) * perTick + k;
            if (idx >= photoIds.length) break;
            const id = photoIds[idx];
            outputs.push({ photoId: id, path: this.assetStore.exportResult(id, format, 1) });
          }
          void t;
        },
      });
    });
    H('export.listJobs', (p) => {
      const state = p.state as string | undefined;
      return [...this.jobs.values()]
        .filter((j) => j.kind.startsWith('export') && (!state || j.state === state))
        .map((j) => ({ jobId: j.jobId, kind: j.kind, state: j.state, progress: j.progress, detail: j.detail, etaMs: j.etaMs }));
    });
    H('export.cancelJob', (p) => {
      this.cancelJob(Number(p.jobId));
      return {};
    });
    H('export.retryJob', (p) => {
      const old = this.jobs.get(Number(p.jobId));
      if (!old) throw new RpcShapeError(3001, '任务不存在');
      this.startJob(old.kind, 2000, () => ({ retried: true }));
      return {}; // 协议 §5.10：export.retryJob → {}
    });
    H('export.listPresets', () => this.exportPresets);
    H('export.savePreset', (p) => {
      const id = this.exportPresetSeq++;
      this.exportPresets.push({ id, name: String(p.name ?? `预设 ${id}`), spec: (p.spec ?? {}) as Record<string, unknown> });
      return { id };
    });

    // ---- 5.11 延时 ----
    H('timelapse.listSequences', () =>
      this.data.sequences.map((s) => {
        const job = [...this.jobs.values()].find((j) => j.kind === 'timelapse.render' && j.state === 'running');
        const analyzing = [...this.jobs.values()].some((j) => j.kind === 'timelapse.analyze' && j.state === 'running' && this.currentAnalyzeSeq === s.seqId);
        return {
          seqId: s.seqId,
          name: s.name,
          frameCount: s.frameCount,
          state: job ? 'rendering' : analyzing ? 'analyzing' : 'idle',
          renderProgress: job ? job.progress : 0,
        };
      }),
    );
    H('timelapse.getSequence', (p) => {
      const seq = this.data.sequences.find((s) => s.seqId === Number(p.seqId));
      if (!seq) throw new RpcShapeError(3001, '序列不存在');
      const frames = this.livePhotos()
        .filter((ph) => ph.seqId === seq.seqId)
        .slice(0, seq.frameCount)
        .map((ph, i) => ({ frameNo: i + 1, photoId: ph.id }));
      return {
        name: seq.name,
        frameCount: seq.frameCount,
        frames,
        exposureRamp: { keyframes: this.rampBySeq.get(seq.seqId) ?? [] },
        deflicker: this.deflickerBySeq.get(seq.seqId) ?? { analyzed: false },
        lrtCompatible: true,
      };
    });
    H('timelapse.analyzeFlicker', (p) => {
      this.currentAnalyzeSeq = Number(p.seqId);
      return this.startJobRpc('timelapse.analyze', 2000, () => ({ brightnessGainPct: 12.3, flickerIndex: 0.042 }));
    });
    H('timelapse.applyDeflicker', (p) => {
      const seqId = Number(p.seqId);
      return this.startJobRpc('timelapse.deflicker', 2500, () => {
        this.deflickerBySeq.set(seqId, { analyzed: true, brightnessGainPct: 12.3 });
        return { framesUpdated: this.data.sequences.find((s) => s.seqId === seqId)?.frameCount ?? 0 };
      });
    });
    H('timelapse.setExposureRamp', (p) => {
      this.rampBySeq.set(Number(p.seqId), (p.keyframes as RampKeyframe[]) ?? []);
      return {};
    });
    H('timelapse.previewFrame', (p) => {
      const seed = this.data.sequences.find((s) => s.seqId === Number(p.seqId)) ? 77 : 77;
      return { path: this.assetStore.framePreview(Number(p.seqId), Number(p.frameNo), seed) };
    });
    H('timelapse.render', (p) => {
      const codec = String(p.codec ?? 'h264');
      if (codec === 'prores') {
        // 红线：ProRes 不可达 → 5001 + 降级建议，绝不产出坏文件（§9-Q3）
        throw new RpcShapeError(5001, 'ProRes 编码器不可达（依赖待接入）', { alternative: 'h264' });
      }
      const seqId = Number(p.seqId);
      return this.startJobRpc('timelapse.render', 6000, () => ({ outputPath: `D:/LUMINA/build/runtime/exports/seq_${seqId}_${String(p.resolution)}.mp4` }));
    });
    H('timelapse.getRenderJobs', () =>
      [...this.jobs.values()]
        .filter((j) => j.kind.startsWith('timelapse'))
        .map((j) => ({ jobId: j.jobId, kind: j.kind, state: j.state, progress: j.progress, detail: j.detail, etaMs: j.etaMs })),
    );

    // ---- 5.12 风光专项 ----
    H('hdr.detectBracketGroups', (p) => {
      const groups: { groupId: number; photoIds: number[]; exposureStops: number }[] = [];
      const byFolder = new Map<number, PhotoDetail[]>();
      for (const ph of this.livePhotos()) {
        if (ph.format !== 'RW2' || ph.isMissing) continue;
        const list = byFolder.get(ph.folderId) ?? [];
        list.push(ph);
        byFolder.set(ph.folderId, list);
      }
      let groupId = 1;
      for (const [, list] of byFolder) {
        for (let i = 0; i + 6 < list.length && groups.length < 12; i += 6) {
          groups.push({ groupId: groupId++, photoIds: list.slice(i, i + 7).map((ph) => ph.id), exposureStops: 2 });
        }
      }
      void p;
      return groups;
    });
    H('hdr.composite', (p) => {
      const photoIds = (p.photoIds as number[]) ?? this.livePhotos().slice(0, 7).map((ph) => ph.id);
      void p;
      return this.startJobRpc('hdr.composite', 3500, () => {
        const folder = this.ensureFolderForImport();
        const ph = this.appendImportedPhoto(9000 + photoIds[0], folder);
        ph.format = 'DNG';
        ph.fileName = ph.fileName.replace(/\.RW2$/, '_HDR.dng');
        ph.filePath = ph.filePath.replace(/\.RW2$/, '_HDR.dng');
        return { photoId: ph.id };
      });
    });
    H('denoise.apply', (p) => this.startJobRpc('denoise.nlm', 2000, () => ({ photoId: Number(p.photoId), strength: p.strength })));
    H('denoise.preview', (p) => {
      const ph = this.photoById(Number(p.photoId));
      return { path: this.assetStore.denoiseSample(ph.id, ph.capturedAt) };
    });

    // ---- 5.13 地图 ----
    H('map.getLocations', (p) => {
      const zoom = Number(p.zoom ?? 6);
      const alive = this.livePhotos();
      const groups = new Map<string, { lat: number; lng: number; count: number; sampleId: number }>();
      for (const ph of alive) {
        const folder = this.folderOf(ph);
        if (!folder) continue;
        const loc = this.data.locations.find((l) => l.name === folder.locationName);
        if (!loc) continue;
        const e = groups.get(loc.name) ?? { lat: loc.lat, lng: loc.lng, count: 0, sampleId: ph.id };
        e.count++;
        groups.set(loc.name, e);
      }
      return [...groups.entries()].map(([name, g], i) => ({
        locationId: i + 1,
        name,
        lat: g.lat,
        lng: g.lng,
        count: g.count,
        cluster: zoom <= 7,
        children:
          zoom > 7
            ? [
                { name: `${name} · 晨光`, lat: g.lat + 0.01, lng: g.lng - 0.01, count: Math.round(g.count * 0.4), cluster: false },
                { name: `${name} · 日落`, lat: g.lat - 0.01, lng: g.lng + 0.01, count: Math.round(g.count * 0.6), cluster: false },
              ]
            : undefined,
      }));
    });
    H('map.getLocationDetail', (p) => {
      const idx = Number(p.locationId) - 1;
      const loc = this.data.locations[Math.max(0, Math.min(this.data.locations.length - 1, idx))];
      const folderNames = new Set(this.data.folders.filter((f) => f.locationName === loc.name).map((f) => f.id));
      const photos = this.livePhotos().filter((ph) => folderNames.has(ph.folderId));
      return {
        name: loc.name,
        lat: loc.lat,
        lng: loc.lng,
        altitude: loc.alt,
        count: photos.length,
        timeSpan: { from: photos[0]?.capturedAt ?? now(), to: photos[photos.length - 1]?.capturedAt ?? now() },
        favorite: this.locationFavorites.has(loc.name),
        samplePhotoId: photos[0]?.id ?? 1,
      };
    });
    H('map.setPhotoLocation', (p) => {
      const ids = (p.ids as number[]) ?? [];
      for (const id of ids) {
        const ph = this.photoById(id);
        ph.gpsLat = Number(p.lat);
        ph.gpsLng = Number(p.lng);
        ph.hasGps = true;
        ph.sidecarMtime = now(); // GPS 写 sidecar 的 exif: 命名空间（§9-Q1），原片零写入
      }
      // LocationWriteResult（零静默契约）：mock 全量成功，无降级警告
      return { updated: ids.length, sidecarFailed: 0, warnings: [] as string[] };
    });
    H('map.clearPhotoLocation', (p) => {
      const ids = (p.ids as number[]) ?? [];
      for (const id of ids) {
        const ph = this.photoById(id);
        ph.gpsLat = null;
        ph.gpsLng = null;
        ph.hasGps = false;
      }
      return { updated: ids.length, sidecarFailed: 0, warnings: [] as string[] };
    });
    H('map.listUnlocated', (p) => {
      const page = (p.page as { offset: number; limit: number }) ?? { offset: 0, limit: 200 };
      const unlocated = this.livePhotos().filter((ph) => !ph.hasGps);
      return { total: unlocated.length, items: unlocated.slice(page.offset, page.offset + page.limit).map((ph) => this.summary(ph)) };
    });
    H('map.toggleFavorite', (p) => {
      const idx = Number(p.locationId) - 1;
      const loc = this.data.locations[Math.max(0, Math.min(this.data.locations.length - 1, idx))];
      if (p.favorite === true) this.locationFavorites.add(loc.name);
      else this.locationFavorites.delete(loc.name);
      return {};
    });

    // ---- 5.14 Web 画廊 ----
    H('gallery.publish', (p) => {
      const albumId = Number(p.albumId);
      const existing = this.publishedGalleries.find((g) => g.albumId === albumId);
      if (existing) throw new RpcShapeError(6001, '该画册已发布', { albumId });
      const token = Math.random().toString(36).slice(2, 10);
      const staticDir = `D:/LUMINA/build/runtime/gallery/${token}`;
      const previewUrl = `http://localhost:51866/gallery/${token}/`;
      const expiresAt = now() + 30 * DAY;
      this.publishedGalleries.push({ albumId, token, previewUrl, expiresAt, password: String(p.password ?? ''), favorites: 0 });
      const itemCount = (this.albumItems.get(albumId) ?? []).length;
      this.notify('event.engine.log', { level: 'info', module: 'gallery', message: `published album=${albumId} token=${token} items=${itemCount}` });
      return { token, staticDir, expiresAt, previewUrl };
    });
    H('gallery.revoke', (p) => {
      const albumId = Number(p.albumId);
      this.publishedGalleries = this.publishedGalleries.filter((g) => g.albumId !== albumId);
      return {};
    });
    H('gallery.listPublished', () =>
      this.publishedGalleries.map((g) => {
        const itemCount = (this.albumItems.get(g.albumId) ?? []).length;
        return {
          albumId: g.albumId,
          token: g.token,
          previewUrl: g.previewUrl,
          expiresAt: g.expiresAt,
          stats: { photos: itemCount, favorites: g.favorites, completionRate: 96 },
        };
      }),
    );
    H('gallery.importFeedback', (p) => {
      const albumId = Number(p.albumId);
      const favorites = 8;
      const g = this.publishedGalleries.find((x) => x.albumId === albumId);
      if (g) g.favorites = favorites;
      const itemCount = (this.albumItems.get(albumId) ?? []).length || 42;
      return { favorites, completionRate: Math.round((favorites / itemCount) * 100) };
    });
    H('gallery.getFeedback', (p) => {
      const albumId = Number(p.albumId);
      const g = this.publishedGalleries.find((x) => x.albumId === albumId);
      const itemCount = (this.albumItems.get(albumId) ?? []).length || 42;
      const perPhoto = (this.albumItems.get(albumId) ?? []).map((photoId, i) => ({ photoId, favorited: i < (g?.favorites ?? 8) }));
      return { favoritesCount: g?.favorites ?? 8, completionRate: Math.round(((g?.favorites ?? 8) / itemCount) * 100), perPhoto };
    });

    // ---- 5.15 模拟图库 ----
    H('mock.generate', () =>
      this.startJobRpc('mock.generate', 1200, () => {
        const photos = this.data.photos;
        const live = photos.filter((p) => !p.isMissing);
        return {
          dbPhotos: photos.length,
          imageFiles: live.length,
          sidecars: photos.filter((p) => p.hasEdits).length,
          missing: photos.length - live.length,
          diskBytes: live.reduce((acc, p) => acc + p.fileSize, 0),
          durationMs: 1180,
          dngSelfTest: { ok: true, sampled: 32, succeeded: 32, decodeMsP50: 18 },
          readmePath: `${this.settings.homeDir}/cache/mock-library-readme.md`,
        };
      }),
    );
    H('mock.status', () => ({
      generated: true,
      libraryRoot: 'D:/Lumina/',
      dbPhotoCount: this.data.photos.length,
      assetFormat: 'linear-dng',
      lastSelfTest: { ok: true, sampled: 32, succeeded: 32, decodeMsP50: 18 },
    }));

    // ---- Mock 专属调试方法（capabilities.mock=true 时 UI 才展示入口；非协议方法） ----
    H('mock.debug.simulateExternalEdit', (p) => {
      this.conflictIds.add(Number(p.id));
      return { ok: true };
    });
    H('mock.debug.simulateCorruption', (p) => {
      const ph = this.photoById(Number(p.id));
      const issueId = this.issueSeq++;
      this.corruptIssues.set(issueId, {
        issueId,
        photoId: ph.id,
        path: ph.filePath,
        lastKnown: ph.filePath,
        kind: 'corrupt',
        detectedAt: now(),
      });
      this.notify('event.health.issue', { issueId, photoId: ph.id, kind: 'corrupt', path: ph.filePath });
      return { issueId };
    });
  }

  private currentAnalyzeSeq = 0;

  private allFolders(): MockFolder[] {
    return [...this.data.folders, ...this.extraFolders.values()];
  }

  private summary(p: PhotoDetail) {
    const { md5, fileSize, sidecarPath, sidecarMtime, whiteBalance, folderId, rootId, keywords, description, seqInfo, health, backup, ...s } = p;
    void md5;
    void fileSize;
    void sidecarPath;
    void sidecarMtime;
    void whiteBalance;
    void keywords;
    void description;
    void seqInfo;
    void health;
    void backup;
    // folderId/rootId 不属于 Summary，但 engine 需要它们做树定位；协议规定 Summary 不含 → 严格删除
    void folderId;
    void rootId;
    return s;
  }

  private detail(p: PhotoDetail) {
    return { ...p };
  }

  private presetParams(exposure: number, contrast: number, highlights: number, shadows: number, dehaze: number, vibrance: number): EditParams {
    const p = defaultParams();
    p.exposure = exposure;
    p.contrast = contrast;
    p.highlights = highlights;
    p.shadows = shadows;
    p.dehaze = dehaze;
    p.vibrance = vibrance;
    p.temperature = 5400;
    return p;
  }

  private syncParams(sourceId: number, targetIds: number[]): { updated: number } {
    const source = this.getParams(sourceId);
    for (const id of targetIds) {
      this.data.paramsById.set(id, structuredClone(source));
      const ph = this.photoById(id);
      ph.hasEdits = true;
      ph.sidecarMtime = now();
    }
    this.notify('event.engine.log', { level: 'info', module: 'edit', message: `sync ${targetIds.length} photos from ${sourceId}` });
    return { updated: targetIds.length };
  }

  /** 直方图替身：由参数驱动的确定性分布（mock 模式下与真实解码无关） */
  private histogram(photoId: number, edited: boolean): { r: number[]; g: number[]; b: number[] } {
    const params = edited ? this.getParams(photoId) : defaultParams();
    const shift = params.exposure * 30;
    const mk = (centerBase: number, spread: number, tintShift: number): number[] => {
      const arr = new Array<number>(256).fill(0);
      const center = Math.max(10, Math.min(245, centerBase + shift + tintShift));
      for (let i = 0; i < 256; i++) {
        const d = (i - center) / spread;
        arr[i] = Math.round(1000 * Math.exp(-d * d) + 120 * Math.exp(-Math.pow((i - 20) / 12, 2)) + 60 * Math.exp(-Math.pow((i - 250) / 10, 2)));
      }
      return arr;
    };
    const wbShift = ((params.temperature ?? 5500) - 5500) / 60;
    return {
      r: mk(120, 34, wbShift),
      g: mk(128, 32, 0),
      b: mk(118, 36, -wbShift),
    };
  }
}

/** MockTransport：与真引擎传输层同构（帧收发 → 信封），供 EngineBridge 无感切换 */
export class MockTransport {
  private engine: MockEngine;
  constructor(homeDir: string) {
    this.engine = new MockEngine(homeDir);
  }
  onNotification(cb: (n: EngineNotification) => void): void {
    this.engine.onNotification(cb);
  }
  async call(req: { id: number; method: string; params: unknown }): Promise<{ result?: unknown; error?: { code: number; message: string; data?: unknown } }> {
    // 微延迟模拟真实 IPC 往返
    await new Promise((r) => setTimeout(r, 4));
    return this.engine.dispatch(req as { id: number; method: string; params: unknown });
  }
  dispose(): void {
    /* no-op */
  }
}
