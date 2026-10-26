/**
 * Mock 图库数据生成器 —— 按 ipc-protocol.md §5.15 与 PRD §2 基准分布生成 18,192 张模拟照片。
 * 元数据分布（严格复刻调研基准）：
 *   RW2 16,470 / JPG 1,387 / DNG 273 / ARW 15 / MP4 45 / PSD 1 / PNG 1
 *   5 根目录 / 79 文件夹 / 9 个月（2025-12 → 2026-08）
 *   缺失 421（集中在 7 个文件夹）/ 已调整 4,347（其中 83.8% 含局部蒙版）
 *   评分 2★ 1,365 / 3★ 1 / 4★ 57 / 5★ 0；GPS 137（0.75%）
 * 全部确定性（固定种子），多次启动结果一致。
 */

import type {
  PhotoDetail,
  PhotoFormat,
  PhotoSummary,
  Filter,
  FolderNode,
  EditParams,
  MaskLayer,
  Sort,
  Page,
  QueryResult,
  LibraryStats,
  TimelineBucket,
  FacetsResult,
} from '../../src/api/protocol';

// ---------- 确定性 PRNG（mulberry32） ----------

export function mulberry32(seed: number): () => number {
  let a = seed >>> 0;
  return () => {
    a |= 0;
    a = (a + 0x6d2b79f5) | 0;
    let t = Math.imul(a ^ (a >>> 15), 1 | a);
    t = (t + Math.imul(t ^ (t >>> 7), 61 | t)) ^ t;
    return ((t ^ (t >>> 14)) >>> 0) / 4294967296;
  };
}

const MONTHS = ['2025-12', '2026-01', '2026-02', '2026-03', '2026-04', '2026-05', '2026-06', '2026-07', '2026-08'];
const MONTH_COUNTS = [1305, 1602, 1744, 1888, 2103, 2231, 2450, 2607, 2262]; // 合计 18,192

const LOCATIONS = [
  { name: '松赞林寺', lat: 27.8331, lng: 99.7238, alt: 3300, region: '云南 · 香格里拉' },
  { name: '梅里雪山', lat: 28.4428, lng: 98.7629, alt: 3400, region: '云南 · 德钦' },
  { name: '牛背山', lat: 29.7337, lng: 102.6495, alt: 3666, region: '四川 · 雅安' },
  { name: '亚丁', lat: 28.3722, lng: 100.6636, alt: 4100, region: '四川 · 稻城' },
  { name: '新都桥', lat: 30.0427, lng: 101.4899, alt: 3460, region: '四川 · 康定' },
  { name: '色达', lat: 32.2685, lng: 100.3289, alt: 3900, region: '四川 · 甘孜' },
  { name: '霞浦', lat: 26.8862, lng: 120.0091, alt: 10, region: '福建 · 宁德' },
  { name: '元阳梯田', lat: 23.0962, lng: 102.7756, alt: 1900, region: '云南 · 红河' },
  { name: '坝上草原', lat: 42.4432, lng: 117.2421, alt: 1500, region: '河北 · 承德' },
  { name: '滇池', lat: 24.9512, lng: 102.6626, alt: 1886, region: '云南 · 昆明' },
];

const ROOT_DEFS = [
  { path: 'F:/2025', label: 'F:/2025' },
  { path: 'F:/照片', label: 'F:/照片' },
  { path: 'X:/2026', label: 'X:/2026' },
  { path: 'Y:/2026/2026', label: 'Y:/2026/2026' },
  { path: 'F:/ljf.it', label: 'F:/ljf.it' },
];

const CAMERAS = ['DC-S5M2', 'DC-S5M2', 'DC-S5M2', 'ILCE-7RM4', 'EOS R5'];
const LENSES = [
  'LUMIX S 70-300mm',
  'LUMIX S 24-60mm',
  'Sigma 20-200mm F4-7.1',
  'MJ',
  'FE 70-200mm F2.8',
  'VILTROX 16mm F1.8',
];
const LENS_WEIGHTS = [0.3, 0.25, 0.15, 0.042, 0.15, 0.108];

export interface MockFolder {
  id: number;
  rootId: number;
  path: string;
  name: string;
  locationName: string;
  missing: number;
  photoCount: number;
}

export interface MockSequence {
  seqId: number;
  name: string;
  frameCount: number;
}

export interface MockLibraryData {
  photos: PhotoDetail[];
  folders: MockFolder[];
  locations: typeof LOCATIONS;
  sequences: MockSequence[];
  /** 已调整照片的 sidecar 参数（编辑存量 4,347） */
  paramsById: Map<number, EditParams>;
  /** 缺失照片 id → 原路径 */
  missingIds: Set<number>;
}

// ---------- 格式配额 ----------
const FORMAT_QUOTA: [PhotoFormat, number][] = [
  ['RW2', 16470],
  ['JPG', 1387],
  ['DNG', 273],
  ['ARW', 15],
  ['MP4', 45],
  ['PSD', 1],
  ['PNG', 1],
];

function pickWeighted(rng: () => number, values: string[], weights: number[]): string {
  const r = rng();
  let acc = 0;
  for (let i = 0; i < values.length; i++) {
    acc += weights[i];
    if (r < acc) return values[i];
  }
  return values[values.length - 1];
}

function daysInMonth(ym: string): number {
  const [y, m] = ym.split('-').map(Number);
  return new Date(y, m, 0).getDate();
}

function monthStartMs(ym: string): number {
  const [y, m] = ym.split('-').map(Number);
  return Date.UTC(y, m - 1, 1);
}

// ---------- 生成 ----------

export function generateMockLibrary(seed = 20261022): MockLibraryData {
  const rng = mulberry32(seed);
  const photos: PhotoDetail[] = [];
  const folders: MockFolder[] = [];
  const paramsById = new Map<number, EditParams>();
  const missingIds = new Set<number>();

  // 1. 文件夹：9 个月 × [8,9,9,9,9,9,9,9,8] = 79 个
  const folderPerMonth = [8, 9, 9, 9, 9, 9, 9, 9, 8];
  let folderId = 1;
  const folderMonths: { folder: MockFolder; month: string }[] = [];
  MONTHS.forEach((month, mi) => {
    for (let f = 0; f < folderPerMonth[mi]; f++) {
      const loc = LOCATIONS[Math.floor(rng() * LOCATIONS.length)];
      const day = 1 + Math.floor(rng() * 26);
      const name = `${month}-${String(day).padStart(2, '0')}_${loc.name}`;
      const rootIdx = Math.floor(rng() * ROOT_DEFS.length);
      const root = ROOT_DEFS[rootIdx];
      const folder: MockFolder = {
        id: folderId++,
        rootId: rootIdx + 1,
        path: `${root.path}/${name}`,
        name,
        locationName: loc.name,
        missing: 0,
        photoCount: 0,
      };
      folders.push(folder);
      folderMonths.push({ folder, month });
    }
  });

  // 3. 格式配额指针
  const fmtRemaining = FORMAT_QUOTA.map(([, n]) => n);
  let fmtTotalRemaining = 18192;

  const editedIds: number[] = [];
  let id = 1;
  folderMonths.forEach(({ folder, month }, fi) => {
    // 该文件夹的照片数：按月份配额近似均分
    const mi = MONTHS.indexOf(month);
    const count = Math.round(MONTH_COUNTS[mi] / folderPerMonth[mi]);
    const day = Number(folder.name.slice(8, 10));
    const baseMs = monthStartMs(month) + (day - 1) * 86400_000;
    for (let p = 0; p < count && fmtTotalRemaining > 0; p++) {
      // 格式按剩余配额比例抽取，保证全局精确命中
      const r = rng() * fmtTotalRemaining;
      let acc = 0;
      let fi2 = 0;
      for (let k = 0; k < FORMAT_QUOTA.length; k++) {
        acc += fmtRemaining[k];
        if (r < acc) {
          fi2 = k;
          break;
        }
      }
      const format = FORMAT_QUOTA[fi2][0];
      fmtRemaining[fi2]--;
      fmtTotalRemaining--;

      const isVideo = format === 'MP4';
      const hour = isVideo ? 6 + Math.floor(rng() * 4) : Math.floor(rng() * 24);
      const minute = Math.floor(rng() * 60);
      const capturedAt = baseMs + hour * 3600_000 + minute * 60_000;
      const loc = LOCATIONS.find((l) => l.name === folder.locationName) ?? LOCATIONS[0];
      const fileName = `${folder.name}_${String(p + 1).padStart(4, '0')}.${format.toLowerCase()}`;
      const portrait = rng() < 0.18;
      const width = format === 'MP4' ? 3840 : portrait ? 4480 : 6720;
      const height = format === 'MP4' ? 2160 : portrait ? 6720 : 4480;

      const photo: PhotoDetail = {
        id,
        fileName,
        filePath: `${folder.path}/${fileName}`,
        format,
        width,
        height,
        capturedAt,
        importedAt: capturedAt + 3 * 86400_000,
        camera: pickWeighted(rng, CAMERAS, [0.36, 0.28, 0.282, 0.048, 0.03]),
        lens: pickWeighted(rng, LENSES, LENS_WEIGHTS),
        iso: [100, 200, 400, 800, 1600, 3200, 6400, 12800][Math.floor(rng() * 8)],
        aperture: ['f/2.8', 'f/4', 'f/5.6', 'f/8', 'f/11', 'f/16'][Math.floor(rng() * 6)],
        shutter: ['1/8000s', '1/1000s', '1/250s', '1/125s', '1/60s', '1/15s', '1/4s', '30s'][Math.floor(rng() * 8)],
        focalLength: `${[16, 24, 35, 50, 70, 135, 200, 300][Math.floor(rng() * 8)]}mm`,
        gpsLat: null,
        gpsLng: null,
        rating: 0,
        pick: 'none',
        colorLabel: null,
        hasEdits: false,
        hasGps: false,
        isMissing: false,
        isVideo,
        pairKind: 'single',
        thumbState: 'pending',
        thumbPath: null,
        seqId: null,
        md5: null,
        fileSize: format === 'RW2' ? 26_000_000 + Math.floor(rng() * 6_000_000) : 4_000_000 + Math.floor(rng() * 4_000_000),
        sidecarPath: null,
        sidecarMtime: null,
        whiteBalance: rng() < 0.786 ? 'asShot' : 'custom',
        folderId: folder.id,
        rootId: folder.rootId,
        keywords: [],
        description: '',
        seqInfo: null,
        health: 'unchecked',
        backup: { backedUp: true, backupPath: `${folder.path.replace(/^(\w):/, '$1_bak:')}/${fileName}`, verifiedAt: capturedAt },
      };
      photos.push(photo);
      folder.photoCount++;
      id++;
    }
  });

  // 4. 评分分布：2★ 1,365 / 3★ 1 / 4★ 57
  const ratingTargets: [number, number][] = [
    [2, 1365],
    [3, 1],
    [4, 57],
  ];
  for (const [rating, n] of ratingTargets) {
    let assigned = 0;
    while (assigned < n) {
      const idx = Math.floor(rng() * photos.length);
      if (photos[idx].rating === 0) {
        photos[idx].rating = rating as PhotoDetail['rating'];
        if (rating === 4) photos[idx].pick = 'pick';
        assigned++;
      }
    }
  }

  // 5. GPS 137 张
  for (let g = 0; g < 137; g++) {
    const idx = Math.floor(rng() * photos.length);
    const loc = LOCATIONS[Math.floor(rng() * LOCATIONS.length)];
    photos[idx].gpsLat = loc.lat + (rng() - 0.5) * 0.02;
    photos[idx].gpsLng = loc.lng + (rng() - 0.5) * 0.02;
    photos[idx].hasGps = true;
  }

  // 6. 缺失 421 张（集中 7 文件夹，精确命中基准数）
  const missingFolderIdx = new Set<number>();
  while (missingFolderIdx.size < 7) {
    missingFolderIdx.add(Math.floor(rng() * folders.length));
  }
  const missingPool: PhotoDetail[] = [];
  for (const fi of missingFolderIdx) {
    for (const p of photos) {
      if (p.folderId === folders[fi].id) missingPool.push(p);
    }
  }
  let missingAssigned = 0;
  while (missingAssigned < 421 && missingPool.length > 0) {
    const pick = missingPool.splice(Math.floor(rng() * missingPool.length), 1)[0];
    if (!pick.isMissing) {
      pick.isMissing = true;
      pick.health = 'missing';
      missingIds.add(pick.id);
      missingAssigned++;
      const folder = folders.find((f) => f.id === pick.folderId);
      if (folder) folder.missing++;
    }
  }

  // 7. 已调整 4,347（精确命中；其中 83.8% 含局部蒙版）
  const editedTarget = 4347;
  const maskRatio = 0.838;
  const editPool = photos.filter((p) => !p.isMissing);
  for (let e = 0; e < editedTarget; e++) {
    if (editPool.length === 0) break;
    const pick = editPool.splice(Math.floor(rng() * editPool.length), 1)[0];
    pick.hasEdits = true;
    pick.sidecarPath = `${pick.filePath}.xmp`;
    pick.sidecarMtime = pick.importedAt + 3600_000;
    pick.health = 'ok';
    editedIds.push(pick.id);
  }
  const withMask = Math.round(editedTarget * maskRatio);
  for (let m = 0; m < withMask; m++) {
    const pid = editedIds[m];
    paramsById.set(pid, buildEditedParams(mulberry32(pid)));
  }
  for (let m = withMask; m < editedTarget; m++) {
    const pid = editedIds[m];
    paramsById.set(pid, buildEditedParams(mulberry32(pid), true));
  }

  // 8. 延时序列 3 组
  const sequences: MockSequence[] = [
    { seqId: 1, name: '松赞林寺 · 晨雾', frameCount: 480 },
    { seqId: 2, name: '梅里 · 日照金山', frameCount: 300 },
    { seqId: 3, name: '牛背山 · 云海', frameCount: 150 },
  ];
  const seqCandidates = photos.filter((p) => p.format === 'RW2' && !p.isMissing);
  let cursor = 0;
  for (const seq of sequences) {
    for (let f = 0; f < seq.frameCount && cursor < seqCandidates.length; f++) {
      const photo = seqCandidates[cursor++];
      photo.seqId = seq.seqId;
      photo.seqInfo = { seqId: seq.seqId, name: seq.name, frameNo: f + 1, frameCount: seq.frameCount };
    }
  }

  // 9. RAW+JPG 配对（前 1,387 张 RW2 标记伴随 JPG）
  let paired = 0;
  for (const photo of photos) {
    if (paired >= 1387) break;
    if (photo.format === 'RW2' && photo.pairKind === 'single') {
      photo.pairKind = 'raw+jpg';
      paired++;
    }
  }

  // capturedAt 排序 + id 重排（默认视图按拍摄时间升序）
  photos.sort((a, b) => a.capturedAt - b.capturedAt);
  photos.forEach((p, i) => {
    p.id = i + 1;
  });

  return { photos, folders, locations: LOCATIONS, sequences, paramsById, missingIds };
}

/** 生成一套「已调整」参数（含或不含蒙版） */
function buildEditedParams(rng: () => number, noMask = false): EditParams {
  const curve: { x: number; y: number }[] = [
    { x: 0, y: 0 },
    { x: 64, y: 58 + Math.floor(rng() * 8) },
    { x: 128, y: 128 + Math.floor(rng() * 10) - 5 },
    { x: 192, y: 198 + Math.floor(rng() * 6) },
    { x: 255, y: 255 },
  ];
  const params: EditParams = {
    processVersion: '15.4',
    whiteBalance: rng() < 0.6 ? 'asShot' : 'custom',
    temperature: 5200 + Math.floor(rng() * 1800),
    tint: Math.floor(rng() * 24) - 12,
    exposure: Math.round((rng() * 1.6 - 0.5) * 100) / 100,
    contrast: Math.floor(rng() * 40) - 10,
    highlights: -Math.floor(rng() * 80),
    shadows: Math.floor(rng() * 50),
    whites: Math.floor(rng() * 30) - 10,
    blacks: -Math.floor(rng() * 30),
    clarity: Math.floor(rng() * 30),
    texture: Math.floor(rng() * 20),
    dehaze: Math.floor(rng() * 25),
    vibrance: Math.floor(rng() * 35),
    saturation: Math.floor(rng() * 10) - 5,
    toneCurveRGB: curve,
    hsl: {
      red: { hue: 0, sat: 0, lum: 0 },
      orange: { hue: 0, sat: 0, lum: 0 },
      yellow: { hue: Math.floor(rng() * 8) - 4, sat: Math.floor(rng() * 15), lum: 0 },
      green: { hue: Math.floor(rng() * 10) - 5, sat: Math.floor(rng() * 12), lum: Math.floor(rng() * 10) - 5 },
      aqua: { hue: 0, sat: 0, lum: 0 },
      blue: { hue: Math.floor(rng() * 6) - 3, sat: Math.floor(rng() * 20), lum: 0 },
      purple: { hue: 0, sat: 0, lum: 0 },
      magenta: { hue: 0, sat: 0, lum: 0 },
    },
    colorGrade: {
      shadowHue: 220,
      shadowSat: Math.floor(rng() * 12),
      midtoneHue: 40,
      midtoneSat: Math.floor(rng() * 8),
      highlightHue: 45,
      highlightSat: Math.floor(rng() * 10),
      blend: 50,
      balance: 0,
    },
    calibration: { shadowTint: 0, redHue: 0, redSat: 0, greenHue: 0, greenSat: 0, blueHue: 0, blueSat: 0 },
    detail: {
      sharpness: 40 + Math.floor(rng() * 20),
      sharpenRadius: 1.0,
      sharpenDetail: 25,
      sharpenMasking: 10,
      luminanceSmoothing: 5,
      colorNR: 20,
      colorNRDetail: 50,
    },
    optics: {
      lensProfileEnable: true,
      chromaticAberration: 0,
      manualDistortion: 0,
      vignetteAmount: -Math.floor(rng() * 15),
      vignetteMidpoint: 50,
      vignetteFeather: 50,
      grainAmount: Math.floor(rng() * 10),
      grainFrequency: 50,
      grainSize: 25,
    },
    geometry: {
      hasCrop: false,
      perspective: { vertical: 0, horizontal: 0, rotate: 0, scale: 100, aspect: 100, x: 0, y: 0 },
    },
    masks: [],
  };
  if (!noMask) {
    const n = 1 + Math.floor(rng() * 3);
    for (let i = 0; i < n; i++) {
      const type = rng() < 0.55 ? 'linear' : rng() < 0.7 ? 'radial' : 'brush';
      const mask: MaskLayer = {
        id: `m_${Math.floor(rng() * 1e9).toString(36)}`,
        type,
        enabled: true,
        opacity: 100,
        combine: 'replace',
        amount: type === 'radial' ? -Math.floor(rng() * 60) : Math.floor(rng() * 80) - 40,
        geometry:
          type === 'linear'
            ? { x1: Math.round(rng() * 100) / 100, y1: 0, x2: Math.round(rng() * 100) / 100, y2: 1 }
            : type === 'radial'
              ? { cx: Math.round(rng() * 100) / 100, cy: Math.round(rng() * 100) / 100, radius: 0.2 + rng() * 0.3, ratio: 1 }
              : undefined,
        strokes:
          type === 'brush'
            ? Array.from({ length: 8 }, () => ({
                x: Math.round(rng() * 100) / 100,
                y: Math.round(rng() * 100) / 100,
                radius: 0.05 + rng() * 0.1,
                flow: 80,
                density: 100,
                erase: false,
              }))
            : undefined,
        local: {
          exposure: Math.round((rng() * 1.4 - 0.7) * 100) / 100,
          contrast: Math.floor(rng() * 20) - 10,
          highlights: -Math.floor(rng() * 60),
          shadows: Math.floor(rng() * 30),
          temperature: Math.floor(rng() * 20) - 10,
          tint: 0,
          saturation: Math.floor(rng() * 16) - 8,
          clarity: Math.floor(rng() * 20),
          dehaze: Math.floor(rng() * 15),
        },
      };
      params.masks.push(mask);
    }
  }
  return params;
}

// ---------- 查询执行（Filter AND / Sort / Page） ----------

export function matchesFilter(p: PhotoSummary, filter: Filter | undefined, folders: MockFolder[], data?: MockLibraryData): boolean {
  if (!filter) return true;
  if (filter.folderIds && filter.folderIds.length > 0 && !filter.folderIds.includes((p as unknown as PhotoDetail).folderId)) return false;
  if (filter.rootIds && filter.rootIds.length > 0 && !filter.rootIds.includes((p as unknown as PhotoDetail).rootId)) return false;
  if (filter.formats && filter.formats.length > 0 && !filter.formats.includes(p.format)) return false;
  if (filter.cameras && filter.cameras.length > 0 && !filter.cameras.includes(p.camera)) return false;
  if (filter.lenses && filter.lenses.length > 0 && !filter.lenses.includes(p.lens)) return false;
  if (filter.minRating !== undefined && p.rating < filter.minRating) return false;
  if (filter.pick !== undefined && filter.pick !== 'none' && p.pick !== filter.pick) return false;
  if (filter.located !== undefined && p.hasGps !== filter.located) return false;
  if (filter.edited !== undefined && p.hasEdits !== filter.edited) return false;
  if (filter.missing !== undefined && p.isMissing !== filter.missing) return false;
  if (filter.isVideo !== undefined && p.isVideo !== filter.isVideo) return false;
  if (filter.dateFrom !== undefined && p.capturedAt < filter.dateFrom) return false;
  if (filter.dateTo !== undefined && p.capturedAt > filter.dateTo) return false;
  if (filter.seqId !== undefined && p.seqId !== filter.seqId) return false;
  if (filter.locationIds && filter.locationIds.length > 0) {
    const folder = folders.find((f) => f.id === (p as unknown as PhotoDetail).folderId);
    if (!folder || !filter.locationIds.includes(folder.id)) return false;
  }
  void data;
  return true;
}

export function runQuery(
  photos: PhotoDetail[],
  deleted: Set<number>,
  filter: Filter | undefined,
  sort: Sort | undefined,
  page: Page,
  folders: MockFolder[],
): QueryResult {
  const sortField = sort?.field ?? 'capturedAt';
  const order = sort?.order ?? 'asc';
  const dir = order === 'asc' ? 1 : -1;
  const alive = photos.filter((p) => !deleted.has(p.id) && matchesFilter(p, filter, folders));
  alive.sort((a, b) => {
    let r = 0;
    if (sortField === 'capturedAt') r = a.capturedAt - b.capturedAt;
    else if (sortField === 'importedAt') r = a.importedAt - b.importedAt;
    else if (sortField === 'fileName') r = a.fileName.localeCompare(b.fileName, 'zh-Hans-CN');
    else if (sortField === 'rating') r = a.rating - b.rating;
    return r * dir;
  });
  const items = alive.slice(page.offset, page.offset + page.limit).map((p) => toSummary(p));
  return { total: alive.length, items };
}

export function toSummary(p: PhotoDetail): PhotoSummary {
  const { md5, fileSize, sidecarPath, sidecarMtime, whiteBalance, folderId, rootId, keywords, description, seqInfo, health, backup, ...summary } = p;
  void md5;
  void fileSize;
  void sidecarPath;
  void sidecarMtime;
  void whiteBalance;
  void folderId;
  void rootId;
  void keywords;
  void description;
  void seqInfo;
  void health;
  void backup;
  return summary;
}

export function buildStats(photos: PhotoDetail[], deleted: Set<number>): LibraryStats {
  const alive = photos.filter((p) => !deleted.has(p.id));
  const byFormat: Record<string, number> = {};
  const monthMap = new Map<string, number>();
  let totalBytes = 0;
  let missing = 0;
  let edited = 0;
  for (const p of alive) {
    byFormat[p.format] = (byFormat[p.format] ?? 0) + 1;
    const ym = new Date(p.capturedAt).toISOString().slice(0, 7);
    monthMap.set(ym, (monthMap.get(ym) ?? 0) + 1);
    totalBytes += p.fileSize;
    if (p.isMissing) missing++;
    if (p.hasEdits) edited++;
  }
  const months = MONTHS.filter((m) => monthMap.has(m)).map((m) => ({ month: m, count: monthMap.get(m) ?? 0 }));
  return {
    totalPhotos: alive.length,
    totalBytes,
    verified: alive.length - missing,
    missing,
    edited,
    byFormat,
    months,
  };
}

export function buildTimeline(photos: PhotoDetail[], deleted: Set<number>): TimelineBucket[] {
  const map = new Map<string, { count: number; firstPhotoId: number }>();
  for (const p of photos) {
    if (deleted.has(p.id)) continue;
    const ym = new Date(p.capturedAt).toISOString().slice(0, 7);
    const e = map.get(ym);
    if (e) {
      e.count++;
      e.firstPhotoId = Math.min(e.firstPhotoId, p.id);
    } else {
      map.set(ym, { count: 1, firstPhotoId: p.id });
    }
  }
  return MONTHS.filter((m) => map.has(m)).map((m) => ({ month: m, count: map.get(m)!.count, firstPhotoId: map.get(m)!.firstPhotoId }));
}

export function buildFacets(photos: PhotoDetail[], deleted: Set<number>, filter: Filter | undefined, folders: MockFolder[]): FacetsResult {
  const alive = photos.filter((p) => !deleted.has(p.id) && matchesFilter(p, filter, folders));
  const counter = <K extends string>(keyFn: (p: PhotoDetail) => string | null) => {
    const m = new Map<string, number>();
    for (const p of alive) {
      const k = keyFn(p);
      if (k !== null && k !== '') m.set(k, (m.get(k) ?? 0) + 1);
    }
    return [...m.entries()].map(([value, count]) => ({ value, count })).sort((a, b) => b.count - a.count);
  };
  const ratings = new Map<string, number>();
  for (const p of alive) {
    const k = String(p.rating);
    ratings.set(k, (ratings.get(k) ?? 0) + 1);
  }
  return {
    cameras: counter((p) => p.camera),
    lenses: counter((p) => p.lens),
    formats: counter((p) => p.format),
    ratings: [...ratings.entries()].map(([value, count]) => ({ value, count })).sort((a, b) => Number(a.value) - Number(b.value)),
    locations: counter((p) => folders.find((f) => f.id === p.folderId)?.locationName ?? null),
  };
}

/** 文件夹树：root → folders（无嵌套子层，日期文件夹直接挂根下） */
export function buildFolderTree(photos: PhotoDetail[], deleted: Set<number>, folders: MockFolder[]): FolderNode[] {
  const countByFolder = new Map<number, { count: number; missing: number }>();
  for (const p of photos) {
    if (deleted.has(p.id)) continue;
    const e = countByFolder.get(p.folderId) ?? { count: 0, missing: 0 };
    e.count++;
    if (p.isMissing) e.missing++;
    countByFolder.set(p.folderId, e);
  }
  const roots = ROOT_DEFS.map((r, i) => ({
    id: -(i + 1),
    rootId: i + 1,
    path: r.path,
    name: r.label,
    count: 0,
    missing: 0,
    children: [] as FolderNode[],
  }));
  for (const f of folders) {
    const c = countByFolder.get(f.id) ?? { count: 0, missing: 0 };
    const root = roots[f.rootId - 1];
    root.count += c.count;
    root.missing += c.missing;
    root.children.push({
      id: f.id,
      rootId: f.rootId,
      path: f.path,
      name: f.name,
      count: c.count,
      missing: c.missing,
      children: [],
    });
  }
  return roots;
}

export { MONTHS, LOCATIONS, ROOT_DEFS, FORMAT_QUOTA };
